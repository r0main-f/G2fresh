// Port of the original G2 editor's ParamText namespace (Mac v1.62, i386).
// Each function mirrors the original arithmetic (float vs double steps and
// printf formats included) so the strings match the original editor. Known
// quirks of the original are kept and marked "quirk". See
// re/notes/param-display.md for the derivation.
#include "g2/param_text.hpp"

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>

// The original was compiled for SSE without fused multiply-add; contracting
// a*b+c into an FMA would change the last bit of some results. (GCC does not
// contract in ISO mode, -std=c++20, which is what the project uses.)
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace g2::paramtext {

namespace {

// "±" in UTF-8 (the original used Mac Roman 0xB1).
constexpr const char* kPlusMinus = "\xC2\xB1";

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
std::string fmt(const char* f, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, f);
    const int n = std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    if (n < 0) return {};
    return std::string(buf, static_cast<std::size_t>(n < int(sizeof buf) ? n : int(sizeof buf) - 1));
}

template <std::size_t N>
std::string table(const std::array<const char*, N>& t, u8 v)
{
    return v < N ? std::string(t[v]) : std::string();
}

std::string table2(const char* a, const char* b, u8 v)
{
    return v == 0 ? a : v == 1 ? b : "";
}

std::string table3(const char* a, const char* b, const char* c, u8 v)
{
    return v == 0 ? a : v == 1 ? b : v == 2 ? c : "";
}

std::string intToStr(long v, const char* unit = "")
{
    return fmt("%ld", v) + unit;
}

// logf/expf of the original libm are modelled as the double function rounded
// to float (correctly rounded in practice); this keeps the output identical
// across platforms whose own expf may be off by one ulp.
float logF(float x) { return float(std::log(double(x))); }
float expF(float x) { return float(std::exp(double(x))); }

const float kLn2f = logF(2.0f); // the original calls logf(2.0f)

// FloatToStrSci: three significant digits with m/k prefixes.
// quirk: the "< 0.01" branch ("%.2fm") is overwritten by the "< 0.1" one.
std::string floatToStrSci(float v, const char* unit)
{
    const double d = v;
    if (d < 0.1) return fmt("%.1fm%s", double(v * 1000.0f), unit);
    if (v < 1.0f) return fmt("%.0fm%s", double(v * 1000.0f), unit);
    if (v < 10.0f) return fmt("%.3f%s", d, unit);
    if (v < 100.0f) return fmt("%.2f%s", d, unit);
    if (v < 1000.0f) return fmt("%.1f%s", d, unit);
    if (v < 10000.0f) return fmt("%.2fk%s", double(v / 1000.0f), unit);
    return fmt("%.1fk%s", double(v / 1000.0f), unit);
}

// FloatToStrSci6: one more digit than FloatToStrSci (used for oscillator Hz).
std::string floatToStrSci6(float v, const char* unit)
{
    const double d = v;
    if (d < 0.01) return fmt("%.3fm%s", double(v * 1000.0f), unit);
    if (d < 0.1) return fmt("%.2fm%s", double(v * 1000.0f), unit);
    if (v < 1.0f) return fmt("%.1fm%s", double(v * 1000.0f), unit);
    if (v < 10.0f) return fmt("%.4f%s", d, unit);
    if (v < 100.0f) return fmt("%.3f%s", d, unit);
    if (v < 1000.0f) return fmt("%.2f%s", d, unit);
    if (v < 10000.0f) return fmt("%.3fk%s", double(v / 1000.0f), unit);
    return fmt("%.2fk%s", double((v + 5.0f) / 1000.0f), unit);
}

// FloatToStrTime: seconds in, "m" (milliseconds) or "s" out. No unit is
// appended after "m".
std::string floatToStrTime(float seconds)
{
    const float ms = seconds * 1000.0f;
    if (ms < 10.0f) return fmt("%.2fm", double(ms));
    if (ms < 100.0f) return fmt("%.1fm", double(ms));
    if (ms < 999.5f) return fmt("%.0fm", double(ms));
    if (ms < 10000.0f) return fmt("%.2fs", double(ms / 1000.0f));
    return fmt("%.1fs", double((ms + 50.0f) / 1000.0f));
}

std::string floatToStrTimeShort(float seconds)
{
    float ms = seconds * 1000.0f;
    if (ms < 10.0f) return fmt("%.1fm", double(ms));
    if (ms < 999.5f) return fmt("%.0fm", double(ms));
    if (ms >= 10000.0f) ms += 50.0f;
    return fmt("%.1fs", double(ms / 1000.0f));
}

// FloatToString(buf, fixed16_16, 2): two decimals, rounded on the fraction.
std::string fixed16ToStr2(std::uint32_t v)
{
    std::uint32_t whole = v >> 16;
    std::uint32_t frac = ((v & 0xFFFF) * 100 + 0x8000) >> 16;
    if (frac > 99) {
        ++whole;
        frac = 0;
    }
    return fmt("%u.%02u", whole & 0xFFFF, frac);
}

// Semitone tuning, (note + fine/128 - 69) / 12 octaves from A440, in float.
float noteRatiof(u8 coarse, u8 fine)
{
    const float x = ((float(coarse) + (float(fine) - 64.0f) * 0.0078125f) - 69.0f) / 12.0f;
    return expF(x * kLn2f);
}

double centsOf(u8 fine) { return double((float(fine) - 64.0f) * 50.0f * 0.015625f); }

} // namespace

