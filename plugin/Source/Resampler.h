// The emulated G2's audio at the host's sample rate, and the host's at the G2's: a band-limited sample rate
// conversion, as a converter does it (re/notes/audio-path.md).
//
// The G2 computes at 96 kHz, so its outputs hold partials up to 48 kHz. A real G2 sends them out through its DACs;
// whoever records it at 48 kHz does so through an audio interface whose converter removes what its rate cannot
// hold before it samples. Taking every second sample instead (what an interpolator without a filter does) folds
// everything above 24 kHz back into the audible band, at full level: partials that are not in the G2's sound. The
// other way, an input brought to 96 kHz without a filter comes with copies of itself above the host's Nyquist
// frequency, and loses treble.
//
// The filter: a sinc at the lower of the two rates' Nyquist frequency under a Kaiser window, linear phase.
//   pass band   flat (within 0.01 dB) up to 20 kHz at 44.1 and 48 kHz (0.4535 x the rate at lower ones; the same
//               share of the rate as at 48 kHz, 0.4167, at higher ones: 40 kHz at 96 kHz and above)
//   stop band   from (the lower rate - the pass band's edge) on, 100 dB down: whatever folds lands above the pass
//               band or is 100 dB down (24.1 kHz at 44.1 kHz, 28 kHz at 48 kHz: a converter's half-band filter)
//   delay       half its length, a whole number of input samples: delay() (0.4 ms at 48 kHz, 0.8 ms at 44.1 kHz)
// Between equal rates nothing is computed: the samples go through as they are, without delay.
//
// The two rates as whole numbers of Hz give a ratio M/L in lowest terms; output sample j is at input time j M/L
// exactly (integers: no drift), and the filter's taps for each of the L places between two input samples come from
// a table made in prepare(). (More than kMaxRows places: a table of that many, interpolated.) 2:1 has one row, a
// half-band filter. A stretch of output takes the input up to its last sample's place and no further: needs().
//
// Threads: prepare() allocates; everything else is for the audio thread (no allocation, no locks).
//
// Plain C++ (no JUCE).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

namespace g2io {

class Resampler {
public:
    static constexpr double kStopDb = 100.0;       // the stop band's attenuation the filter is designed for
    static constexpr std::size_t kMaxRows = 2048;  // rows of taps kept (a rate pair with more places: interpolated)

    // The pass band's edge for a conversion whose lower rate is `lowerRate` (Hz).
    static double passband(double lowerRate)
    {
        return (std::min)(20000.0 * (std::max)(1.0, lowerRate / 48000.0), 0.4535 * lowerRate);
    }

