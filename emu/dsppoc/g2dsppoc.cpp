// Proof of concept: run one DSP fragment of the G2 OS (from the user's own
// firmware, exported by tools/firmware/g2frags.py) on Gearmulator's DSP56300
// emulator, sample by sample, and look at its outputs.
//
// A fragment is straight-line DSP56300 code with its X and Y data blocks.
// It reaches its own state through r3 (X block) and r4 (Y block), and its
// cables through short absolute addresses left as $0 in the OS (patched when
// the synth links a patch): here they become zero-page slots $10, $11, ... in
// order of appearance, loads being inputs and stores outputs. Some fragments
// write results through a pointer kept in their X block instead (watch=).
//
//   g2dsppoc FRAG SAMPLES [X:i=hex] [Y:i=hex] [in:k=saw:Hz|sine:Hz|noise|const:v]
//            [watch=hexaddr,count] [wav=out.wav] [trace]
#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/disasm.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/opcodes.h"
#include "dsp56kEmu/peripherals.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace dsp56k;

static std::vector<TWord> readLine(std::istream& in, char tag)
{
    std::string t; size_t n; in >> t >> n;
    if (t[0] != tag) throw std::runtime_error("bad frag file");
    std::vector<TWord> w(n);
    for (auto& x : w) { std::string h; in >> h; x = std::stoul(h, nullptr, 16); }
    return w;
}

static double frac(TWord w) { return (w & 0x800000 ? double(int(w) - (1 << 24)) : double(w)) / double(1 << 23); }
static TWord toWord(double v)
{
    long x = std::lround(std::max(-1.0, std::min(v, 1.0 - 1.0 / (1 << 23))) * (1 << 23));
    return TWord(x) & 0xFFFFFF;
}