// Tables copied from the original's data section (ParamText statics).
namespace {

constexpr const char* kEnvelopeTime[128] = {
    " 0.5m", " 0.6m", " 0.7m", " 0.9m", " 1.1m", " 1.3m", " 1.5m", " 1.8m",
    " 2.1m", " 2.5m", " 3.0m", " 3.5m", " 4.0m", " 4.7m", " 5.5m", " 6.3m",
    " 7.3m", " 8.4m", " 9.7m", "11.1m", "12.7m", "14.5m", "16.5m", "18.7m",
    "21.2m", "24.0m", "27.1m", "30.6m", "34.4m", "38.7m", "43.4m", "48.6m",
    "54.3m", "60.6m", "67.6m", "75.2m", "83.6m", "92.8m", " 103m", " 114m",
    " 126m", " 139m", " 153m", " 169m", " 186m", " 204m", " 224m", " 246m",
    " 269m", " 295m", " 322m", " 352m", " 384m", " 419m", " 456m", " 496m",
    " 540m", " 586m", " 636m", " 690m", " 748m", " 810m", " 876m", " 947m",
    "1.02s", "1.10s", "1.19s", "1.28s", "1.38s", "1.49s", "1.60s", "1.72s",
    "1.85s", "1.99s", "2.13s", "2.28s", "2.45s", "2.62s", "2.81s", "3.00s",
    "3.21s", "3.43s", "3.66s", "3.91s", "4.17s", "4.45s", "4.74s", "5.05s",
    "5.37s", "5.72s", "6.08s", "6.47s", "6.87s", "7.30s", "7.75s", "8.22s",
    "8.72s", "9.25s", "9.80s", "10.4s", "11.0s", "11.6s", "12.3s", "13.0s",
    "13.8s", "14.6s", "15.4s", "16.2s", "17.1s", "18.1s", "19.1s", "20.1s",
    "21.2s", "22.4s", "23.5s", "24.8s", "26.1s", "27.5s", "28.9s", "30.4s",
    "32.0s", "33.6s", "35.3s", "37.1s", "38.9s", "40.9s", "42.9s", "45.0s",
};

constexpr const char* kNoiseGateAtkTime[128] = {
    " 0.2m", " 0.3m", " 0.4m", " 0.5m", " 0.6m", " 0.8m", " 0.9m", " 1.0m",
    " 1.2m", " 1.4m", " 1.6m", " 1.7m", " 2.0m", " 2.2m", " 2.4m", " 2.6m",
    " 2.9m", " 3.1m", " 3.4m", " 3.7m", " 4.0m", " 4.3m", " 4.6m", " 4.9m",
    " 5.3m", " 5.6m", " 6.0m", " 6.3m", " 6.7m", " 7.1m", " 7.5m", " 7.9m",
    " 8.4m", " 8.8m", " 9.3m", " 9.7m", "10.2m", "10.7m", "11.2m", "11.7m",
    "12.2m", "12.7m", "13.3m", "13.8m", "14.4m", "14.9m", "15.5m", "16.1m",
    "16.7m", "17.4m", "18.0m", "18.6m", "19.3m", "19.9m", "20.6m", "21.3m",
    "22.0m", "22.7m", "23.4m", "24.1m", "24.9m", "25.6m", "26.4m", "27.2m",
    "28.0m", "28.8m", "29.6m", "30.4m", "31.2m", "32.1m", "32.9m", "33.8m",
    "34.6m", "35.5m", "36.4m", "37.3m", "38.3m", "39.2m", "40.1m", "41.1m",
    "42.0m", "43.0m", "44.0m", "45.0m", "46.0m", "47.0m", "48.1m", "49.1m",
    "50.2m", "51.2m", "52.3m", "53.4m", "54.5m", "55.6m", "56.7m", "57.9m",
    "59.0m", "60.2m", "61.3m", "62.5m", "63.7m", "64.9m", "66.1m", "67.3m",
    "68.6m", "69.8m", "71.1m", "72.3m", "73.6m", "74.9m", "76.2m", "77.5m",
    "78.8m", "80.2m", "81.5m", "82.9m", "84.2m", "85.6m", "87.0m", "88.4m",
    "89.8m", "91.2m", "92.7m", "94.1m", "95.6m", "97.0m", "98.5m", " 100m",
};

constexpr const char* kCompressorAttack[128] = {
    "Fast", "0.53m", "0.56m", "0.59m", "0.63m", "0.67m", "0.71m", "0.75m",
    "0.79m", "0.84m", "0.89m", "0.94m", "1.00m", "1.06m", "1.12m", "1.19m",
    "1.26m", "1.33m", "1.41m", "1.50m", "1.59m", "1.68m", "1.78m", "1.89m",
    "2.00m", "2.12m", "2.24m", "2.38m", "2.52m", "2.67m", "2.83m", "3.00m",
    "3.17m", "3.36m", "3.56m", "3.78m", "4.00m", "4.24m", "4.49m", "4.76m",
    "5.04m", "5.34m", "5.66m", "5.99m", "6.35m", "6.73m", "7.13m", "7.55m",
    "8.00m", "8.48m", "8.98m", "9.51m", "10.1m", "10.7m", "11.3m", "12.0m",
    "12.7m", "13.5m", "14.3m", "15.1m", "16.0m", "17.0m", "18.0m", "19.0m",
    "20.2m", "21.4m", "22.6m", "24.0m", "25.4m", "26.9m", "28.5m", "30.2m",
    "32.0m", "33.9m", "35.9m", "38.1m", "40.3m", "42.7m", "45.3m", "47.9m",
    "50.8m", "53.8m", "57.0m", "60.4m", "64.0m", "67.8m", "71.8m", "76.1m",
    "80.6m", "85.4m", "90.5m", "95.9m", " 102m", " 108m", " 114m", " 121m",
    " 128m", " 136m", " 144m", " 152m", " 161m", " 171m", " 181m", " 192m",
    " 203m", " 215m", " 228m", " 242m", " 256m", " 271m", " 287m", " 304m",
    " 323m", " 342m", " 362m", " 384m", " 406m", " 431m", " 456m", " 483m",
    " 512m", " 542m", " 575m", " 609m", " 645m", " 683m", " 724m", " 767m",
};

constexpr const char* kCompressorRelease[128] = {
    " 125m", " 129m", " 134m", " 139m", " 144m", " 149m", " 154m", " 159m",
    " 165m", " 171m", " 177m", " 183m", " 189m", " 196m", " 203m", " 210m",
    " 218m", " 225m", " 233m", " 241m", " 250m", " 259m", " 268m", " 277m",
    " 287m", " 297m", " 308m", " 319m", " 330m", " 342m", " 354m", " 366m",
    " 379m", " 392m", " 406m", " 420m", " 435m", " 451m", " 467m", " 483m",
    " 500m", " 518m", " 536m", " 555m", " 574m", " 595m", " 616m", " 637m",
    " 660m", " 683m", " 707m", " 732m", " 758m", " 785m", " 812m", " 841m",
    " 871m", " 901m", " 933m", " 966m", "1.00s", "1.04s", "1.07s", "1.11s",
    "1.15s", "1.19s", "1.23s", "1.27s", "1.32s", "1.37s", "1.41s", "1.46s",
    "1.52s", "1.57s", "1.62s", "1.68s", "1.74s", "1.80s", "1.87s", "1.93s",
    "2.00s", "2.07s", "2.14s", "2.22s", "2.30s", "2.38s", "2.46s", "2.55s",
    "2.64s", "2.73s", "2.83s", "2.93s", "3.03s", "3.14s", "3.25s", "3.36s",
    "3.48s", "3.61s", "3.73s", "3.86s", "4.00s", "4.14s", "4.29s", "4.44s",
    "4.59s", "4.76s", "4.92s", "5.10s", "5.28s", "5.46s", "5.66s", "5.86s",
    "6.06s", "6.28s", "6.50s", "6.73s", "6.96s", "7.21s", "7.46s", "7.73s",
    "8.00s", "8.28s", "8.57s", "8.88s", "9.19s", "9.51s", "9.85s", "10.2s",
};

constexpr std::int32_t kEnvDcyMult[128] = {
    5715092, 6120511, 6467022, 6761176, 7009758, 7219245, 7395527, 7543798, 7668543, 7773584,
    7862145, 7936930, 8000197, 8053827, 8099382, 8138163, 8171250, 8199544, 8223792, 8244621,
    8262552, 8278022, 8291399, 8302990, 8313054, 8321811, 8329445, 8336114, 8341951, 8347070,
    8351566, 8355523, 8359011, 8362092, 8364817, 8367231, 8369373, 8371277, 8372972, 8374482,
    8375831, 8377036, 8378115, 8379083, 8379951, 8380731, 8381433, 8382065, 8382636, 8383151,
    8383616, 8384038, 8384420, 8384766, 8385080, 8385366, 8385626, 8385863, 8386079, 8386276,
    8386455, 8386620, 8386770, 8386908, 8387034, 8387150, 8387256, 8387354, 8387444, 8387526,
    8387602, 8387672, 8387737, 8387797, 8387852, 8387903, 8387950, 8387993, 8388034, 8388071,
    8388106, 8388138, 8388168, 8388196, 8388221, 8388245, 8388268, 8388289, 8388308, 8388326,
    8388343, 8388359, 8388373, 8388387, 8388400, 8388412, 8388423, 8388433, 8388443, 8388452,
    8388461, 8388469, 8388477, 8388484, 8388491, 8388497, 8388503, 8388508, 8388514, 8388519,
    8388523, 8388528, 8388532, 8388535, 8388539, 8388543, 8388546, 8388549, 8388552, 8388555,
    8388557, 8388560, 8388562, 8388564, 8388566, 8388568, 8388570, 8388572,
};

constexpr std::int32_t kEnvLinAdd[128] = {
    699050, 574209, 473895, 392870, 327100, 273460, 229515, 193356, 163481, 138699,
    118064, 100819, 86357, 74187, 63912, 55211, 47820, 41524, 36145, 31537,
    27580, 24172, 21231, 18687, 16480, 14562, 12892, 11434, 10159, 9042,
    8061, 7198, 6438, 5766, 5173, 4647, 4181, 3767, 3398, 3069,
    2776, 2514, 2279, 2069, 1880, 1711, 1558, 1421, 1297, 1185,
    1084, 992, 909, 834, 766, 704, 647, 596, 549, 506,
    467, 431, 399, 369, 341, 316, 293, 272, 252, 234,
    218, 203, 189, 176, 164, 153, 142, 133, 124, 116,
    108, 101, 95, 89, 83, 78, 73, 69, 65, 61,
    57, 54, 50, 47, 45, 42, 40, 37, 35, 33,
    31, 30, 28, 26, 25, 24, 22, 21, 20, 19,
    18, 17, 16, 15, 14, 14, 13, 12, 12, 11,
    10, 10, 9, 9, 8, 8, 8, 7,
};

constexpr std::int32_t kVibratoRateDisp[128] = {
    262144, 264208, 266272, 268336, 270401, 272465, 274529, 276593, 278657, 280721,
    282785, 284849, 286914, 288978, 291042, 293106, 295170, 297234, 299298, 301362,
    303427, 305491, 307555, 309619, 311683, 313747, 315811, 317875, 319940, 322004,
    324068, 326132, 328196, 330260, 332324, 334388, 336453, 338517, 340581, 342645,
    344709, 346773, 348837, 350901, 352966, 355030, 357094, 359158, 361222, 363286,
    365350, 367414, 369479, 371543, 373607, 375671, 377735, 379799, 381863, 383927,
    385992, 388056, 390120, 392184, 394248, 396312, 398376, 400440, 402505, 404569,
    406633, 408697, 410761, 412825, 414889, 416953, 419018, 421082, 423146, 425210,
    427274, 429338, 431402, 433466, 435531, 437595, 439659, 441723, 443787, 445851,
    447915, 449979, 452044, 454108, 456172, 458236, 460300, 462364, 464428, 466492,
    468557, 470621, 472685, 474749, 476813, 478877, 480941, 483005, 485070, 487134,
    489198, 491262, 493326, 495390, 497454, 499518, 501583, 503647, 505711, 507775,
    509839, 511903, 513967, 516031, 518096, 520160, 522224, 524288,
};

constexpr std::int32_t kNoiseGateRelMult[128] = {
    5715092, 6044476, 6298676, 6523910, 6723194, 6899390, 7055147, 7192872, 7314729, 7422638,
    7518299, 7603207, 7678672, 7745840, 7805714, 7859166, 7906961, 7949765, 7988159, 8022652,
    8053689, 8081660, 8106906, 8129726, 8150385, 8169114, 8186117, 8201576, 8215650, 8228480,
    8240192, 8250896, 8260691, 8269666, 8277900, 8285461, 8292414, 8298813, 8304710, 8310150,
    8315173, 8319817, 8324113, 8328092, 8331781, 8335203, 8338382, 8341337, 8344086, 8346646,
    8349031, 8351256, 8353332, 8355272, 8357085, 8358782, 8360370, 8361858, 8363252, 8364561,
    8365789, 8366943, 8368028, 8369048, 8370008, 8370912, 8371764, 8372568, 8373325, 8374041,
    8374716, 8375354, 8375958, 8376528, 8377068, 8377580, 8378064, 8378523, 8378959, 8379372,
    8379764, 8380136, 8380490, 8380826, 8381145, 8381449, 8381738, 8382013, 8382275, 8382525,
    8382763, 8382990, 8383206, 8383413, 8383610, 8383798, 8383978, 8384150, 8384314, 8384471,
    8384621, 8384765, 8384902, 8385034, 8385160, 8385280, 8385396, 8385507, 8385613, 8385715,
    8385813, 8385907, 8385997, 8386084, 8386167, 8386247, 8386324, 8386398, 8386468, 8386537,
    8386602, 8386665, 8386726, 8386785, 8386841, 8386895, 8386948, 8386998,
};

constexpr std::int32_t kEnvFollowRelease[128] = {
    40144, 38385, 36703, 35094, 33556, 32085, 30678, 29333, 28047, 26817,
    25641, 24517, 23442, 22414, 21430, 20490, 19591, 18732, 17910, 17124,
    16373, 15655, 14968, 14311, 13683, 13082, 12508, 11959, 11434, 10932,
    10453, 9994, 9555, 9136, 8735, 8351, 7985, 7634, 7299, 6978,
    6672, 6379, 6099, 5831, 5575, 5330, 5096, 4873, 4659, 4454,
    4259, 4072, 3893, 3722, 3558, 3402, 3253, 3110, 2973, 2843,
    2718, 2598, 2484, 2375, 2271, 2171, 2076, 1985, 1897, 1814,
    1734, 1658, 1585, 1516, 1449, 1385, 1325, 1266, 1211, 1158,
    1107, 1058, 1012, 967, 925, 884, 845, 808, 773, 739,
    706, 675, 645, 617, 590, 564, 539, 516, 493, 471,
    450, 431, 412, 394, 376, 360, 344, 329, 314, 301,
    287, 275, 263, 251, 240, 229, 219, 210, 200, 192,
    183, 175, 167, 160, 153, 146, 140, 134,
};

constexpr std::int32_t kEnvFollowAttack[128] = {
    8388607, 724814, 684475, 646285, 610140, 575942, 543592, 512999, 484074, 456733,
    430893, 406478, 383412, 361626, 341051, 321623, 303280, 285965, 269623, 254199,
    239645, 225912, 212956, 200734, 189205, 178331, 168075, 158404, 149284, 140685,
    132577, 124932, 117726, 110932, 104528, 98491, 92801, 87438, 82384, 77620,
    73130, 68899, 64912, 61155, 57614, 54278, 51135, 48172, 45381, 42752,
    40274, 37940, 35740, 33668, 31716, 29877, 28144, 26512, 24974, 23525,
    22160, 20874, 19663, 18522, 17447, 16434, 15480, 14582, 13735, 12938,
    12187, 11479, 10813, 10185, 9594, 9037, 8512, 8018, 7552, 7113,
    6700, 6311, 5945, 5599, 5274, 4968, 4679, 4408, 4152, 3910,
    3683, 3469, 3268, 3078, 2899, 2731, 2572, 2423, 2282, 2149,
    2024, 1907, 1796, 1692, 1593, 1501, 1414, 1331, 1254, 1181,
    1113, 1048, 987, 930, 876, 825, 777, 732, 689, 649,
    611, 576, 542, 511, 481, 453, 427, 402,
};

constexpr std::int32_t kAmpGainNew[128] = {
    0, 21845, 43690, 65536, 87381, 109226, 131072, 152917, 174762, 196608,
    218453, 240298, 262144, 283989, 305834, 327680, 349525, 371370, 393216, 415061,
    436906, 458752, 480597, 502442, 524288, 542776, 561917, 581733, 602248, 623487,
    645474, 668236, 691802, 716198, 741455, 767602, 794672, 822696, 851708, 881743,
    912838, 945029, 978356, 1012857, 1048576, 1085553, 1123835, 1163467, 1204497, 1246974,
    1290948, 1336473, 1383604, 1432397, 1482910, 1535205, 1589344, 1645392, 1703416, 1763487,
    1825676, 1890059, 1956712, 2025715, 2097152, 2143073, 2190000, 2237955, 2286960, 2337038,
    2388212, 2440507, 2493948, 2548558, 2604364, 2661392, 2719669, 2779222, 2840079, 2902269,
    2965820, 3030763, 3097128, 3164947, 3234250, 3305071, 3377443, 3451399, 3526975, 3604205,
    3683127, 3763777, 3846193, 3930414, 4016479, 4104428, 4194304, 4289143, 4386126, 4485303,
    4586722, 4690435, 4796492, 4904948, 5015856, 5129271, 5245251, 5363854, 5485139, 5609165,
    5735997, 5865696, 5998328, 6133959, 6272656, 6414490, 6559531, 6707851, 6859525, 7014629,
    7173240, 7335437, 7501302, 7670917, 7844368, 8021740, 8203123, 8388607,
};

constexpr std::uint16_t kPortamentoTimes[128] = {
    155, 164, 175, 186, 197, 210, 223, 237, 253, 269, 286, 305,
    325, 346, 369, 393, 419, 447, 477, 508, 542, 578, 616, 657,
    701, 747, 797, 849, 905, 964, 1026, 1092, 1162, 1235, 1313, 1394,
    1480, 1569, 1663, 1760, 1862, 1967, 2076, 2189, 2305, 2425, 2547, 2672,
    2800, 2930, 3062, 3195, 3330, 3466, 3602, 3739, 3876, 4013, 4149, 4286,
    4421, 4556, 4690, 4824, 4957, 5089, 5220, 5351, 5481, 5612, 5742, 5872,
    6003, 6135, 6267, 6401, 6536, 6673, 6813, 6955, 7099, 7247, 7399, 7555,
    7715, 7881, 8051, 8228, 8411, 8601, 8799, 9005, 9220, 9444, 9680, 9926,
    10185, 10457, 10743, 11046, 11366, 11704, 12063, 12444, 12851, 13284, 13748, 14245,
    14779, 15355, 15978, 16653, 17387, 18190, 19070, 20039, 21113, 22308, 23647, 25156,
    26870, 28837, 31113, 33780, 36947, 40767, 45471, 51402,
};

constexpr const char* kDelaySync[32] = {
    "1/64T", "1/64", "1/32T", "1/64D", "1/32", "1/16T", "1/32D", "1/16",
    "1/16", "1/8T", "1/8T", "1/16D", "1/16D", "1/8", "1/8", "1/4T",
    "1/4T", "1/8D", "1/8D", "1/4", "1/4", "1/2T", "1/2T", "1/4D",
    "1/4D", "1/2", "1/2", "1/1T", "1/2D", "1/1", "1/1D", "2/1",
};

constexpr const char* kLfoSyncRatios[32] = {
    "64/1", "48/1", "32/1", "24/1", "16/1", "12/1", "8/1", "6/1",
    "4/1", "3/1", "2/1", "1/1D", "1/1", "1/2D", "1/1T", "1/2",
    "1/4D", "1/2T", "1/4", "1/8D", "1/4T", "1/8", "1/16D", "1/8T",
    "1/16", "1/32D", "1/16T", "1/32", "1/64D", "1/32T", "1/64", "1/64T",
};

} // namespace