    // From `fromRate` to `toRate` (Hz, rounded to whole numbers), `channels` channels, `maxOut` output samples
    // per process() at most without splitting the work. Starts with silence in the past.
    void prepare(double fromRate, double toRate, int channels, std::size_t maxOut)
    {
        const auto from = static_cast<std::uint64_t>((std::max)(1.0, std::round(fromRate)));
        const auto to = static_cast<std::uint64_t>((std::max)(1.0, std::round(toRate)));
        const auto g = std::gcd(from, to);
        step_ = from / g;
        places_ = to / g;
        phase_ = places_;
        maxOut_ = (std::max<std::size_t>)(1, maxOut);
        channels_ = (std::max)(1, channels);
        if (from == to) { // nothing to do
            taps_ = 0;
            rows_ = 0;
            table_.clear();
            history_.clear();
            zeros_.assign(static_cast<std::size_t>(channels_), 0);
            return;
        }
        // Kaiser's estimates: the window's shape for the attenuation, the length for the transition band
        const double lower = static_cast<double>((std::min)(from, to));
        const double edge = passband(lower);
        const double transition = (lower - 2.0 * edge) / static_cast<double>(from); // in cycles per input sample
        const double beta = 0.1102 * (kStopDb - 8.7);
        const auto length = static_cast<std::size_t>(std::ceil((kStopDb - 7.95) / (14.36 * transition)));
        taps_ = (length + 3) / 4 * 4; // a multiple of 4: the delay is a whole number of samples, the sums go by fours
        interpolated_ = places_ > kMaxRows;
        rows_ = interpolated_ ? kMaxRows + 1 : static_cast<std::size_t>(places_);
        table_.assign(rows_ * taps_, 0.0f);
        // h(t), t in input samples from the filter's start, centred on taps_/2; cut-off at the lower rate's Nyquist
        const double cutoff = 0.5 * lower / static_cast<double>(from); // cycles per input sample
        const double half = 0.5 * static_cast<double>(taps_);
        const double i0Beta = besselI0(beta);
        const double pi = 3.14159265358979323846;
        std::vector<double> row(taps_);
        for (std::size_t r = 0; r < rows_; ++r) {
            const double fraction = static_cast<double>(r) / static_cast<double>(interpolated_ ? kMaxRows : rows_);
            double sum = 0;
            for (std::size_t m = 0; m < taps_; ++m) {
                const double t = static_cast<double>(m) + fraction - half;
                const double x = 2.0 * pi * cutoff * t;
                const double sinc = std::abs(x) < 1e-9 ? 1.0 : std::sin(x) / x;
                const double u = t / half;
                const double window = std::abs(u) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - u * u)) / i0Beta;
                row[m] = 2.0 * cutoff * sinc * window;
                sum += row[m];
            }
            // every place passes DC at exactly 1; the taps stored oldest input first
            for (std::size_t m = 0; m < taps_; ++m)
                table_[r * taps_ + (taps_ - 1 - m)] = static_cast<float>(row[m] / sum);
        }
        // per channel: the taps_ samples before the next input, then room for one stretch of new ones
        room_ = static_cast<std::size_t>(maxOut_ * step_ / places_) + 2;
        stride_ = taps_ + room_;
        history_.assign(static_cast<std::size_t>(channels_) * stride_, 0.0f);
        zeros_.assign(static_cast<std::size_t>(channels_), taps_);
    }

    // Silence in the past, the next output sample on the next input sample.
    void reset()
    {
        phase_ = places_;
        std::fill(history_.begin(), history_.end(), 0.0f);
        std::fill(zeros_.begin(), zeros_.end(), taps_);
    }

    // Equal rates: process() copies.
    bool transparent() const { return taps_ == 0; }
    // What the filter delays the signal by, in input samples (and seconds x the input rate).
    std::size_t delay() const { return taps_ / 2; }
    std::size_t taps() const { return taps_; }
    // The ratio in lowest terms: an output sample every step() / places() input samples.
    std::uint64_t step() const { return step_; }
    std::uint64_t places() const { return places_; }

    // The input samples the next `out` output samples take: exactly, from where the conversion is.
    std::size_t needs(std::size_t out) const { return out == 0 ? 0 : taken(phase_, out); }
    // Where output sample `out` from here is, in input samples after the first one the next process() takes
    // (before the filter's delay; down to -1: between the last sample taken and that one).
    double position(std::size_t out) const
    {
        return static_cast<double>(phase_ + out * step_) / static_cast<double>(places_) - 1.0;
    }

    // Channel `channel`: its next `n` output samples into out[0], out[outStride], ..., from its next needs(n) input
    // samples in[0], in[inStride], ... Once for every channel, then advance(n).
    void process(int channel, const float* in, std::ptrdiff_t inStride, float* out, std::ptrdiff_t outStride, std::size_t n)
    {
        const auto c = static_cast<std::size_t>(channel);
        if (transparent()) {
            for (std::size_t i = 0; i < n; ++i)
                out[static_cast<std::ptrdiff_t>(i) * outStride] = in[static_cast<std::ptrdiff_t>(i) * inStride];
            return;
        }
        float* const history = history_.data() + c * stride_;
        std::uint64_t phase = phase_;
        while (n > 0) { // (a block larger than prepare() was told: in stretches)
            const std::size_t count = (std::min)(n, maxOut_);
            const std::size_t taken = this->taken(phase, count);
            // a channel that has been silent for the filter's length gives silence
            std::size_t quiet = 0;
            while (quiet < taken && in[static_cast<std::ptrdiff_t>(taken - 1 - quiet) * inStride] == 0.0f)
                ++quiet;
            const bool silent = quiet == taken && zeros_[c] >= taps_;
            zeros_[c] = quiet == taken ? (std::min)(zeros_[c] + taken, taps_) : quiet;
            if (silent) {
                for (std::size_t i = 0; i < count; ++i)
                    out[static_cast<std::ptrdiff_t>(i) * outStride] = 0.0f;
            } else {
                for (std::size_t i = 0; i < taken; ++i)
                    history[taps_ + i] = in[static_cast<std::ptrdiff_t>(i) * inStride];
                std::uint64_t at = phase;
                for (std::size_t i = 0; i < count; ++i, at += step_) {
                    const float* x = history + static_cast<std::size_t>(at / places_);
                    const std::uint64_t place = at % places_;
                    float y;
                    if (!interpolated_) {
                        y = dot(x, table_.data() + static_cast<std::size_t>(place) * taps_);
                    } else {
                        const double row = static_cast<double>(place) * static_cast<double>(kMaxRows) / static_cast<double>(places_);
                        const auto r = static_cast<std::size_t>(row);
                        const auto w = static_cast<float>(row - static_cast<double>(r));
                        const float a = dot(x, table_.data() + r * taps_), b = dot(x, table_.data() + (r + 1) * taps_);
                        y = a + w * (b - a);
                    }
                    out[static_cast<std::ptrdiff_t>(i) * outStride] = y;
                }
                std::memmove(history, history + taken, taps_ * sizeof(float));
            }
            phase = phase + count * step_ - taken * places_;
            in += static_cast<std::ptrdiff_t>(taken) * inStride;
            out += static_cast<std::ptrdiff_t>(count) * outStride;
            n -= count;
        }
    }

    void advance(std::size_t n)
    {
        if (n > 0)
            phase_ = phase_ + n * step_ - taken(phase_, n) * places_;
    }

