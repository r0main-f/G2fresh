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
//
// Serial audio (re/notes/g2-hardware-and-emulation.md 3.7). Each DSP has two
// ESAIs (ESAI in X, ESAI_1 in Y). Their transmit frames can be linked to
// another DSP's receivers (g2dsp_link): what the G2's inter-DSP serial lines do.
// A link carries the words of two transmitters (TX0/TX1 or TX2/TX3) of each
// slot to receivers RX0/RX1 of the other side, frame by frame, through a small
// single-producer single-consumer queue. Each DSP keeps running its own thread:
// the queues pace them against each other, as the shared serial clock does on
// the board (the approach of Gearmulator's Nord Lead 2x, source/claudia/n2x, whose
// DSP A feeds DSP B's ESAI through the library's audio ring buffers; GPL-3,
// adapted, no code copied). A line starts once its receiver listens and its
// sender sends; before that the receiver gets silence. The ESAI clock ticks once
// per slot of the 8-slot frame (g2dsp_clock); an ESAI with fewer slots per frame
// (the converters) gets a divider so that its frames have the same period. An
// unlinked ESAI transmitter is the DACs (g2dsp_sink_record), an unlinked
// receiver the ADCs (silence). While a DSP idles in its background loop, its
// clock skips to the next slot (g2dsp_idle_loop).
#include "dsp56kBase/logging.h"
#include "dsp56kEmu/disasm.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/dspBootCode.h"
#include "dsp56kEmu/esaiclock.h"
#include "dsp56kEmu/hdi08.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#include <pthread.h>
#endif

using namespace dsp56k;

namespace
{
using Clock = std::chrono::steady_clock;

// The library logs every ESAI control register write and each transmit
// underrun; drop those and repeats of the same line.
void quietLog(const std::string& s)
{
	static std::mutex m;
	static std::string last;
	std::lock_guard lock(m);
	if(s.find("Write ESAI") != std::string::npos || s.find("Write Timer") != std::string::npos ||
	   s.find("HPCR") != std::string::npos || s.find("underrun") != std::string::npos)
		return;
	if(s == last) return;
	last = s;
	std::fprintf(stderr, "dsp56300: %s\n", s.c_str());
}

std::atomic<bool> g_quit{false};

struct G2Dsp;

// One serial line pair between two DSPs: the slots of transmitters txBase and
// txBase+1 of `up`'s ESAI `upEsai` arrive in receivers 0 and 1 of `down`'s ESAI
// `downEsai`. Frames are queued whole (the library's ESAI hands frames over at
// their end on the transmit side and at their start on the receive side).
struct Link
{
	static constexpr uint32_t Capacity = 64;   // frames
	static constexpr uint32_t Slots = 8;

	struct Frame { uint32_t slots = 0; TWord w[Slots][2]; };

	G2Dsp* up = nullptr; int upEsai = 0; int txBase = 0;
	G2Dsp* down = nullptr; int downEsai = 0;
	uint32_t prefill = 0;   // empty frames queued when the link starts
	uint32_t maxQueue = 4;  // frames the sender may be ahead before it waits

	Frame ring[Capacity];
	std::atomic<uint32_t> head{0}, tail{0};    // written by the sender / the receiver
	std::atomic<bool> active{false};           // the sender has sent since the receiver listens

	// A thread that finds the queue empty (receiver) or full (sender) blocks on a
	// condition variable; the other side signals it only when someone waits.
	struct Waitable
	{
		std::mutex m;
		std::condition_variable cv;
		std::atomic<int> waiters{0};
		std::atomic<uint32_t> need{1};   // frames (or free places) the waiter waits for
		void wake(uint32_t _have)
		{
			if(waiters.load() && _have >= need.load())
			{
				std::lock_guard lock(m);
				cv.notify_all();
			}
		}
	};
	Waitable data, space;
	uint32_t batch = 1;      // a receiver that had to wait waits for this many frames (or a moment)