// --- generic scales ----------------------------------------------------------

std::string From0to100(u8 v)
{
    switch (v) {
    case 0: return "0";
    case 32: return "25";
    case 64: return "50";
    case 96: return "75";
    case 127: return "100";
    default: return fmt("%.1f", double(float(v) * 100.0f * 0.0078125f));
    }
}

std::string Default(u8 v) { return From0to100(v); }
std::string VocoderLevel(u8 v) { return From0to100(v); }

std::string From0to200Percent(u8 v)
{
    std::string s;
    switch (v) {
    case 0: s = "0"; break;
    case 16: s = "25"; break;
    case 32: s = "50"; break;
    case 48: s = "75"; break;
    case 64: s = "100"; break;
    case 80: s = "125"; break;
    case 96: s = "150"; break;
    case 112: s = "175"; break;
    case 127: s = "200"; break;
    default: s = fmt("%.1f", double(float(v) * 200.0f * 0.0078125f));
    }
    return s + "%";
}

std::string DualDefault(u8 a, u8) { return intToStr(a); }
std::string TripleDefault(u8 a, u8, u8) { return intToStr(a); }

std::string UniPol(u8 v)
{
    if (v == 127) return "64.0";
    return fmt((v & 1) ? "%d.5" : "%d.0", v >> 1);
}

std::string UniPolCompact(u8 v)
{
    if (v == 127) return "64";
    return fmt((v & 1) ? "%d." : "%d", v >> 1);
}

std::string BiPol(u8 v) { return v == 127 ? "64" : fmt("%d", v - 64); }
std::string UniPolShort(u8 v) { return v == 63 ? "64" : fmt("%d", v); }
std::string Sym(u8 v) { return fmt(v < 65 ? "%d" : "+%d", int(v) - 64); }
std::string BiUniPol(u8 v, u8 polarity) { return polarity == 0 ? BiPol(v) : UniPol(v); }
std::string BiUniPolCompact(u8 v, u8 polarity) { return polarity == 0 ? BiPol(v) : UniPolCompact(v); }
std::string MultiEnvBiUni(u8 v, u8 outType) { return outType < 4 ? UniPol(v) : BiPol(v); }
std::string Enum(u8 v) { return fmt("%d", v + 1); }
std::string Negative(u8 v) { return fmt("%d", -int(v)); }
std::string OffNum(u8 v) { return v == 0 ? "Off" : fmt("%d", v); }
std::string MIDIValue(u8 v) { return intToStr(v); }
std::string SwitchCtrl(u8 v) { return intToStr(long(v) << 2); }
std::string Phase(u8 v) { return fmt("%d", (int(v) * 0x2D000 - 0xB40000) >> 16); }
std::string OscCoarse(u8 v) { return fmt("%+3d", v - 64); }
std::string OscFine(u8 v) { return fmt("%+5.1f", centsOf(v)); }
std::string OscSemi2(u8 coarse, u8 fine) { return fmt("%+3d %+4.0f", coarse - 64, centsOf(fine)); }

std::string PitchShift(u8 semi, u8 fine)
{
    return fmt("%+2.1f %+4.0f", double((float(semi) - 64.0f) * 0.25f), centsOf(fine));
}

std::string LevMult(u8 mode, u8 v)
{
    if (mode != 0) return From0to100(v);
    return intToStr(long(v) * 2 - 128); // quirk: the "-127"/"127" special cases are overwritten
}

// --- notes ---------------------------------------------------------------------