private:
    // The input samples `count` (> 0) output samples take from `phase`: up to the last one's place.
    std::size_t taken(std::uint64_t phase, std::size_t count) const
    {
        return static_cast<std::size_t>((phase + (count - 1) * step_) / places_);
    }

    // The modified Bessel function of the first kind, order 0 (the Kaiser window's).
    static double besselI0(double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 200; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1e-17 * sum)
                break;
        }
        return sum;
    }

    // The sum of x[i] * h[i] over the taps, four partial sums side by side (a compiler makes one vector of them).
    float dot(const float* x, const float* h) const
    {
        float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (std::size_t i = 0; i < taps_; i += 4) {
            s0 += x[i] * h[i];
            s1 += x[i + 1] * h[i + 1];
            s2 += x[i + 2] * h[i + 2];
            s3 += x[i + 3] * h[i + 3];
        }
        return (s0 + s2) + (s1 + s3);
    }

    std::uint64_t step_ = 1, places_ = 1; // input samples per output sample = step_ / places_
    // the next output sample is phase_ / places_ input samples after the last input sample taken (places_: on the next one)
    std::uint64_t phase_ = 1;
    std::size_t taps_ = 0, rows_ = 0, maxOut_ = 1, room_ = 0, stride_ = 0;
    int channels_ = 1;
    bool interpolated_ = false;
    std::vector<float> table_;        // rows_ x taps_
    std::vector<float> history_;      // channels_ x stride_
    std::vector<std::size_t> zeros_;  // per channel: zeros its input ended with (up to taps_)
};

} // namespace g2io