	// statistics
	std::atomic<uint64_t> sent{0}, received{0}, dropped{0}, zeros{0}, timeouts{0};

	uint32_t size() const { return head.load() - tail.load(); }
};

// Waits until `have()` (frames, or free places) is at least 1: spins briefly, then
// blocks on `w` until `have()` reaches `want` (a batch, so that the two threads do
// not hand over every single frame) or, after `soon`, until it is at least 1.
// Returns false on a timeout or at shutdown. `waitedNs` accumulates the time.
template<typename F>
bool waitFor(Link::Waitable& w, F have, uint32_t want, std::atomic<uint64_t>& waitedNs,
             std::chrono::microseconds soon, std::chrono::milliseconds timeout)
{
	if(have() >= 1) return true;
	const auto t0 = Clock::now();
	bool ok = false;
	for(uint32_t i = 0; i < 256 && !ok; ++i)
		ok = have() >= 1;
	if(!ok)
	{
		std::unique_lock lock(w.m);
		w.need = want;
		++w.waiters;
		ok = w.cv.wait_for(lock, soon, [&] { return have() >= want || g_quit.load(); });
		if(!ok && have() >= 1)
			ok = true;
		else if(!ok)
		{
			w.need = 1;
			ok = w.cv.wait_for(lock, timeout, [&] { return have() >= 1 || g_quit.load(); });
		}
		--w.waiters;
		ok = ok && !g_quit.load();
	}
	waitedNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
	return ok;
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

	// serial audio, per ESAI (0) and ESAI_1 (1)
	Link* in[2] = {nullptr, nullptr};
	Link* out[2] = {nullptr, nullptr};
	std::atomic<uint64_t> rxFrames[2] = {0, 0};
	std::atomic<uint64_t> waitedNs{0};
#ifdef __APPLE__
	std::atomic<mach_port_t> machThread{0};
#endif

	// the converters: an unlinked ESAI (not ESAI_1) transmitter is a sink (the DACs
	// on the output DSP): while recording is on, its frames are kept as int32 words,
	// slots 0 and 1, transmitters 0 and 1 of each (4 words per frame).
	std::mutex sinkMutex;
	std::vector<int32_t> sink[2];
	std::atomic<uint64_t> sinkLimit{0};        // frames still to record
	std::atomic<double> throttle{0.0};         // >0: the sink waits so that it runs at most this many times real time
	Clock::time_point throttleStart;
	uint64_t throttleFrames = 0;
	uint32_t sampleRate = 96000;

	// ESAI (0) and ESAI_1 (1) transmit frames, flattened: slot count, then 6
	// transmit registers per slot; recorded only while capture is on.
	std::mutex audioMutex;
	std::vector<uint32_t> audioTx[2];
	std::atomic<bool> capture{false};
	std::atomic<uint64_t> txFrames[2] = {0, 0};

	// Idle skipping (g2dsp_idle_loop): while the DSP spins in its background loop
	// (P:first..last) with nothing to do - the frame counter X:counter at most
	// `limit`, no interrupt pending, HF0 clear - nothing it does can change until
	// the next serial slot, so its clock is moved on to that slot instead of
	// running the loop. The loop only reads; skipping it changes no state.
	struct IdleLoop { TWord first = 0, last = 0, counter = 0, limit = 0; };
	IdleLoop idle;
	std::atomic<bool> idleOn{false};
	uint64_t lastTick = 0;                      // instruction count at the last slot tick
	uint32_t ticksPerSlot = 192;
	std::atomic<uint64_t> skipped{0};           // instructions skipped
	std::atomic<bool> useJit{false};

	Esai& esai(int i) { return i ? periphY.getEsai() : periphX.getEsai(); }

	// Debugging (g2dsp_trace, g2dsp_trace_fine): at each slot tick, or before each instruction,
	// the instruction counter, the PC, X:traceAddr and the ESAI_1 frame count. Found the lost
	// condition codes of notes 3.8 (the library's DSP::execInterrupt).
	std::mutex traceMutex;
	std::vector<uint64_t> trace;
	std::atomic<uint32_t> traceLeft{0};
	std::atomic<uint32_t> fineLeft{0};   // per instruction instead of per slot tick
	TWord traceAddr = 0x43;
	void traceTick()
	{
		if(!traceLeft.load(std::memory_order_relaxed))
			return;
		std::lock_guard lock(traceMutex);
		trace.push_back(dsp.getInstructionCounter());
		trace.push_back(uint64_t(dsp.getPC().toWord()) | uint64_t(mem.get(MemArea_X, traceAddr)) << 24 |
		                uint64_t(rxFrames[1].load() & 0xffff) << 48);
		--traceLeft;
	}

	// from the peripherals' exec, on the DSP thread: instructions until it wants to run again
	uint32_t onPeripherals()
	{
		if(fineLeft.load(std::memory_order_relaxed))
		{
			std::lock_guard lock(traceMutex);
			trace.push_back(dsp.getInstructionCounter() | uint64_t(dsp.getProcessingMode()) << 60);
			trace.push_back(uint64_t(dsp.getPC().toWord()) | uint64_t(mem.get(MemArea_X, traceAddr)) << 24 |
			                uint64_t(rxFrames[1].load() & 0xffff) << 48);
			--fineLeft;
			return 0;
		}
		if(!idleOn.load(std::memory_order_relaxed))
			return std::numeric_limits<uint32_t>::max();
		const TWord pc = dsp.getPC().toWord();
		if(pc < idle.first || pc > idle.last)
			return 64;
		if(int32_t(mem.get(MemArea_X, idle.counter) << 8) >> 8 > int32_t(idle.limit))
			return 64;
		if(dsp.hasPendingInterrupts() || hdi.hasPendingHostFlags01() ||
		   (hdi.readStatusRegister() & (1 << HDI08::HSR_HF0)))
			return 64;
		const uint64_t now = dsp.getInstructionCounter();
		const uint64_t next = lastTick + ticksPerSlot;
		if(next > now + 1)
		{
			const auto n = static_cast<TWord>(next - now - 1);
			dsp.fastForward(n, n);
			skipped += n;
		}
		return 0;
	}

	G2Dsp()
	{
		// The ESAI clock ticks once per slot. Stage 1 sets up 8 slots of 24 bits at a bit
		// clock of Fsys/8 on the clock master: 1536 DSP clocks per frame, 192 per slot [C].
		// The clock counts instructions: the library's interpreter does not count cycles in
		// a build that has the JIT (DSP::execOp, `if constexpr(!g_useJIT)`), so a clock on
		// cycles stops after a few frames. Most DSP56300 instructions take one clock, so this
		// gives the frame program somewhat more room than the chip has.
		auto& clock = periphX.getEsaiClock();
		clock.setCyclesPerSample(192);
		for(int i = 0; i < 2; ++i)
		{
			esai(i).setReadRxCallback([this, i](uint64_t& _frame, Audio::RxFrame& _f) { readRx(i, _f); ++_frame; });
			esai(i).setWriteTxCallback([this, i](uint64_t& _frame, const Audio::TxFrame& _f) { writeTx(i, _f); ++_frame; });
		}
		clock.setTickCallback([this] { lastTick = dsp.getInstructionCounter(); traceTick(); });
		periphX.setExecCallback([this] { return onPeripherals(); });
		thread = std::thread([this] { threadFunc(); });
	}

	~G2Dsp()
	{
		quit = true;
		dsp.terminate();
		if(thread.joinable()) thread.join();
	}

	static void zero(Audio::RxFrame& _f)
	{
		_f.resize(Audio::MaxSlotsPerFrame);
		for(uint32_t s = 0; s < Audio::MaxSlotsPerFrame; ++s) _f[s].fill(0);
	}

	// at the first slot of a receive frame
	void readRx(int i, Audio::RxFrame& _f)
	{
		++rxFrames[i];
		zero(_f);
		Link* l = in[i];
		if(!l || !l->active.load(std::memory_order_acquire))
		{
			if(l) ++l->zeros;
			return;  // nothing sends yet (or an unconnected input: the ADCs): silence
		}
		if(!waitFor(l->data, [l] { return l->size(); }, l->batch, waitedNs, std::chrono::microseconds(500),
		            std::chrono::milliseconds(2000)))
		{
			++l->timeouts;
			return;
		}
		const auto& fr = l->ring[l->tail.load() % Link::Capacity];
		for(uint32_t s = 0; s < fr.slots; ++s) { _f[s][0] = fr.w[s][0]; _f[s][1] = fr.w[s][1]; }
		l->tail.fetch_add(1);
		++l->received;
		const uint32_t limit = l->prefill + l->maxQueue;
		const uint32_t n = l->size();
		l->space.wake(n < limit ? limit - n : 0);
	}

	// after the last slot of a transmit frame
	void writeTx(int i, const Audio::TxFrame& _f)
	{
		++txFrames[i];
		if(Link* l = out[i])
			send(*l, _f);
		else if(i == 0 && sinkLimit.load(std::memory_order_relaxed))
			record(i, _f);
		if(i == 0 && !out[i] && throttle.load(std::memory_order_relaxed) > 0)
			pace();
		if(capture)
		{
			std::lock_guard lock(audioMutex);
			if(audioTx[i].size() > (64u << 20)) return;
			audioTx[i].push_back(_f.size());
			for(uint32_t s = 0; s < _f.size(); ++s)
				for(auto w : _f[s]) audioTx[i].push_back(w);
		}
	}

	void send(Link& l, const Audio::TxFrame& _f)
	{
		G2Dsp& d = *l.down;
		if(!d.esai(l.downEsai).getEnabledReceivers())
		{
			// the other side does not listen (yet): the line carries nothing anyone keeps
			if(l.active.exchange(false))
				l.tail.store(l.head.load());
			++l.dropped;
			return;
		}
		if(!l.active.load(std::memory_order_acquire))
		{
			// the first frame since the receiver listens: start with `prefill` empty frames
			l.tail.store(l.head.load());
			for(uint32_t k = 0; k < l.prefill; ++k)
			{
				auto& fr = l.ring[l.head.load(std::memory_order_relaxed) % Link::Capacity];
				fr.slots = Link::Slots;
				std::memset(fr.w, 0, sizeof(fr.w));
				l.head.fetch_add(1);
			}
			l.active.store(true);
		}
		const uint32_t limit = l.prefill + l.maxQueue;
		if(!waitFor(l.space, [&l, limit] { const uint32_t n = l.size(); return n < limit ? limit - n : 0u; }, 1, waitedNs,
		            std::chrono::microseconds(500), std::chrono::milliseconds(2000)))
		{
			++l.timeouts;
			++l.dropped;
			return;
		}
		auto& fr = l.ring[l.head.load() % Link::Capacity];
		fr.slots = std::min<uint32_t>(_f.size(), Link::Slots);
		for(uint32_t s = 0; s < fr.slots; ++s) { fr.w[s][0] = _f[s][l.txBase]; fr.w[s][1] = _f[s][l.txBase + 1]; }
		l.head.fetch_add(1);
		++l.sent;
		l.data.wake(l.size());
	}

	void record(int i, const Audio::TxFrame& _f)
	{
		std::lock_guard lock(sinkMutex);
		for(uint32_t s = 0; s < 2; ++s)
			for(uint32_t t = 0; t < 2; ++t)
				sink[i].push_back(s < _f.size() ? int32_t(_f[s][t] << 8) >> 8 : 0);
		--sinkLimit;
	}

	// the sink's frames may not run ahead of the wall clock (times `throttle`)
	void pace()
	{
		const double speed = throttle.load(std::memory_order_relaxed);
		if(throttleFrames++ == 0) { throttleStart = Clock::now(); return; }
		const auto due = throttleStart + std::chrono::nanoseconds(int64_t(1e9 * double(throttleFrames) / (sampleRate * speed)));
		const auto now = Clock::now();
		if(due > now + std::chrono::milliseconds(2))
			std::this_thread::sleep_until(due);
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
#ifdef __APPLE__
		machThread = pthread_mach_thread_np(pthread_self());
#endif
		while(!quit)
		{
			if(booting)
			{
				bootRom();
				continue;
			}
			if(useJit.load(std::memory_order_relaxed))
				dsp.getJit().getTrampoline().exec(&dsp, 128);  // as dsp56300's DSPThread does
			else
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

std::mutex g_registry;
std::vector<G2Dsp*> g_dsps;
std::vector<std::unique_ptr<Link>> g_links;
}

extern "C"
{
	void* g2dsp_create()
	{
		Logging::setLogFunc(&quietLog);
		auto* d = new G2Dsp();
		std::lock_guard lock(g_registry);
		g_dsps.push_back(d);
		return d;
	}
	void g2dsp_destroy(void* h)
	{
		auto* d = static_cast<G2Dsp*>(h);
		{
			std::lock_guard lock(g_registry);
			g_dsps.erase(std::remove(g_dsps.begin(), g_dsps.end(), d), g_dsps.end());
		}
		delete d;
	}
	// stops every DSP thread (links included); call before the process exits
	void g2dsp_shutdown_all()
	{
		g_quit = true;
		std::vector<G2Dsp*> all;
		{
			std::lock_guard lock(g_registry);
			all.swap(g_dsps);
		}
		for(auto* d : all) { d->quit = true; d->dsp.terminate(); }
		for(auto& l : g_links)
			for(auto* w : {&l->data, &l->space}) { std::lock_guard lock(w->m); w->cv.notify_all(); }
		for(auto* d : all) delete d;
		g_links.clear();
	}
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
	uint64_t g2dsp_cycles(void* h) { return static_cast<G2Dsp*>(h)->dsp.getCycles(); }
	uint32_t g2dsp_hcr(void* h) { return static_cast<G2Dsp*>(h)->hdi.readControlRegister(); }
	uint32_t g2dsp_rx_queued(void* h) { return uint32_t(static_cast<G2Dsp*>(h)->hdi.rxData().size()); }
	// area: 0 P, 1 X, 2 Y (dsp56k::EMemArea); reads race with the running DSP, which is fine for dumps
	uint32_t g2dsp_mem_read(void* h, int area, uint32_t addr) { return static_cast<G2Dsp*>(h)->mem.get(EMemArea(area), addr); }
	// a debugging poke, racing with the running DSP (P writes go through the instruction cache)
	void g2dsp_mem_write(void* h, int area, uint32_t addr, uint32_t value)
	{
		auto* d = static_cast<G2Dsp*>(h);
		if(area == MemArea_P) d->dsp.memWriteP(addr, value & 0xffffff);
		else d->dsp.memWrite(EMemArea(area), addr, value & 0xffffff);
	}
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
	// ESAI (esai 0) or ESAI_1 (1) registers as last written: 0 TCR, 1 TCCR, 2 RCR, 3 RCCR, 4 SAISR, 5 SAICR
	uint32_t g2dsp_esai_reg(void* h, int esai, int which)
	{
		auto& e = static_cast<G2Dsp*>(h)->esai(esai & 1);
		switch(which)
		{
		case 0: return e.readTransmitControlRegister();
		case 1: return e.readTransmitClockControlRegister();
		case 2: return e.readReceiveControlRegister();
		case 3: return e.readReceiveClockControlRegister();
		case 4: return e.readStatusRegister();
		case 5: return e.readControlRegister();
		default: return 0;
		}
	}
	void g2dsp_capture(void* h, int on) { static_cast<G2Dsp*>(h)->capture = on != 0; }
	uint64_t g2dsp_tx_frames(void* h, int esai) { return static_cast<G2Dsp*>(h)->txFrames[esai & 1]; }
	uint64_t g2dsp_rx_frames(void* h, int esai) { return static_cast<G2Dsp*>(h)->rxFrames[esai & 1]; }
	uint64_t g2dsp_waited_ns(void* h) { return static_cast<G2Dsp*>(h)->waitedNs; }
	// CPU time of the DSP's thread (user + system), in nanoseconds (macOS; 0 elsewhere)
	uint64_t g2dsp_thread_cpu_ns(void* h)
	{
#ifdef __APPLE__
		const mach_port_t t = static_cast<G2Dsp*>(h)->machThread;
		if(!t) return 0;
		thread_basic_info_data_t info;
		mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
		if(thread_info(t, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS) return 0;
		return (uint64_t(info.user_time.seconds) + uint64_t(info.system_time.seconds)) * 1000000000ull +
		       (uint64_t(info.user_time.microseconds) + uint64_t(info.system_time.microseconds)) * 1000ull;
#else
		(void)h;
		return 0;
#endif
	}
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

	// The ESAI clock: DSP clocks per slot tick (192: 1536 per 8-slot frame), and per ESAI
	// the ticks per slot minus one for the transmit and the receive side (0 for an 8-slot
	// interface, 3 for a 2-slot one, so that all frames have the same period).
	void g2dsp_clock(void* h, uint32_t cyclesPerTick, uint32_t divTx0, uint32_t divRx0, uint32_t divTx1, uint32_t divRx1)
	{
		auto* d = static_cast<G2Dsp*>(h);
		auto& clock = d->periphX.getEsaiClock();
		clock.setCyclesPerSample(cyclesPerTick);
		d->ticksPerSlot = cyclesPerTick;
		clock.setEsaiDivider(&d->esai(0), divTx0, divRx0);
		clock.setEsaiDivider(&d->esai(1), divTx1, divRx1);
	}

	// The DSP's background loop P:first..last, which spins while X:counter <= limit:
	// skip its idle time (see G2Dsp::onPeripherals). first = last = 0 turns it off.
	void g2dsp_idle_loop(void* h, uint32_t first, uint32_t last, uint32_t counter, uint32_t limit)
	{
		auto* d = static_cast<G2Dsp*>(h);
		d->idleOn = false;
		d->idle = {first, last, counter, limit};
		d->idleOn = last != 0;
	}
	uint64_t g2dsp_skipped(void* h) { return static_cast<G2Dsp*>(h)->skipped; }
	// debugging: record `ticks` slot ticks: per tick two words, the instruction counter, then
	// PC | X:addr << 24 | (ESAI_1 receive frames & $FFFF) << 48
	void g2dsp_trace(void* h, uint32_t ticks, uint32_t addr)
	{
		auto* d = static_cast<G2Dsp*>(h);
		std::lock_guard lock(d->traceMutex);
		d->trace.clear();
		d->traceAddr = addr;
		d->traceLeft = ticks;
	}
	// The same before each instruction (the peripherals run before each one meanwhile, which changes
	// the timing; idle skipping is off while it records). The library runs no peripherals inside a
	// long interrupt, so the instructions of the frame program and of other jsr vectors are missing:
	// they show as gaps in the instruction counter. The first word also carries the processing
	// mode in bits 60-63.
	void g2dsp_trace_fine(void* h, uint32_t instructions, uint32_t addr)
	{
		auto* d = static_cast<G2Dsp*>(h);
		std::lock_guard lock(d->traceMutex);
		d->trace.clear();
		d->traceAddr = addr;
		d->fineLeft = instructions;
	}
	uint32_t g2dsp_trace_take(void* h, uint64_t* buf, uint32_t max)
	{
		auto* d = static_cast<G2Dsp*>(h);
		std::lock_guard lock(d->traceMutex);
		const uint32_t n = uint32_t(std::min<size_t>(max, d->trace.size()));
		std::memcpy(buf, d->trace.data(), n * sizeof(uint64_t));
		d->trace.erase(d->trace.begin(), d->trace.begin() + n);
		return n;
	}
	// run the DSP with dsp56300's JIT instead of its interpreter (set before boot). Experimental:
	// so far the DSPs did not answer the stage-1 HF0 handshake with it (notes 3.7.6)
	void g2dsp_jit(void* h, int on) { static_cast<G2Dsp*>(h)->useJit = on != 0; }

	// Links transmitters txBase, txBase+1 of `up`'s ESAI upEsai to receivers 0, 1 of
	// `down`'s ESAI downEsai. prefill: empty frames in the line when it starts (the
	// receiver's head start); maxQueue: frames the sender may run ahead beyond that;
	// batch: frames a receiver that had to wait waits for (or 0.5 ms).
	// Returns a link index for g2dsp_link_stats.
	int g2dsp_link(void* up, int upEsai, int txBase, void* down, int downEsai, uint32_t prefill, uint32_t maxQueue,
	               uint32_t batch)
	{
		auto l = std::make_unique<Link>();
		l->up = static_cast<G2Dsp*>(up); l->upEsai = upEsai & 1; l->txBase = txBase & 4 ? 4 : txBase & 2;
		l->down = static_cast<G2Dsp*>(down); l->downEsai = downEsai & 1;
		l->prefill = std::min<uint32_t>(prefill, Link::Capacity / 2);
		l->maxQueue = std::max<uint32_t>(1, std::min<uint32_t>(maxQueue, Link::Capacity / 2 - 1));
		l->batch = std::max<uint32_t>(1, std::min<uint32_t>(batch, l->maxQueue));
		l->up->out[l->upEsai] = l.get();
		l->down->in[l->downEsai] = l.get();
		std::lock_guard lock(g_registry);
		g_links.push_back(std::move(l));
		return int(g_links.size() - 1);
	}
	// 0 sent, 1 received, 2 dropped, 3 zero frames before it started, 4 timeouts, 5 queued now, 6 active
	uint64_t g2dsp_link_stats(int link, int which)
	{
		if(link < 0 || size_t(link) >= g_links.size()) return 0;
		auto& l = *g_links[size_t(link)];
		switch(which)
		{
		case 0: return l.sent; case 1: return l.received; case 2: return l.dropped;
		case 3: return l.zeros; case 4: return l.timeouts; case 5: return l.size(); case 6: return l.active ? 1 : 0;
		default: return 0;
		}
	}

	// Records the next `frames` frames of an unlinked ESAI's transmitters 0 and 1
	// (2 slots each: 4 words per frame).
	void g2dsp_sink_record(void* h, uint64_t frames) { static_cast<G2Dsp*>(h)->sinkLimit = frames; }
	uint32_t g2dsp_sink_take(void* h, int esai, int32_t* buf, uint32_t max)
	{
		auto* d = static_cast<G2Dsp*>(h);
		std::lock_guard lock(d->sinkMutex);
		auto& v = d->sink[esai & 1];
		const uint32_t n = uint32_t(std::min<size_t>(max, v.size()));
		std::memcpy(buf, v.data(), n * sizeof(int32_t));
		v.erase(v.begin(), v.begin() + n);
		return n;
	}
	// >0: an unlinked transmitter (the DACs) runs at most `speed` times real time; 0: as fast as it can
	void g2dsp_throttle(void* h, double speed, uint32_t sampleRate)
	{
		auto* d = static_cast<G2Dsp*>(h);
		d->sampleRate = sampleRate ? sampleRate : 96000;
		d->throttleFrames = 0;
		d->throttle = speed;
	}
}