std::string Semitone(u8 v)
{
    static constexpr std::array<const char*, 12> kNotes = {"A", "Bb", "B", "C", "C#", "D",
                                                           "D#", "E", "F", "F#", "G", "G#"};
    return fmt("%s%d", kNotes[(v + 3) % 12], v / 12 - 1);
}

std::string SemitoneKey(u8 v)
{
    std::string s = Semitone(v);
    if (v == 64) s += " Key";
    return s;
}

std::string SemitoneFilter(u8 v) { return SemitoneKey(u8(v + 4)); }
std::string DXBreakPoint(u8 v) { return Semitone(u8(v + 9)); }

std::string PlusMinusHalfSteps(u8 v)
{
    if (v == 0) return "0";
    if (v == 127) return std::string(kPlusMinus) + "64";
    return kPlusMinus + fmt("%.*f%s", 1, double(float(double(v) * 0.5)), "");
}

std::string NoteScaler(u8 v)
{
    std::string s = PlusMinusHalfSteps(v);
    switch ((v + 120) % 24) {
    case 0: s += "-Oct"; break;
    case 14: s += "-5th"; break;
    case 20: s += "-7th"; break;
    default: break;
    }
    return s;
}

std::string NotePlusMinus(u8 v)
{
    if (v == 127) return std::string(kPlusMinus) + "64.0";
    return kPlusMinus + fmt((v & 1) ? "%d.5" : "%d.0", v >> 1);
}

std::string PartialGen(u8 v)
{
    std::string s = kPlusMinus + fmt("%d", v >> 1);
    if (v > 64) s += "*";
    return s;
}

// --- enumerations ------------------------------------------------------------------

std::string OnOff(u8 v) { return table2("Off", "On", v); }
std::string InactiveActive(u8 v) { return table2("Inact", "Active", v); }
std::string Mono(u8 v) { return table2("Poly", "Mono", v); }
std::string Flip(u8 v) { return table2("Normal", "Inverted", v); }
std::string GainCompensation(u8 v) { return table2("GC Off", "GC On", v); }
std::string GCOnOff(u8 v) { return table2("GC Off", "GC On", v); }
std::string Mute(u8 v) { return table2("Muted", "Active", v); }
std::string Bypass(u8 v) { return table2("Bypass", "Active", v); }
std::string EnvReset(u8 v) { return table2("Normal", "Reset", v); }
std::string EnvADRSust(u8 v) { return table2("AD", "AR", v); }
std::string Loop(u8 v) { return table2("1-Cycle", "Loop", v); }
std::string Rec(u8 v) { return table2("Off", "Rec On", v); }
std::string KBTOnOff(u8 v) { return table2("KBT Off", "KBT On", v); }
std::string OscWaveSine(u8 v) { return table2("Off", "Sine", v); }
std::string Polarity(u8 v) { return table2("Bipol", "Unipol", v); }
std::string TrigGate(u8 v) { return table2("Trig", "Gate", v); }
std::string GoStop(u8 v) { return table2("Go", "Stop", v); }
std::string dB_6_12(u8 v) { return table2("6dB", "12dB", v); }
std::string dB_12_24(u8 v) { return table2("12dB", "24dB", v); }
std::string HiLo(u8 v) { return table2("Low", "High", v); }
std::string Emphasis(u8 v) { return table2("Off", "On", v); }
std::string ClipSym(u8 v) { return table2("Asym", "Sym", v); }
std::string VocoderMon(u8 v) { return table2("Active", "Monitor", v); }
std::string OverDriveSym(u8 v) { return table2("Asym", "Sym", v); }
std::string PhaserType(u8 v) { return table2("Type I", "Type II", v); }
std::string BendOnOff(u8 v) { return table2("Off", "On", v); }
std::string ModAmountMode(u8 v) { return table2("m", "1-m", v); }
std::string KeyQuantCaptureRange(u8 v) { return table2("Closest", "Evenly", v); }
std::string MIDIClockKeyboardTrig(u8 v) { return table2("Off", "On", v); }
std::string EnvAttackType(u8 v) { return table3("Exp", "Lin", "Log", v); }
std::string FmType(u8 v) { return table3("FM A", "FM B", "FM C", v); }
std::string FilterCdB(u8 v) { return table3("12dB", "18dB", "24dB", v); }
std::string EnvMultiType(u8 v) { return table3("Bipolar", "Uni/Exp", "Uni/Lin", v); }
std::string dB_0_6_12(u8 v) { return table3("0dB", "-6dB", "-12dB", v); }
std::string OutBusBDest(u8 v) { return table3("1/2", "3/4", "CVA", v); }
std::string MonoKeybPrio(u8 v) { return table3("Last", "Low", "High", v); }

#define G2_TABLE(name, ...)                                              \
    std::string name(u8 v)                                               \
    {                                                                    \
        static constexpr std::array kItems = std::to_array<const char*>({__VA_ARGS__}); \
        return table(kItems, v);                                         \
    }

G2_TABLE(FltVariant, "Notch", "Peak", "Deep")
G2_TABLE(ClkGenSync, "1", "2", "4", "8", "16", "32")
G2_TABLE(EnvAttackDecayShape, "LogExp", "LinExp", "ExpExp", "LinLin")
G2_TABLE(LFOWaveFull, "Sine", "Tri", "Saw", "SawN", "Sqr")
G2_TABLE(EqFreqHi, "6 kHz", "8 kHz", "12 kHz")
G2_TABLE(EqFreqLo, "80 Hz", "110 Hz", "160 Hz")
G2_TABLE(OscWaveFull, "Sqr", "Saw", "Tri", "Sine")
G2_TABLE(LevelShift, "Pos", "PosInv", "Neg", "NegInv", "Bip", "BipInv", "Bip", "BipInv")
G2_TABLE(RndLevelShift, "Bip", "Pos", "Neg")
G2_TABLE(SignalType, "Bipol", "Pos", "Neg")
G2_TABLE(XFade, "Off", "25%", "50%", "100%")
G2_TABLE(OscAWave, "Sine", "Tri", "Saw", "Sqr50", "Sqr25", "Sqr10")
G2_TABLE(OscBWave, "Sine", "Tri", "Saw", "Sqr", "DualSaw")
G2_TABLE(OscSinShpWave, "Sine1", "Sine2", "Sine3", "Sine4", "TriSaw", "Pulse")
G2_TABLE(OscTuneMode, "Semi", "Freq", "Factor", "Partial", "Sub")
G2_TABLE(DelayTimeMode, "Time", "ClkSync")
G2_TABLE(Out2Dest, "Out 1/2", "Out 3/4", "FX 1/2", "FX 3/4", "Bus 1/2", "Bus 3/4")
G2_TABLE(Out4Dest, "Out", "FX", "Bus")
G2_TABLE(In2Src, "In 1/2", "In 3/4", "Bus 1/2", "Bus 3/4")
G2_TABLE(In4Src, "In", "Bus")
G2_TABLE(InCVA, "FX 1/2", "FX 3/4")
G2_TABLE(InputPad, "+6 dB", " 0 dB", "-6dB", "-12dB")
G2_TABLE(OutPad, " 0 dB", "+6 dB", "+12dB", "+18dB")
G2_TABLE(FltKBT, "KBT Off", "KBT 25%", "KBT 50%", "KBT 75%", "KBT 100")
G2_TABLE(LfoKBT, "KBT Off", "KBT 25%", "KBT 50%", "KBT 75%", "KBT 100")
G2_TABLE(FmKBT, "FM Lin", "FM Trk")
G2_TABLE(FilterType, "BR", "HP", "BP", "LP")
G2_TABLE(Vowels, "A", "E", "I", "O", "U", "Y", "AA", "AE", "OE")
G2_TABLE(DiodeModes, "HalfPos", "HalfNeg", "FullPos", "FullNeg")
G2_TABLE(ShaperModes, "Inv x3", "Inv x2", "x2", "x3")
G2_TABLE(ShapeExpCurve, "x2", "x3", "x4", "x5")
G2_TABLE(ReverbType, "Room", "Hall", "Plate", "Gate")
G2_TABLE(EnvMultiSusPlace, "L1", "L2", "L3", "Trg")
G2_TABLE(EnvADDSRSusPlace, "L1", "L2")
G2_TABLE(LFORange, "Sub", "Lo", "Hi", "BPM", "ClkSync")
G2_TABLE(LfoAWave, "Sine", "Tri", "Saw", "Sqr", "RndStep", "Rnd")
G2_TABLE(LfoBWave, "Sine", "Tri", "Saw", "Sqr")
G2_TABLE(LfoCWave, "Sine", "CosBell", "TriBell", "Saw2Tri", "Sqr2Tri", "Sqr")
G2_TABLE(RndSmooth, "0%", "25%", "50%", "75%", "100%")
G2_TABLE(RndStepSwitch, "25%", "50%", "75%", "100%")
G2_TABLE(RndLogic, "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%")
G2_TABLE(PatchMIDIOutChannel, "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13",
         "14", "15", "16", "This", "SlotA", "SlotB", "SlotC", "SlotD")
G2_TABLE(PatchMIDIInChannel, "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13",
         "14", "15", "16", "This", "Keyb")
