// A C interface to emulated DSP56367s (Gearmulator's dsp56300 library), seen
// from the host CPU through their HDI08 ports, for tools/firmware/g2hostemu.py
// (loaded with ctypes). The host emulator runs the G2's ColdFire OS; every byte
// it reads or writes in a DSP's host-port window comes here.
//
// Each DSP runs freely on its own thread, as in Gearmulator: the interpreter
// (the JIT did not leave a `brset #TFS,x:SAISR,*` poll on aarch64 in our test,
// and the JIT may not share a thread with Unicorn's own JIT anyway). The host
// side only touches the library's thread-safe parts of the HDI08: the receive
// queue, the host flags, host commands and the transmit queue.
//
// Host-side registers (8-bit bus, A0-A2):
//   0 ICR (RREQ 0x01, TREQ 0x02, HDRQ 0x04, HF0 0x08, HF1 0x10, HLEND 0x20, INIT 0x80)
//   1 CVR (HC 0x80, HV 0x7F: the host command vector is P:2*HV)
//   2 ISR (RXDF 0x01, TXDE 0x02, TRDY 0x04, HF2 0x08, HF3 0x10, HREQ 0x80)
//   3 IVR   5/6/7 RXH/RXM/RXL (read) and TXH/TXM/TXL (write; TXL completes a word)
//
// Boot: a DSP56367 in host-boot mode runs its boot ROM, which reads a count, an
// address and that many words through HDI08 into P memory, then jumps there
// (dsp56300's DspBoot does the memory side). A jump to the boot ROM at
// P:$FF0000 starts that again.
#include "dsp56kBase/logging.h"
#include "dsp56kEmu/disasm.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/dspBootCode.h"
#include "dsp56kEmu/hdi08.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace dsp56k;

namespace
{
// The library logs every ESAI control register write and each transmit
// underrun; drop those and repeats of the same line.
void quietLog(const std::string& s)
{
	static std::mutex m;
	static std::string last;
	std::lock_guard lock(m);
	if(s.find("ESAI RCR") != std::string::npos || s.find("ESAI TCR") != std::string::npos ||
	   s.find("underrun") != std::string::npos)
		return;
	if(s == last) return;
	last = s;
	std::fprintf(stderr, "dsp56300: %s\n", s.c_str());
}

struct G2Dsp
{
	DefaultMemoryValidator validator;
	// Internal memories, plus the external SRAM that stage 1 maps with AAR0 = $800031 [C]:
	// base $800000, X and Y enabled, no address bits compared, so X and Y external
	// accesses reach the same RAM: bridged from $800000; stage 1 clears up to $83FFFF.
	Memory mem{validator, 0x080000, 0x840000, 0x800000};
	Peripherals56367 periphY;
	Peripherals56362 periphX{&periphY};
	DSP dsp{mem, &periphX, &periphY};
	HDI08& hdi = periphX.getHDI08();

	// host side
	uint8_t icr = 0, cvr = 0, ivr = 0x0f;
	uint8_t tx[3] = {0, 0, 0};
	uint8_t rx[3] = {0, 0, 0};
	bool rxLatched = false;

	// DSP side
	std::atomic<bool> booting{true};
	std::atomic<bool> quit{false};
	std::atomic<int> bootCount{0};
	std::atomic<uint32_t> lastBootLength{0}, lastBootAddress{0};
	std::thread thread;

	// ESAI (0) and ESAI_1 (1) transmit frames, flattened: slot count, then 6
	// transmit registers per slot; recorded only while capture is on.
	std::mutex audioMutex;
	std::vector<uint32_t> audioTx[2];
	std::atomic<bool> capture{false};
	std::atomic<uint64_t> txFrames[2] = {0, 0};

