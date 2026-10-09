// Runs one DSP's per-frame program offline, one frame (one sample) at a time,
// from the memories g2hostemu.py dumped from the emulated G2 after it loaded a
// patch (dspN_live_{P,X,Y,Yext}.bin in its --out directory; Clavia data, keep
// them out of the repository). This gives the samples the patch computes
// without the ESAI/DMA timing of the full machine.
//
//   g2dspframe DIR N FRAMES [wav=out.wav] [watch=X:2]... [in=X:10:sine:440]...
//              [poke=Y:58=1c200]... [dump=out.f32] [trace]
//
// in= drives a word before every frame: impulse, dc:V, sine:HZ, saw:HZ, noise,
// or sweep:F0:F1 (exponential sine sweep over the run); values are fractions.
// poke= writes a word once before the first frame. watch= may repeat; dump=
// writes all watched words per frame as interleaved float32 (for analysis).
// inat=PC applies the inputs when the frame reaches P:PC (right before the
// module that reads them) instead of at the frame start, so that the code that
// normally writes that cable (a source module) is overridden.
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
	bool trace = false, watchSet = false;
	struct Watch { EMemArea area; TWord addr; };
	struct Input { EMemArea area; TWord addr; std::string kind; double a = 0, b = 0; };
	struct Poke { EMemArea area; TWord addr; TWord value; };
	std::vector<Watch> watches;
	std::vector<Input> inputs;
	std::vector<Poke> pokes;
	std::string dump;
	long inAt = -1;
	auto areaOf = [](char c) { return c == 'Y' ? MemArea_Y : c == 'P' ? MemArea_P : MemArea_X; };
	for(int i = 4; i < argc; ++i)
	{
		const std::string a = argv[i];
		if(a.rfind("wav=", 0) == 0) wav = a.substr(4);
		else if(a.rfind("watch=", 0) == 0)
		{
			const Watch w{areaOf(a[6]), TWord(std::stoul(a.substr(8), nullptr, 16))};
			if(!watchSet) { watchArea = w.area; watchAddr = w.addr; watchSet = true; }
			watches.push_back(w);
		}
		else if(a.rfind("in=", 0) == 0)  // in=X:10:sine:440
		{
			Input in{areaOf(a[3]), 0, ""};
			std::vector<std::string> f; size_t st = 5, q;
			while((q = a.find(':', st)) != std::string::npos) { f.push_back(a.substr(st, q - st)); st = q + 1; }
			f.push_back(a.substr(st));
			in.addr = TWord(std::stoul(f.at(0), nullptr, 16));
			in.kind = f.at(1);
			if(f.size() > 2) in.a = std::stod(f[2]);
			if(f.size() > 3) in.b = std::stod(f[3]);
			inputs.push_back(in);
		}
		else if(a.rfind("poke=", 0) == 0)  // poke=Y:58=1c200
			pokes.push_back({areaOf(a[5]), TWord(std::stoul(a.substr(7, a.find('=', 7) - 7), nullptr, 16)),
			                 TWord(std::stoul(a.substr(a.find('=', 7) + 1), nullptr, 16))});
		else if(a.rfind("dump=", 0) == 0) dump = a.substr(5);
		else if(a.rfind("inat=", 0) == 0) inAt = std::stol(a.substr(5), nullptr, 16);
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

	for(const auto& p : pokes) dsp.memWrite(p.area, p.addr, p.value & 0xffffff);
	if(watches.empty()) watches.push_back({watchArea, watchAddr});
	auto toWord = [](double v) { long x = std::lround(std::max(-1.0, std::min(v, 1.0 - 1.0 / 8388608)) * 8388608); return TWord(x) & 0xffffff; };
	uint64_t seed = 1;
	std::vector<float> all;
	Opcodes opcodes; Disassembler disasm(opcodes);
	std::vector<float> out;
	uint64_t steps = 0;
	for(int f = 0; f < frames; ++f)
	{
		auto applyInputs = [&]
		{
		for(const auto& in : inputs)
		{
			const double t = f / 96000.0;
			double v = 0;
			if(in.kind == "impulse") v = f == 0 ? 0.5 : 0;
			else if(in.kind == "dc") v = in.a;
			else if(in.kind == "sine") v = 0.5 * std::sin(2 * M_PI * in.a * t);
			else if(in.kind == "saw") v = 2 * std::fmod(in.a * t, 1.0) - 1;
			else if(in.kind == "noise") { seed = seed * 6364136223846793005ULL + 1; v = 0.5 * double(int32_t(seed >> 32)) / 2147483648.0; }
			else if(in.kind == "sweep")
			{
				const double T = frames / 96000.0, k = std::log(in.b / in.a);
				v = 0.5 * std::sin(2 * M_PI * in.a * T / k * (std::exp(k * t / T) - 1));
			}
			dsp.memWrite(in.area, in.addr, toWord(v));
		}
		};
		if(inAt < 0) applyInputs();
		dsp.setPC(entry);
		for(int guard = 0; guard < 200000; ++guard)
		{
			const TWord pc = dsp.getPC().toWord();
			if(mem.get(MemArea_P, pc) == 0x000004) break;  // rti: end of the frame
			if(long(pc) == inAt) applyInputs();
			if(trace)
			{
				std::string t; TWord a = mem.get(MemArea_P, pc), b = mem.get(MemArea_P, pc + 1);
				disasm.disassemble(t, a, b, 0, 0, pc); std::cerr << std::hex << pc << ": " << t << std::dec << "\n";
			}
			dsp.execInterpreter();
			++steps;
		}
		out.push_back(float(frac(mem.get(watchArea, watchAddr))));
		for(const auto& w : watches) all.push_back(float(frac(mem.get(w.area, w.addr))));
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
	if(!dump.empty())
	{
		std::ofstream d(dump, std::ios::binary);
		d.write(reinterpret_cast<const char*>(all.data()), std::streamsize(all.size() * 4));
	}
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