G2_TABLE(PosNeg, "Pos", "Neg")
G2_TABLE(AmpType, "Lin", "dB")
G2_TABLE(ExpLin, "Exp", "Lin", "dB")
G2_TABLE(LogLin, "Log", "Lin")
G2_TABLE(PortamentoType, "C-Rate", "C-Time")
G2_TABLE(OverDriveType, "Soft", "Hard", "Fat", "Heavy")
G2_TABLE(PTrackAlgorithm, "Normal", "HiPitch")
G2_TABLE(PitchShiftDelay, "12.5m", "25m", "50m", "100m")
G2_TABLE(MIDIFilter, "Notes", "Note+CC")
G2_TABLE(MIDIEcho, "EchoOff", "EchoOn")
G2_TABLE(DXCurve, "-Lin", "-Exp", "+Exp", "+Lin")
G2_TABLE(DXTuneMode, "Ratio", "Fixed")
G2_TABLE(DrumFilterType, "LP", "BP", "HP")
G2_TABLE(OffOnOn, "Off", "On", "On")
G2_TABLE(MorphGroupSource1, "Knob", "Wheel")
G2_TABLE(MorphGroupSource2, "Knob", "Vel")
G2_TABLE(MorphGroupSource3, "Knob", "Keyb")
G2_TABLE(MorphGroupSource4, "Knob", "Aft.Tch")
G2_TABLE(MorphGroupSource5, "Knob", "Sust.Pd", "G.Wh 1")
G2_TABLE(MorphGroupSource6, "Knob", "Ctrl.Pd")
G2_TABLE(MorphGroupSource7, "Knob", "P.Stick")
G2_TABLE(MorphGroupSource8, "Knob", "G.Wh 2")
G2_TABLE(ArpeggiatorRange, "1 Oct", "2 Oct", "3 Oct", "4 Oct")
G2_TABLE(ArpeggiatorRate, "1/8", "1/8T", "1/16", "1/16T")
G2_TABLE(ArpeggiatorMode, "Up", "Down", "Up/Down", "Rnd")
G2_TABLE(FilterCType, "LP", "BP", "HP", "BR")
G2_TABLE(FilterCSlope, "12dB", "24dB")

#undef G2_TABLE

std::string ClkGenSource(u8 v) { return v == 0 ? "Intern" : "Master"; }
std::string OscKBT(u8 v) { return v == 0 ? "KBT Off" : "KBT On"; }
std::string MIDIChannel(u8 v) { return v == 16 ? "Off" : fmt("%d", v + 1); }

std::string Pad(u8 v)
{
    switch (v) {
    case 0: return "0dB";
    case 1: return "-6dB";
    case 2: return "-12dB";
    default: return "";
    }
}

std::string VibratoSource(u8 v)
{
    switch (v) {
    case 0: return "Off";
    case 1: return "Aftouch";
    case 2: return "Wheel";
    default: return "";
    }
}

std::string PortamentoMode(u8 v)
{
    switch (v) {
    case 0: return "Off";
    case 1: return "Normal";
    case 2: return "Auto";
    default: return "";
    }
}

std::string AmRm(u8 v)
{
    if (v == 0) return "None";
    if (v == 64) return "Am";
    if (v == 127) return "Rm";
    return fmt("%d", v);
}

std::string Fade2to1(u8 v)
{
    if (v == 64) return "Mute";
    if (v < 64) return v == 0 ? "I1:127" : fmt("I1:%d", (64 - v) * 2);
    return v == 127 ? "I2:127" : fmt("I2:%d", v * 2 - 128);
}

std::string Fade1to2(u8 v)
{
    if (v == 64) return "Mute";
    if (v < 64) return v == 0 ? "O1:127" : fmt("O1:%d", (64 - v) * 2);
    return v == 127 ? "O2:127" : fmt("O2:%d", v * 2 - 128);
}

// --- percentages, counts --------------------------------------------------------

std::string PulseWidth(u8 v) { return intToStr(((v * 99) >> 7) + 1, "%"); }
std::string PulseWidthHalf(u8 v) { return fmt("%.0f%c", (double(v) * 49.0) / 127.0 + 50.0, '%'); }
std::string RndStep(u8 v) { return fmt("%u%c", (v * 100u) / 127u, '%'); }
std::string RndDistribution(u8 v) { return RndStep(v); }
std::string RndDensity(u8 v) { return RndStep(v); }
std::string LfoShape(u8 v) { return fmt("%d%s", int((std::int64_t(v) * 0xC58B16 + 0x1000000) >> 24), "%"); }
std::string ClkGenSwing(u8 v) { return fmt("%.1f%%", double(float(v) / 5.08f + 50.0f)); }
std::string LFOPhase(u8 v) { return fmt("%.0f", double(float(v) * 360.0f * 0.0078125f)); }
std::string OscPhase(u8 v) { return LFOPhase(v); }
std::string VibratoAmount(u8 v) { return fmt("%3d cnt", v); }
std::string BendRange(u8 v) { return fmt("%d semi", v + 1); }
std::string OctShift(u8 v) { return fmt(v > 2 ? "+%d oct" : "%d oct", int(v) - 2); }
std::string ControlPedalGain(u8 v) { return fmt("x%2.2f", double(float(v) * 0.015625f + 1.0f)); }
std::string DigitizerBits(u8 v) { return v == 12 ? " Off" : fmt("%d", v + 1); }
std::string DXDetune(u8 v) { return intToStr(long(v) - 7); }
std::string CompressorThreshold(u8 v) { return v == 42 ? " Off" : fmt("%3ddB", v - 30); }
std::string CompressorLevel(u8 v) { return fmt("%3ddB", v - 30); }

std::string CompressorRatio(u8 v)
{
    int u = v > 34 ? v - 35 : v;
    int r = u < 10 ? u + 10 : u < 25 ? u * 2 : u * 5 - 75; // ratio * 10
    if (v > 34) r *= 10;
    if (r < 100) return fmt("%d.%d:1", r / 10, r % 10);
    return fmt(" %2d:1", r / 10);
}

std::string BPM(u8 v)
{
    const int bpm = v > 96 ? v * 2 - 40 : v < 32 ? v * 2 + 24 : v + 56;
    return intToStr(bpm, "BPM");
}

std::string ClkGenTempo(u8 rate, u8 active, u8 source)
{
    if (active == 0) return "--";
    return source == 0 ? BPM(rate) : "Master";
}

// --- times ---------------------------------------------------------------------

std::string EnvelopeTime(u8 v) { return v < 128 ? kEnvelopeTime[v] : ""; }
std::string NoiseGateAtkTime(u8 v) { return v < 128 ? kNoiseGateAtkTime[v] : ""; }
std::string CompressorAttack(u8 v) { return v < 128 ? kCompressorAttack[v] : ""; }
std::string CompressorRelease(u8 v) { return v < 128 ? kCompressorRelease[v] : ""; }

std::string DelayTime(u8 v) { return floatToStrSci(float((double(v) * 2.5) / 127.0), "s"); }

std::string ReverbTime(u8 time, u8 roomType)
{
    return floatToStrSci(float((double(float(roomType * 3 + 3) * float(time))) / 127.0), "s");
}

std::string Smooth(u8 v)
{
    const double t = std::exp((double(v) / 127.0) * std::log(1000.0));
    return floatToStrSci(float(t * 0.0003183098861837907), "s");
}

std::string EnvFollowerAttack(u8 v)
{
    if (v >= 128) return "";
    if (v == 0) return "Fast";
    const double r = std::log10(0.01) / std::log10(1.0 - double(kEnvFollowAttack[v]) * 1.1920928955078125e-07);
    return floatToStrTime(float(r / 96000.0));
}

std::string EnvFollowerRelease(u8 v)
{
    if (v >= 128) return "";
    const double r = std::log10(0.01) / std::log10(1.0 - double(kEnvFollowRelease[v]) * 1.1920928955078125e-07);
    return floatToStrTime(float(r / 96000.0));
}

std::string NoiseGateRelTime(u8 v)
{
    if (v >= 128) return "";
    const double r = std::log10(0.01) / std::log10(double(kNoiseGateRelMult[v]) * 1.1920928955078125e-07);
    return floatToStrTime(float(r / 24000.0));
}

std::string PortamentoTime(u8 v)
{
    if (v >= 128) return "";
    float t = float(kPortamentoTimes[v]) * 0.0001220703125f;
    if (t >= 1.0f) return fmt("%.2fs", double(t));
    t *= 1000.0f;
    return fmt("%.0fm", double(t));
}

std::string GlideTimeRate(u8 v, u8 shape)
{
    if (v >= 128) return "";
    if (shape == 0) {
        const double r = std::log(double(kEnvDcyMult[v] * 2 - 0x800000) * 1.1920928955078125e-07);
        return floatToStrTimeShort(float(std::log(0.01) / (r * 24000.0)));
    }
    // quirk: the original first copies a fixed string for v > 114 and then
    // overwrites it with the computed value.
    return floatToStrTimeShort(float(245760.0 / double(kEnvLinAdd[v] * 1500))) + "/oct";
}

std::string Slide(u8 v)
{
    const double t = std::exp((double(v) / 127.0) * std::log(255.60377358490567));
    return fmt("%.*f%s", 0, double(float(t * 0.0053 * 1000.0)), "ms");
}

std::string LogicTime(u8 time, u8 range)
{
    const double x = double(time) * -0.000643966018031 + 0.933269244944;
    double p = x * x * x * x;
    p = p * p;
    p = p * x * p * p;
    p = p * p;
    double ms = 1.0 / (p * p * 96000.0);
    ms *= range == 0 ? 10.0 : range == 1 ? 100.0 : 1000.0;
    if (ms < 10.0) return fmt("%.2fm", ms);
    if (ms < 100.0) return fmt("%.1fm", ms + 0.005);
    if (ms < 999.9) return fmt("%.0fm", ms + 0.05);
    if (ms < 10000.0) return fmt("%.2fs", (ms + 5.0) / 1000.0);
    return fmt("%.1fs", (ms + 50.0) / 1000.0);
}