	G2Dsp()
	{
		Esai* esais[2] = {&periphX.getEsai(), &periphY.getEsai()};
		for(int i = 0; i < 2; ++i)
		{
			// nothing drives the receivers: zeros in, without blocking
			esais[i]->setReadRxCallback([](uint64_t& _frame, Audio::RxFrame& _f)
			{
				_f.resize(Audio::MaxSlotsPerFrame);
				for(uint32_t s = 0; s < Audio::MaxSlotsPerFrame; ++s) _f[s].fill(0);
				++_frame;
			});
			esais[i]->setWriteTxCallback([this, i](uint64_t& _frame, const Audio::TxFrame& _f)
			{
				++_frame;
				++txFrames[i];
				if(!capture) return;
				std::lock_guard lock(audioMutex);
				if(audioTx[i].size() > (64u << 20)) return;
				audioTx[i].push_back(_f.size());
				for(uint32_t s = 0; s < _f.size(); ++s)
					for(auto w : _f[s]) audioTx[i].push_back(w);
			});
		}
		thread = std::thread([this] { threadFunc(); });
	}

	~G2Dsp()
	{
		quit = true;
		dsp.terminate();
		thread.join();
	}

	// the boot ROM: count, address, then the words into P, then jump there
	void bootRom()
	{
		DspBoot boot(dsp);
		while(!quit)
		{
			if(hdi.rxData().empty())
			{
				std::this_thread::sleep_for(std::chrono::microseconds(50));
				continue;
			}
			const TWord w = hdi.readRX(Movep_Spp);
			if(boot.hdiWriteTX(w))
			{
				lastBootLength = boot.getLength();
				lastBootAddress = boot.getInitialPC();
				++bootCount;
				booting = false;
				return;
			}
		}
	}

	void threadFunc()
	{
		while(!quit)
		{
			if(booting)
			{
				bootRom();
				continue;
			}
			dsp.execInterpreter();  // a DO FOREVER loop does not come back from here
			if(dsp.getPC().toWord() >= 0xff0000)  // back into the boot ROM
				booting = true;
		}
	}

	uint8_t isr()
	{
		uint8_t v = 0;
		if(rxLatched || hdi.hasTX()) v |= 0x01 | 0x80;
		v |= 0x02;                         // (inferred) the host may write ahead: the queue keeps the order
		if(hdi.rxData().empty()) v |= 0x04;
		v |= hdi.readControlRegister() & 0x18;  // HF2, HF3
		return v;
	}

	uint8_t readCvr()
	{
		// HC stays set until the DSP has taken the command
		if((cvr & 0x80) && !dsp.hasPendingExternalInterrupts())
			cvr &= 0x7f;
		return cvr;
	}

	uint8_t read(int reg)
	{
		switch(reg)
		{
		case 0: return icr;
		case 1: return readCvr();
		case 2: return isr();
		case 3: return ivr;
		case 5: case 6: case 7:
			if(!rxLatched && hdi.hasTX())
			{
				const TWord w = hdi.readTX();
				rx[0] = (w >> 16) & 0xff; rx[1] = (w >> 8) & 0xff; rx[2] = w & 0xff;
				rxLatched = true;
			}
			{
				const uint8_t v = (icr & 0x20) ? rx[2 - (reg - 5)] : rx[reg - 5];
				if(reg == 7) rxLatched = false;
				return v;
			}
		default: return 0;
		}
	}

	void write(int reg, uint8_t v)
	{
		switch(reg)
		{
		case 0:
			icr = v & 0x7f;  // INIT completes at once
			hdi.setPendingHostFlags01(v & 0x18);
			break;
		case 1:
			cvr = v;
			if(v & 0x80)
			{
				// (inferred) the DSP takes it when HCIE allows; the stage-1 program enables HCIE
				// before the host sends any command
				hdi.injectHostCommand(TWord(v & 0x7f) * 2);
			}
			break;
		case 3: ivr = v; break;
		case 5: case 6: case 7:
			tx[reg - 5] = v;
			if(reg == 7)
			{
				const TWord w = (icr & 0x20) ? (TWord(tx[2]) << 16 | TWord(tx[1]) << 8 | tx[0])
				                             : (TWord(tx[0]) << 16 | TWord(tx[1]) << 8 | tx[2]);
				hdi.writeRX(&w, 1);
			}
			break;
		default: break;
		}
	}
};
}