int main(int argc, char** argv)
{
    if (argc < 3) { std::cerr << "g2dsppoc FRAGFILE SAMPLES [X:i=hex] [Y:i=hex] [in:k=saw:Hz|sine:Hz|noise|const:v] [wav=out.wav] [trace]\n"; return 2; }
    std::ifstream f(argv[1]);
    auto P = readLine(f, 'P'), X = readLine(f, 'X'), Y = readLine(f, 'Y');
    const int samples = std::atoi(argv[2]);
    std::map<int, std::string> inputs; std::string wav; bool trace = false;
    TWord watchAddr = 0; int watchCount = 0; // extra outputs: X memory written through a pointer
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("X:", 0) == 0 || a.rfind("Y:", 0) == 0) {
            const int idx = std::stoi(a.substr(2, a.find('=') - 2));
            const TWord v = std::stoul(a.substr(a.find('=') + 1), nullptr, 16);
            (a[0] == 'X' ? X : Y).at(idx) = v;
        } else if (a.rfind("in:", 0) == 0) inputs[std::stoi(a.substr(3, a.find('=') - 3))] = a.substr(a.find('=') + 1);
        else if (a.rfind("wav=", 0) == 0) wav = a.substr(4);
        else if (a == "trace") trace = true;
        else if (a.rfind("watch=", 0) == 0) {
            watchAddr = std::stoul(a.substr(6, a.find(',') - 6), nullptr, 16);
            watchCount = std::stoi(a.substr(a.find(',') + 1));
        }
    }

    DefaultMemoryValidator validator;
    Memory mem(validator, 0x080000, 0x800000, 0x200000);
    Peripherals56362 periphX; Peripherals56367 periphY;
    DSP dsp(mem, &periphX, &periphY);
    Opcodes opcodes; Disassembler disasm(opcodes); Assembler assembler;

    // Re-assemble each instruction; the placeholder short addresses x:$0 / y:$0
    // get zero-page slots 0x10, 0x11, ... in order of appearance.
    constexpr TWord kCode = 0x100, kXBase = 0x1000, kYBase = 0x1000, kZp = 0x10;
    std::vector<std::string> roles; // "in" or "out" per slot
    std::vector<TWord> code;
    for (size_t pc = 0; pc < P.size();) {
        std::string text;
        const TWord opB = pc + 1 < P.size() ? P[pc + 1] : 0;
        const auto len = disasm.disassemble(text, P[pc], opB, 0, 0, TWord(pc));
        const bool hasPlaceholder = text.find("x:$0") != std::string::npos || text.find("y:$0") != std::string::npos;
        if (!hasPlaceholder) { // copied as is
            for (uint32_t k = 0; k < (len ? len : 1); ++k) code.push_back(P[pc + k]);
            if (trace) std::cerr << pc << ": " << text << "\n";
            pc += len ? len : 1;
            continue;
        }
        // Round trip: the unchanged text must give the same words, so that
        // the assembler can be trusted to patch this instruction.
        const auto again = assembler.assemble(text.c_str());
        if (!again.success() || again.word[0] != P[pc] || (again.wordCount == 2 && again.word[1] != opB)) {
            std::cerr << "round trip failed at " << pc << ": " << text << "\n";
            return 1;
        }
        std::string patched = text;
        for (const char* mem : {"x:$0", "y:$0"}) {
            size_t at;
            while ((at = patched.find(mem)) != std::string::npos && (at + 4 == patched.size() || !std::isxdigit(patched[at + 4]))) {
                const bool load = at + 4 < patched.size() && patched[at + 4] == ',';
                char slot[16]; std::snprintf(slot, sizeof slot, "%c:$%x", mem[0], unsigned(kZp + roles.size()));
                roles.push_back(load ? "in" : "out");
                patched.replace(at, 4, slot);
            }
        }
        const auto r = assembler.assemble(patched.c_str());
        if (!r.success()) { std::cerr << "cannot assemble: " << patched << "\n"; return 1; }
        for (uint32_t k = 0; k < r.wordCount; ++k) code.push_back(r.word[k]);
        if (trace) std::cerr << pc << ": " << patched << "\n";
        pc += len ? len : 1;
    }
    for (size_t i = 0; i < roles.size(); ++i) std::cerr << "slot " << i << " (x/y:$" << std::hex << kZp + i << std::dec << "): " << roles[i] << "\n";
    for (size_t i = 0; i < code.size(); ++i) dsp.memWriteP(TWord(kCode + i), code[i]);
    for (size_t i = 0; i < X.size(); ++i) dsp.memWrite(MemArea_X, TWord(kXBase + i), X[i]);
    for (size_t i = 0; i < Y.size(); ++i) dsp.memWrite(MemArea_Y, TWord(kYBase + i), Y[i]);

    const double fs = 96000.0;
    for (int i = 0; i < watchCount; ++i) roles.push_back("out"); // watched words follow the slots
    std::vector<std::vector<float>> outs(roles.size());
    const size_t slotCount = roles.size() - size_t(watchCount);
    uint64_t steps = 0, seed = 1;
    for (int s = 0; s < samples; ++s) {
        for (auto& [slot, kind] : inputs) {
            double v = 0, ph;
            if (kind.rfind("saw:", 0) == 0) { ph = std::fmod(s * std::stod(kind.substr(4)) / fs, 1.0); v = 2 * ph - 1; }
            else if (kind.rfind("sine:", 0) == 0) v = std::sin(2 * M_PI * s * std::stod(kind.substr(5)) / fs);
            else if (kind == "noise") { seed = seed * 6364136223846793005ULL + 1; v = double(int32_t(seed >> 32)) / 2147483648.0; }
            else if (kind.rfind("const:", 0) == 0) v = std::stod(kind.substr(6));
            dsp.memWrite(MemArea_X, TWord(kZp + slot), toWord(v * 0.5));
        }
        dsp.regs().r[3].var = kXBase; dsp.regs().r[4].var = kYBase;
        dsp.setPC(kCode);
        for (int guard = 0; dsp.getPC().toWord() != kCode + code.size() && guard < 100000; ++guard, ++steps)
            dsp.execInterpreter();
        for (size_t i = 0; i < roles.size(); ++i) {
            const TWord addr = i < slotCount ? TWord(kZp + i) : TWord(watchAddr + (i - slotCount));
            if (roles[i] == "out") outs[i].push_back(float(frac(mem.get(MemArea_X, addr))));
        }
        if (trace && s < 8) {
            std::cerr << "sample " << s << ":";
            for (size_t i = 0; i < roles.size(); ++i)
                std::cerr << " " << frac(mem.get(MemArea_X, i < slotCount ? TWord(kZp + i) : TWord(watchAddr + (i - slotCount))));
            std::cerr << "  Y:"; for (size_t i = 0; i < Y.size(); ++i) std::cerr << " " << frac(mem.get(MemArea_Y, TWord(kYBase + i)));
            std::cerr << "\n";
        }
    }
    std::cerr << samples << " samples, " << steps << " instructions\n";
    for (size_t i = 0; i < roles.size(); ++i) {
        if (roles[i] != "out" || outs[i].empty()) continue;
        double lo = 1e9, hi = -1e9; int crossings = 0;
        for (size_t k = 0; k < outs[i].size(); ++k) {
            lo = std::min(lo, double(outs[i][k])); hi = std::max(hi, double(outs[i][k]));
            if (k && (outs[i][k - 1] < 0) != (outs[i][k] < 0)) ++crossings;
        }
        std::printf("out slot %zu: min %.4f max %.4f, zero crossings %d (~%.1f Hz)\n", i, lo, hi, crossings, crossings / 2.0 * fs / samples);
        if (!wav.empty()) { // 32-bit float mono WAV of the first output
            std::ofstream w(wav, std::ios::binary);
            auto u32 = [&](uint32_t v) { w.write((const char*)&v, 4); }; auto u16 = [&](uint16_t v) { w.write((const char*)&v, 2); };
            const uint32_t bytes = uint32_t(outs[i].size() * 4);
            w.write("RIFF", 4); u32(36 + bytes); w.write("WAVEfmt ", 8); u32(16); u16(3); u16(1); u32(96000); u32(96000 * 4); u16(4); u16(32);
            w.write("data", 4); u32(bytes); w.write((const char*)outs[i].data(), bytes);
            wav.clear();
        }
    }
    return 0;
}