namespace {

constexpr std::array<int, 3> kRangeStereo = {378, 756, 1021};
constexpr std::array<int, 7> kRangeTap8 = {4, 19, 76, 378, 756, 1512, 2041};
constexpr std::array<int, 4> kRangeFx = {378, 756, 1512, 2041};
constexpr std::array<int, 7> kRangeTap = {4, 19, 76, 378, 756, 1512, 2041};

// DelayTimeTapHelp: sync 0 = time in samples at 96 kHz, 1 = clock-synced note
// value, anything else leaves the text empty.
std::string delayTimeTapHelp(u8 time, u8 sync, double range, bool eighth)
{
    if (sync == 1) return kDelaySync[time >> 2];
    if (sync != 0) return "";
    double t = double(time) * range + 1.0;
    if (eighth) t *= 0.125;
    t /= 96000.0;
    if (t < 0.01) return fmt("%.2fm", t * 1000.0);
    if (t < 0.1) return fmt("%.1fm", t * 1000.0);
    if (t < 1.0) return fmt("%.0fm", t * 1000.0);
    return fmt("%.3fs", t);
}

} // namespace

std::string DelayTimeStereo(u8 time, u8 sync, u8 range)
{
    return range < kRangeStereo.size() ? delayTimeTapHelp(time, sync, kRangeStereo[range], false) : "";
}

std::string DelayTimeTap8(u8 time, u8 range)
{
    return range < kRangeTap8.size() ? delayTimeTapHelp(time, 0, kRangeTap8[range], true) : "";
}

std::string DelayTimeFx(u8 time, u8 sync, u8 range)
{
    return range < kRangeFx.size() ? delayTimeTapHelp(time, sync, kRangeFx[range], false) : "";
}

std::string DelayTimeTap(u8 time, u8 sync, u8 range)
{
    return range < kRangeTap.size() ? delayTimeTapHelp(time, sync, kRangeTap[range], false) : "";
}

std::string DelayTime2(u8 time, u8 range) { return DelayTimeTap(time, 0, range); }

// --- rates and frequencies -------------------------------------------------------

std::string VibratoRate(u8 v)
{
    return v < 128 ? fixed16ToStr2(std::uint32_t(kVibratoRateDisp[v])) + " Hz" : "";
}

std::string FlangerRate(u8 v)
{
    const double hz = v == 0 ? 0.011444091796875 : double(std::uint32_t(v) * 0x5DC00u) * 5.9604644775390625e-08;
    return fmt("%.*fHz", 2, hz);
}

std::string PhaserRate(u8 v)
{
    const int sq = (int(v) * int(v)) >> 1;
    return fmt("%.*fHz", v < 119 ? 2 : 1, double(sq * 24000 + 0xBB800) * 5.9604644775390625e-08);
}

std::string SampleRate(u8 v)
{
    const double r = std::exp(((double(v) - 69.0) / 12.0) * double(kLn2f));
    return floatToStrSci(float(r * 1760.0), "Hz");
}

std::string FilterCutOff(u8 v)
{
    const double r = std::exp(((double(v) - 65.0) / 12.0) * double(kLn2f));
    return floatToStrSci(float(r * 440.0), "Hz");
}

std::string FilterCutOff2(u8 v)
{
    const double r = std::exp(((double(v) - 60.0) / 12.0) * double(kLn2f));
    return floatToStrSci(float(r * 440.0), "Hz");
}

std::string OnePoleFreq(u8 v)
{
    const float l = logF(1666.6666259765625f);
    return floatToStrSci(float(std::exp((double(v) * double(l)) / 127.0) * 12.0), "Hz");
}

std::string PhaserFreq(u8 v)
{
    const float l = logF(160.0f);
    return floatToStrSci(expF((float(v) * l) / 127.0f) * 100.0f, "Hz");
}

std::string EqFreq(u8 v)
{
    const float l = logF(800.0f);
    return floatToStrSci(expF((float(v) * l) / 127.0f) * 20.0f, "Hz");
}

std::string EqFreq100To8k(u8 v)
{
    const float l = logF(80.0f);
    const double hz = double(expF((float(v) * l) / 127.0f) * 100.0f);
    if (hz < 1000.0) return fmt("%.0f%s", hz, "Hz");
    if (hz < 10000.0) return fmt("%.2fk%s", hz / 1000.0, "Hz");
    return fmt("%.1fk%s", hz / 1000.0, "Hz");
}

std::string DrumMstFreq(u8 v)
{
    const double note = double(u8((v >> 1) + 16)) + (double(v & 1) * 64.0 - 64.0) * 0.0078125;
    const double hz = std::exp(((note - 69.0) / 12.0) * double(kLn2f)) * 440.0;
    return fmt("%.*f%s", hz < 100.0 ? 1 : 0, double(float(hz)), "Hz");
}

std::string LFOFreq(u8 rate, u8 range)
{
    double f;
    switch (range) {
    case 0: { // Sub: period in seconds
        const double s = 699.0506666667 / (double(rate) + 1.0);
        const int prec = s < 0.1 ? 1 : s < 10.0 ? 2 : s < 100.0 ? 1 : 0;
        return fmt("%.*f%s", prec, double(float(s)), "s");
    }
    case 1: f = std::exp((double(rate) / 12.0) * double(kLn2f)) * 0.0159; break;
    case 2: f = std::exp((double(rate) / 12.0) * double(kLn2f)) * 0.2555; break;
    case 3: return fmt("%d", rate > 96 ? rate * 2 - 40 : rate < 32 ? rate * 2 + 24 : rate + 56);
    case 4: return kLfoSyncRatios[rate >> 2];
    default: f = 699.0506666667 / (double(rate) + 1.0); break;
    }
    if (f < 0.1) return fmt("%.*f%s", 1, double(float(1.0 / f)), "s");
    if (f < 10.0) return fmt("%.*f%s", 2, double(float(f)), "Hz");
    if (f < 100.0) return fmt("%.*f%s", 1, double(float(f)), "Hz");
    return fmt("%.*f%s", 0, double(float(f)), "Hz");
}

std::string LFOFreq1(u8 v) { return LFOFreq(v, 1); }

std::string BodeFreq(u8 v, u8 range)
{
    const float x = float(v);
    float hz = x * x * x;
    if (range == 0) hz *= 8.78f;
    else if (range == 1) hz *= 97.6f;
    else if (range == 2) hz *= 1568.0f;
    hz /= 2048383.0f;
    const int prec = hz < 0.1f ? 3 : hz < 10.0f ? 2 : hz < 100.0f ? 1 : 0;
    return fmt("%.*f%s", prec, double(hz), "Hz");
}

std::string OscFreq(u8 coarse, u8 fine) { return floatToStrSci6(noteRatiof(coarse, fine) * 440.0f, "Hz"); }
std::string OscFreqSingle(u8 v) { return OscFreq(v, 64); }

std::string OscFactor(u8 coarse, u8 fine)
{
    const double r = (double(noteRatiof(coarse, fine)) * 440.0) / 329.6275569;
    return fmt("x%.*f", r <= 10.0 ? 4 : 3, r);
}

std::string OscSubFreq(u8 coarse, u8 fine)
{
    if (coarse == 0) return "0 Hz";
    return fmt("%.*f%s", 4, double(noteRatiof(coarse, fine) * 0.21484375f), "Hz");
}

std::string OscPartials(u8 coarse, u8 fine)
{
    if (coarse == 0) return "0 Hz";
    if (coarse < 33) {
        const float hz = noteRatiof(u8(coarse * 4 - 4), fine) * 0.21484375f;
        return fmt("%.3fHz", double(hz));
    }
    if (coarse < 64) return fmt("1:%d %+4.0f", 65 - coarse, centsOf(fine));
    return fmt("%d:1 %+4.0f", coarse - 63, centsOf(fine));
}

std::string OscSyncTimbre(u8 a, u8 b) { return a != 0 ? fmt("+%d", b) : OscFreq(b, 64); }
std::string OscPulseTimbre(u8 a, u8 b) { return OscSyncTimbre(a, b); }

std::string OscFreqDep(u8 coarse, u8 fine, u8 tuneMode)
{
    switch (tuneMode) {
    case 0: return fmt("%+3d %+4.0f", coarse - 64, centsOf(fine));
    case 1: return OscFreq(coarse, fine);
    case 2: return OscFactor(coarse, fine);
    case 3: return OscPartials(coarse, fine);
    default: return OscSubFreq(coarse, fine);
    }
}

std::string DXOscFreq(u8 coarse, u8 fine, u8 fixedMode)
{
    if (fixedMode == 0) {
        const double r = coarse == 0 ? 0.5 : double(coarse);
        return fmt("x%.*f", 2, r + (double(fine) * r) / 100.0);
    }
    const double decade = std::pow(10.0, double(coarse & 3));
    const double f = std::pow(10.0, double(fine) / 100.0);
    if (decade == 1.0) return fmt("%.*fHz", 3, f);
    if (decade == 10.0) return fmt("%.*fHz", 2, f * 10.0);
    if (decade == 100.0) return fmt("%.*fHz", 1, f * 100.0);
    return fmt("%.*f Hz", 0, decade * f);
}

// Shared by LfoRateMult: shows a frequency ratio either as "xN.NN" or, when
// it is close to an integer ratio, as "N:M".
static std::string slaveMult(short semis, short fine128, u8 prec)
{
    const double base = double(semis);
    const double r = std::exp(((double(fine128) * 0.0078125 + base) / 12.0) * double(kLn2f));
    double err, errNext;
    if (r < 1.0) {
        const double n = std::floor(1.0 / r + 0.5);
        err = n + -1.0 / r;
        const int step = err <= 0.0 ? 1 : -1;
        const double rn = std::exp(((double(fine128 + step) * 0.0078125 + base) / 12.0) * std::log(2.0));
        errNext = -1.0 / rn + n;
    } else {
        const double n = std::floor(r + 0.5);
        err = n - r;
        const int step = err >= 0.0 ? 1 : -1;
        const double rn = std::exp(((double(fine128 + step) * 0.0078125 + base) / 12.0) * std::log(2.0));
        errNext = n - rn;
    }
    if (std::fabs(errNext) < std::fabs(err) || errNext * err > 0.0) {
        if (r < 10.0) ++prec;
        return "x" + fmt("%.*f%s", int(prec), double(float(r)), "");
    }
    unsigned num = 1, den = 1;
    if (semis > 0) num = unsigned(int(std::floor(r + 0.5)) & 0xFF);
    else if (semis < 0) den = unsigned(int(std::floor(1.0 / r + 0.5)) & 0xFF);
    return fmt("%u:%u", num, den);
}

