// Runs one DSP's per-frame program offline, one frame (one sample) at a time,
// from the memories g2hostemu.py dumped from the emulated G2 after it loaded a
// patch (dspN_live_{P,X,Y,Yext}.bin in its --out directory; Clavia data, keep
// them out of the repository). This gives the samples the patch computes
// without the ESAI/DMA timing of the full machine.
//
//   g2dspframe DIR N FRAMES [wav=out.wav] [watch=X:2] [trace]
//
// The frame entry is the target of the per-frame `jsr` at P:$76 that stage 1
// installs (P:$77). The frame program first waits for the DMA channels that
// bring the ESAI receive data (`brclr #n,x:M_DSTR,*`): those polls become NOPs.
// It ends with `rti`; each frame here runs from the entry up to that `rti`.
#include "dsp56kEmu/disasm.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace dsp56k;

static std::vector<TWord> load(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	if(!f) throw std::runtime_error("cannot read " + path);
	std::vector<TWord> w;
	unsigned char b[3];
	while(f.read(reinterpret_cast<char*>(b), 3)) w.push_back(TWord(b[0]) << 16 | TWord(b[1]) << 8 | b[2]);
	return w;
}

static double frac(TWord w) { return double(int32_t(w << 8) >> 8) / 8388608.0; }

int main(int argc, char** argv)
{
	if(argc < 4) { std::cerr << "g2dspframe DIR N FRAMES [wav=out.wav] [watch=X:2] [trace]\n"; return 2; }
	const std::string dir = argv[1], n = argv[2];
	const int frames = std::atoi(argv[3]);
	std::string wav;
	EMemArea watchArea = MemArea_X;
	TWord watchAddr = 2;
	bool trace = false;
	for(int i = 4; i < argc; ++i)
	{
		const std::string a = argv[i];
		if(a.rfind("wav=", 0) == 0) wav = a.substr(4);
		else if(a.rfind("watch=", 0) == 0) { watchArea = a[6] == 'Y' ? MemArea_Y : MemArea_X; watchAddr = std::stoul(a.substr(8), nullptr, 16); }
		else if(a == "trace") trace = true;
	}

	DefaultMemoryValidator validator;
	Memory mem(validator, 0x080000, 0x840000, 0x800000);
	Peripherals56367 periphY;
	Peripherals56362 periphX(&periphY);
	DSP dsp(mem, &periphX, &periphY);
	for(int i = 0; i < 2; ++i)
	{
		Esai& e = i ? periphY.getEsai() : periphX.getEsai();
		e.setReadRxCallback([](uint64_t& f, Audio::RxFrame& r) { r.resize(Audio::MaxSlotsPerFrame); for(uint32_t s = 0; s < Audio::MaxSlotsPerFrame; ++s) r[s].fill(0); ++f; });
		e.setWriteTxCallback([](uint64_t& f, const Audio::TxFrame&) { ++f; });
	}

	const auto P = load(dir + "/dsp" + n + "_live_P.bin");
	const auto X = load(dir + "/dsp" + n + "_live_X.bin");
	const auto Y = load(dir + "/dsp" + n + "_live_Y.bin");
	const auto Ye = load(dir + "/dsp" + n + "_live_Yext.bin");
	for(size_t i = 0; i < P.size(); ++i) dsp.memWriteP(TWord(i), P[i]);
	for(size_t i = 0; i < X.size(); ++i) dsp.memWrite(MemArea_X, TWord(i), X[i]);
	for(size_t i = 0; i < Y.size(); ++i) dsp.memWrite(MemArea_Y, TWord(i), Y[i]);
	for(size_t i = 0; i < Ye.size(); ++i) dsp.memWrite(MemArea_Y, TWord(0x800000 + i), Ye[i]);

	const TWord entry = P.at(0x77);
	// the DMA-done polls: brclr #n,x:<<$FFFFF4 (DSTR),* = $0CF40n $000000
	int patched = 0;
	for(TWord a = entry; a + 1 < P.size() && a < entry + 0x800; ++a)
		if((P[a] & 0xfffff0) == 0x0cf400 && P[a + 1] == 0) { dsp.memWriteP(a, 0); dsp.memWriteP(a + 1, 0); ++patched; }
	std::cerr << "entry P:$" << std::hex << entry << std::dec << ", " << patched << " DMA polls removed\n";

	// what stage 1 leaves in the registers the frame program relies on
	auto& r = dsp.regs();
	for(int i = 0; i < 8; ++i) r.m[i].var = 0xffffff;
	r.r[6].var = 0x1bc0; r.n[2].var = 0x18fc; r.n[5].var = 0x19c0;

	Opcodes opcodes; Disassembler disasm(opcodes);
	std::vector<float> out;
	uint64_t steps = 0;
	for(int f = 0; f < frames; ++f)
	{
		dsp.setPC(entry);
		for(int guard = 0; guard < 200000; ++guard)
		{
			const TWord pc = dsp.getPC().toWord();
			if(mem.get(MemArea_P, pc) == 0x000004) break;  // rti: end of the frame
			if(trace && f == 0)
			{
				std::string t; TWord a = mem.get(MemArea_P, pc), b = mem.get(MemArea_P, pc + 1);
				disasm.disassemble(t, a, b, 0, 0, pc); std::cerr << std::hex << pc << ": " << t << std::dec << "\n";
			}
			dsp.execInterpreter();
			++steps;
		}
		out.push_back(float(frac(mem.get(watchArea, watchAddr))));
	}

	double lo = 1e9, hi = -1e9, mean = 0; int crossings = 0;
	for(size_t k = 0; k < out.size(); ++k)
	{
		lo = std::min(lo, double(out[k])); hi = std::max(hi, double(out[k])); mean += out[k];
	}
	mean /= double(out.size());
	for(size_t k = 1; k < out.size(); ++k) if((out[k - 1] < mean) != (out[k] < mean)) ++crossings;
	std::printf("%d frames, %llu instructions (%.1f per frame); %c:$%x min %.5f max %.5f, %d crossings of the mean"
	            " -> %.2f Hz at 96 kHz\n", frames, (unsigned long long)steps, double(steps) / frames,
	            watchArea == MemArea_Y ? 'Y' : 'X', watchAddr, lo, hi, crossings, crossings / 2.0 * 96000.0 / frames);
	for(int k = 0; k < 24 && k < int(out.size()); ++k) std::printf("%s%.5f", k ? " " : "first: ", out[k]);
	std::printf("\n");
	if(!wav.empty())
	{
		std::ofstream w(wav, std::ios::binary);
		auto u32 = [&](uint32_t v) { w.write(reinterpret_cast<const char*>(&v), 4); };
		auto u16 = [&](uint16_t v) { w.write(reinterpret_cast<const char*>(&v), 2); };
		const uint32_t bytes = uint32_t(out.size() * 4);
		w.write("RIFF", 4); u32(36 + bytes); w.write("WAVEfmt ", 8); u32(16); u16(3); u16(1); u32(96000); u32(96000 * 4); u16(4); u16(32);
		w.write("data", 4); u32(bytes); w.write(reinterpret_cast<const char*>(out.data()), bytes);
	}
	return 0;
}