extern "C"
{
	void* g2dsp_create() { Logging::setLogFunc(&quietLog); return new G2Dsp(); }
	void g2dsp_destroy(void* h) { delete static_cast<G2Dsp*>(h); }
	uint32_t g2dsp_host_read(void* h, int reg) { return static_cast<G2Dsp*>(h)->read(reg); }
	void g2dsp_host_write(void* h, int reg, uint32_t v) { static_cast<G2Dsp*>(h)->write(reg, uint8_t(v)); }
	// kept for the lockstep interface: the DSPs run on their own
	void g2dsp_run(void*, uint32_t) {}
	// disassemble P:addr into buf; returns the instruction length in words
	int g2dsp_disasm(void* h, uint32_t addr, char* buf, int len)
	{
		static Opcodes opcodes;
		static Disassembler disasm(opcodes);
		auto* d = static_cast<G2Dsp*>(h);
		std::string text;
		const TWord a = d->mem.get(MemArea_P, addr), b = d->mem.get(MemArea_P, addr + 1);
		const auto n = disasm.disassemble(text, a, b, 0, 0, addr);
		std::snprintf(buf, size_t(len), "%s", text.c_str());
		return int(n ? n : 1);
	}
	int g2dsp_booting(void* h) { return static_cast<G2Dsp*>(h)->booting ? 1 : 0; }
	int g2dsp_boot_count(void* h) { return static_cast<G2Dsp*>(h)->bootCount; }
	uint32_t g2dsp_boot_info(void* h, int which)
	{
		auto* d = static_cast<G2Dsp*>(h);
		return which == 0 ? d->lastBootLength : d->lastBootAddress;
	}
	uint32_t g2dsp_pc(void* h) { return static_cast<G2Dsp*>(h)->dsp.getPC().toWord(); }
	uint64_t g2dsp_instructions(void* h) { return static_cast<G2Dsp*>(h)->dsp.getInstructionCounter(); }
	uint32_t g2dsp_hcr(void* h) { return static_cast<G2Dsp*>(h)->hdi.readControlRegister(); }
	uint32_t g2dsp_rx_queued(void* h) { return uint32_t(static_cast<G2Dsp*>(h)->hdi.rxData().size()); }
	// area: 0 P, 1 X, 2 Y (dsp56k::EMemArea); reads race with the running DSP, which is fine for dumps
	uint32_t g2dsp_mem_read(void* h, int area, uint32_t addr) { return static_cast<G2Dsp*>(h)->mem.get(EMemArea(area), addr); }
	uint32_t g2dsp_reg(void* h, int which)
	{
		auto& r = static_cast<G2Dsp*>(h)->dsp.regs();
		switch(which)
		{
		case 0: return r.sr.toWord();
		case 1: return r.omr.toWord();
		case 2: return r.vba.toWord();
		case 3: return r.sp.toWord();
		default: return (which >= 10 && which < 18) ? uint32_t(r.r[which - 10].toWord()) : 0;
		}
	}
	void g2dsp_capture(void* h, int on) { static_cast<G2Dsp*>(h)->capture = on != 0; }
	uint64_t g2dsp_tx_frames(void* h, int esai) { return static_cast<G2Dsp*>(h)->txFrames[esai & 1]; }
	// moves up to max words of captured ESAI transmit data into buf; returns the count
	uint32_t g2dsp_tx_take(void* h, int esai, uint32_t* buf, uint32_t max)
	{
		auto* d = static_cast<G2Dsp*>(h);
		std::lock_guard lock(d->audioMutex);
		auto& v = d->audioTx[esai & 1];
		const uint32_t n = uint32_t(std::min<size_t>(max, v.size()));
		std::memcpy(buf, v.data(), n * sizeof(uint32_t));
		v.erase(v.begin(), v.begin() + n);
		return n;
	}
}