std::string LfoRateMult(u8 v) { return slaveMult(short(int(v) - 64), 0, 2); }

std::string DrumSlvMult(u8 v)
{
    const double semis = std::floor(double(v >> 2) + 0.5);
    const double fine = double(float(v & 3) * 32.0f);
    const double r = std::exp(((fine * 0.0078125 + semis) / 12.0) * double(kLn2f));
    double err, errNext;
    if (r < 1.0) {
        const double n = std::floor(1.0 / r + 0.5);
        err = n + -1.0 / r;
        const double step = err <= 0.0 ? 1.0 : -1.0;
        const double rn = std::exp((((fine + step) * 0.0078125 + semis) / 12.0) * std::log(2.0));
        errNext = -1.0 / rn + n;
    } else {
        const double n = std::floor(r + 0.5);
        err = n - r;
        const double step = err >= 0.0 ? 1.0 : -1.0;
        const double rn = std::exp((((fine + step) * 0.0078125 + semis) / 12.0) * std::log(2.0));
        errNext = n - rn;
    }
    if (std::fabs(errNext) < std::fabs(err) || errNext * err > 0.0)
        return "x" + fmt("%.*f%s", r < 10.0 ? 2 : 1, double(float(r)), "");
    const unsigned num = semis > 0.0 ? unsigned(int(std::floor(r + 0.5)) & 0xFF) : 1u;
    const unsigned den = semis < 0.0 ? unsigned(int(std::floor(1.0 / r + 0.5)) & 0xFF) : 1u;
    return fmt("%u:%u", num, den);
}

// --- levels and gains ---------------------------------------------------------------

std::string dB(u8 v)
{
    switch (v) {
    case 0: return "-oo";
    case 1: return "-99.9";
    case 2: return "-99.0";
    case 127: return "-0";
    default: break;
    }
    const double x = double(v) / 127.0;
    const double db = std::log10(x * 0.01 + x * (x * 0.99 * x)) * 20.0;
    // quirk: the original tests "db >= 10" (never true) to drop the decimal.
    return fmt("%.*f%s", db >= 10.0 ? 0 : 1, double(float(db)), "");
}

std::string dBLin(u8 v, u8 mode) { return mode == 2 ? dB(v) : From0to100(v); }

std::string NoiseGateLevel(u8 v)
{
    if (v == 0) return "-oo";
    if (v == 127) return "-0dB";
    const double db = std::log10(double(v) / 127.0) * 20.0;
    return fmt("%.*f%s", db < 10.0 ? 1 : 0, double(float(db)), "") + "dB";
}

std::string VolumeDb(u8 v)
{
    const double g = std::exp(double(127 - int(v)) * (std::log(5.333333333333333) / 127.0)) * 18.0 - 18.0;
    return fmt(g >= 10.0 ? "-%.0f dB" : "-%.1f dB", g);
}

std::string EqGain(u8 v)
{
    const double x = v == 127 ? 128.0 : double(v);
    return fmt("%.*f%s", 1, double(float((x - 64.0) * 0.28125)), "dB");
}

std::string NoteVelScale(u8 v)
{
    if (v == 127) return "8.0dB";
    return fmt("%.*f%s", 1, double(float(double(float(int(v) - 64)) * 8.0 * 0.015625)), "dB");
}

std::string EQParBW(u8 v)
{
    return fmt("%.*f%s", 2, double((128.0f - float(v)) * 0.015625f), "Oct");
}

std::string AmpGain(u8 v)
{
    const float g = v == 127 ? 4.0f : float(std::exp(double(v) * std::log(16.0) * 0.0078125) * 0.25);
    return "x" + fmt("%.*f%s", 2, double(g), "");
}

std::string AmpGainNew(u8 v, u8 type)
{
    if (v >= 128) return "";
    const double g = double(kAmpGainNew[v]) * 4.76837158203125e-07;
    if (type == 0) return "x" + fmt("%.*f%s", 2, double(float(g)), "");
    if (g == 0.0) return "-oo";
    return fmt("%.*f%s", 1, double(float(std::log10(g) * 20.0)), "");
}

std::string FilterResonance(u8 v)
{
    float x = float((double(v) * 0.9) / -127.0 + 1.0);
    x = float(0.5 / double(x * x));
    return fmt("%.*f%s", x >= 10.0f ? 0 : 2, double(x), "");
}

std::string KBT(u8 v)
{
    if (v == 64) return "Key";
    if (v == 127) return "x2";
    if (v == 0) return "Off";
    return "x" + fmt("%.*f%s", 2, double(float(v) * 0.015625f), "");
}

std::string PitchShuttle(u8 v)
{
    if (v == 64) return "x0";
    if (v == 127) return "x4.00";
    const int steps = v < 65 ? 64 - v : v - 64;
    const std::string num = fmt("%.*f%s", 2, double(float(steps) * 0.015625f * 4.0f), "");
    return (v < 65 ? "- x" : "x") + num;
}

// --- registration tables (ParamText::Get{Singel,Dual,Triple}DependencyTextFunction) ---

namespace {

template <typename Fn>
struct Entry {
    int id;
    const char* name;
    Fn fn;
};

using Single = std::string (*)(u8);
using Dual = std::string (*)(u8, u8);
using Triple = std::string (*)(u8, u8, u8);

constexpr Entry<Single> kSingle[] = {
    {1, "Negative", &Negative},
    {2, "Enum", &Enum},
    {3, "OnOff", &OnOff},
    {4, "Mono", &Mono},
    {5, "Flip", &Flip},
    {6, "GainCompensation", &GainCompensation},
    {7, "Mute", &Mute},
    {8, "Bypass", &Bypass},
    {9, "Loop", &Loop},
    {10, "Rec", &Rec},
    {11, "OffNum", &OffNum},
    {12, "Phase", &Phase},
    {13, "Semitone", &Semitone},
    {14, "SemitoneKey", &SemitoneKey},
    {15, "SemitoneFilter", &SemitoneFilter},
    {16, "UniPol", &UniPol},
    {17, "BiPol", &BiPol},
    {18, "Polarity", &Polarity},
    {19, "Sym", &Sym},
    {20, "DelayTime", &DelayTime},
    {21, "FilterCutOff", &FilterCutOff},
    {22, "DrumMstFreq", &DrumMstFreq},
    {23, "DrumSlvMult", &DrumSlvMult},
    {24, "OnePoleFreq", &OnePoleFreq},
    {25, "KBT", &KBT},
    {26, "KBTOnOff", &KBTOnOff},
    {27, "PulseWidth", &PulseWidth},
    {28, "EnvelopeTime", &EnvelopeTime},
    {29, "EnvAttackType", &EnvAttackType},
    {30, "LFORange", &LFORange},
    {31, "LFOWaveFull", &LFOWaveFull},
    {32, "LfoRateMult", &LfoRateMult},
    {33, "Slide", &Slide},
    {34, "Smooth", &Smooth},
    {35, "PlusMinusHalfSteps", &PlusMinusHalfSteps},
    {36, "EqGain", &EqGain},
    {37, "AmpGain", &AmpGain},
    {38, "EqFreq", &EqFreq},
    {39, "PhaserFreq", &PhaserFreq},
    {40, "SampleRate", &SampleRate},
    {41, "OscWaveFull", &OscWaveFull},
    {42, "OscWaveSine", &OscWaveSine},
    {43, "TrigGate", &TrigGate},
    {44, "GoStop", &GoStop},
    {45, "BPM", &BPM},
    {46, "LevelShift", &LevelShift},
    {47, "OnOff", &OnOff},
    {48, "EnvAttackType", &EnvAttackType},
    {49, "EnvADDSRSusPlace", &EnvADDSRSusPlace},
    {50, "EnvMultiSusPlace", &EnvMultiSusPlace},
    {51, "EnvMultiType", &EnvMultiType},
    {52, "Fade2to1", &Fade2to1},
    {53, "Fade1to2", &Fade1to2},
    {54, "AmpGain", &AmpGain},
    {55, "AmRm", &AmRm},
    {57, "dB_0_6_12", &dB_0_6_12},
    {58, "OscAWave", &OscAWave},
    {59, "OscFine", &OscFine},
    {61, "BiPol", &BiPol},
    {62, "FmType", &FmType},
    {63, "OscTuneMode", &OscTuneMode},
    {64, "Enum", &Enum},
    {66, "Enum", &Enum},
    {67, "OutBusBDest", &OutBusBDest},
    {68, "PartialGen", &PartialGen},
    {69, "NoteScaler", &NoteScaler},
    {70, "NoteVelScale", &NoteVelScale},
    {71, "FltKBT", &FltKBT},
    {72, "GCOnOff", &GCOnOff},
    {73, "dB_6_12", &dB_6_12},
    {74, "dB_12_24", &dB_12_24},
    {75, "FilterCType", &FilterCType},
    {76, "EQParBW", &EQParBW},
    {77, "HiLo", &HiLo},
    {78, "Vowels", &Vowels},
    {79, "OffNum", &OffNum},
    {80, "Emphasis", &Emphasis},
    {81, "VocoderMon", &VocoderMon},
    {82, "ClipSym", &ClipSym},
    {83, "DiodeModes", &DiodeModes},
    {84, "ShaperModes", &ShaperModes},
    {85, "PhaserFreq", &PhaserFreq},
    {86, "Enum", &Enum},
    {87, "DelayTime", &DelayTime},
    {88, "SampleRate", &SampleRate},
    {89, "Enum", &Enum},
    {90, "ReverbType", &ReverbType},
    {91, "Enum", &Enum},
    {92, "Enum", &Enum},
    {93, "FilterCdB", &FilterCdB},
    {94, "Enum", &Enum},
    {99, "SwitchCtrl", &SwitchCtrl},
    {100, "SwitchCtrl", &SwitchCtrl},
    {101, "SwitchCtrl", &SwitchCtrl},
    {104, "LFORange", &LFORange},
    {105, "LfoKBT", &LfoKBT},
    {106, "LfoAWave", &LfoAWave},
    {108, "PatchMIDIOutChannel", &PatchMIDIOutChannel},
    {109, "PatchMIDIInChannel", &PatchMIDIInChannel},
    {111, "ArpeggiatorRange", &ArpeggiatorRange},
    {112, "ArpeggiatorRate", &ArpeggiatorRate},
    {113, "ArpeggiatorMode", &ArpeggiatorMode},
    {114, "UniPolShort", &UniPolShort},
    {115, "PortamentoTime", &PortamentoTime},
    {116, "PortamentoMode", &PortamentoMode},
    {117, "BendOnOff", &BendOnOff},
    {118, "VolumeDb", &VolumeDb},
    {119, "VibratoAmount", &VibratoAmount},
    {120, "VibratoSource", &VibratoSource},
    {121, "OctShift", &OctShift},
    {123, "FilterCutOff2", &FilterCutOff2},
    {124, "FilterResonance", &FilterResonance},
    {125, "FmKBT", &FmKBT},
    {126, "PulseWidthHalf", &PulseWidthHalf},
    {127, "SignalType", &SignalType},
    {128, "LfoShape", &LfoShape},
    {129, "EnvFollowerAttack", &EnvFollowerAttack},
    {130, "EnvFollowerRelease", &EnvFollowerRelease},
    {131, "LFOFreq1", &LFOFreq1},
    {132, "NotePlusMinus", &NotePlusMinus},
    {134, "XFade", &XFade},
    {135, "PosNeg", &PosNeg},
    {136, "EnvAttackDecayShape", &EnvAttackDecayShape},
    {138, "EnvReset", &EnvReset},
    {139, "EnvADRSust", &EnvADRSust},
    {144, "DelayTimeMode", &DelayTimeMode},
    {148, "AmpType", &AmpType},
    {149, "InputPad", &InputPad},
    {150, "In2Src", &In2Src},
    {151, "In4Src", &In4Src},
    {152, "InCVA", &InCVA},
    {153, "dB", &dB},
    {155, "OscSinShpWave", &OscSinShpWave},
    {156, "OscBWave", &OscBWave},
    {157, "LogLin", &LogLin},
    {158, "PortamentoType", &PortamentoType},
    {159, "NoiseGateAtkTime", &NoiseGateAtkTime},
    {160, "NoiseGateRelTime", &NoiseGateRelTime},
    {161, "BendOnOff", &BendOnOff},
    {162, "BendRange", &BendRange},
    {163, "LFOPhase", &LFOPhase},
    {164, "LfoBWave", &LfoBWave},
    {165, "LfoCWave", &LfoCWave},
    {166, "ClkGenSync", &ClkGenSync},
    {167, "OverDriveType", &OverDriveType},
    {168, "OverDriveSym", &OverDriveSym},
    {169, "ExpLin", &ExpLin},
    {170, "PhaserType", &PhaserType},
    {171, "VibratoRate", &VibratoRate},
    {172, "FltVariant", &FltVariant},
    {173, "OscFreqSingle", &OscFreqSingle},
    {174, "CompressorAttack", &CompressorAttack},
    {175, "CompressorRelease", &CompressorRelease},
    {176, "CompressorThreshold", &CompressorThreshold},
    {177, "CompressorRatio", &CompressorRatio},
    {178, "CompressorLevel", &CompressorLevel},
    {179, "MIDIValue", &MIDIValue},
    {180, "OutPad", &OutPad},
    {181, "MonoKeybPrio", &MonoKeybPrio},
    {182, "Out2Dest", &Out2Dest},
    {183, "Out4Dest", &Out4Dest},
    {184, "EqFreq100To8k", &EqFreq100To8k},
    {185, "OscPhase", &OscPhase},
    {186, "InactiveActive", &InactiveActive},
    {187, "ClkGenSource", &ClkGenSource},
    {188, "Pad", &Pad},
    {189, "ModAmountMode", &ModAmountMode},
    {190, "ClkGenSwing", &ClkGenSwing},
    {191, "From0to200Percent", &From0to200Percent},
    {192, "ShapeExpCurve", &ShapeExpCurve},
    {193, "PTrackAlgorithm", &PTrackAlgorithm},
    {194, "KeyQuantCaptureRange", &KeyQuantCaptureRange},
    {195, "DigitizerBits", &DigitizerBits},
    {196, "DXDetune", &DXDetune},
    {197, "DXBreakPoint", &DXBreakPoint},
    {199, "DXCurve", &DXCurve},
    {200, "DXTuneMode", &DXTuneMode},
    {202, "PitchShiftDelay", &PitchShiftDelay},
    {203, "RndSmooth", &RndSmooth},
    {204, "RndLevelShift", &RndLevelShift},
    {205, "RndStep", &RndStep},
    {206, "RndDistribution", &RndDistribution},
    {207, "RndStepSwitch", &RndStepSwitch},
    {208, "RndLogic", &RndLogic},
    {209, "PitchShuttle", &PitchShuttle},
    {210, "MIDIFilter", &MIDIFilter},
    {211, "MIDIEcho", &MIDIEcho},
    {212, "RndDensity", &RndDensity},
    {213, "EqFreqLo", &EqFreqLo},
    {214, "EqFreqHi", &EqFreqHi},
    {215, "FlangerRate", &FlangerRate},
    {216, "PhaserRate", &PhaserRate},
    {218, "DrumFilterType", &DrumFilterType},
    {219, "OffOnOn", &OffOnOn},
    {220, "NoiseGateLevel", &NoiseGateLevel},
    {221, "MorphGroupSource1", &MorphGroupSource1},
    {222, "MorphGroupSource2", &MorphGroupSource2},
    {223, "MorphGroupSource3", &MorphGroupSource3},
    {224, "MorphGroupSource4", &MorphGroupSource4},
    {225, "MorphGroupSource5", &MorphGroupSource5},
    {226, "MorphGroupSource6", &MorphGroupSource6},
    {227, "MorphGroupSource7", &MorphGroupSource7},
    {228, "MorphGroupSource8", &MorphGroupSource8},
};

constexpr Entry<Dual> kDual[] = {
    {0, "OscSubFreq", &OscSubFreq},
    {65, "OscSemi2", &OscSemi2},
    {95, "LevMult", &LevMult},
    {96, "BiUniPol", &BiUniPol},
    {97, "OscSyncTimbre", &OscSyncTimbre},
    {98, "OscPulseTimbre", &OscPulseTimbre},
    {102, "dBLin", &dBLin},
    {103, "LFOFreq", &LFOFreq},
    {107, "ReverbTime", &ReverbTime},
    {122, "LogicTime", &LogicTime},
    {133, "BiUniPolCompact", &BiUniPolCompact},
    {137, "MultiEnvBiUni", &MultiEnvBiUni},
    {141, "DelayTime2", &DelayTime2},
    {142, "BodeFreq", &BodeFreq},
    {145, "DelayTimeTap8", &DelayTimeTap8},
    {147, "AmpGainNew", &AmpGainNew},
    {201, "PitchShift", &PitchShift},
    {217, "GlideTimeRate", &GlideTimeRate},
};

constexpr Entry<Triple> kTriple[] = {
    {60, "OscFreqDep", &OscFreqDep},
    {110, "ClkGenTempo", &ClkGenTempo},
    {140, "DelayTimeTap", &DelayTimeTap},
    {143, "DelayTimeFx", &DelayTimeFx},
    {146, "DelayTimeStereo", &DelayTimeStereo},
    {198, "DXOscFreq", &DXOscFreq},
};

template <typename Fn, std::size_t N>
const Entry<Fn>* find(const Entry<Fn> (&table)[N], int id)
{
    for (const auto& e : table)
        if (e.id == id) return &e;
    return nullptr;
}

} // namespace

std::string formatSingle(int id, u8 v)
{
    const auto* e = find(kSingle, id);
    return e ? e->fn(v) : Default(v);
}

std::string formatDual(int id, u8 v, u8 dep1)
{
    const auto* e = find(kDual, id);
    return e ? e->fn(v, dep1) : DualDefault(v, dep1);
}

std::string formatTriple(int id, u8 v, u8 dep1, u8 dep2)
{
    const auto* e = find(kTriple, id);
    return e ? e->fn(v, dep1, dep2) : TripleDefault(v, dep1, dep2);
}

std::string format(int infoFunc, std::span<const u8> values)
{
    switch (values.size()) {
    case 0: return {};
    case 1: return formatSingle(infoFunc, values[0]);
    case 2: return formatDual(infoFunc, values[0], values[1]);
    default: return formatTriple(infoFunc, values[0], values[1], values[2]);
    }
}

const char* functionName(int id, int arity)
{
    switch (arity) {
    case 1: {
        const auto* e = find(kSingle, id);
        return e ? e->name : "Default";
    }
    case 2: {
        const auto* e = find(kDual, id);
        return e ? e->name : "DualDefault";
    }
    case 3: {
        const auto* e = find(kTriple, id);
        return e ? e->name : "TripleDefault";
    }
    default: return "";
    }
}

} // namespace g2::paramtext
