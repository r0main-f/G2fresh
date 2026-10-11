// g2plugintest <G2fresh.vst3>: the plugin in a host, as a DAW uses it (see CMakeLists.txt). Prints each check and
// exits with 0 when all pass.
#include <juce_audio_processors/juce_audio_processors.h>

#if JUCE_MAC
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const juce::String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
    if (!ok)
        ++failures;
}

template <typename F>
auto onMessageThread(F&& f)
{
    return juce::MessageManager::callSync(std::forward<F>(f));
}

std::unique_ptr<juce::AudioPluginInstance> load(const juce::String& path, double rate, int block)
{
    return *onMessageThread([&]() -> std::unique_ptr<juce::AudioPluginInstance> {
        // a .component is an Audio Unit (as Live uses it), anything else a VST3
        std::unique_ptr<juce::AudioPluginFormat> format;
#if JUCE_PLUGINHOST_AU && JUCE_MAC
        if (path.endsWithIgnoreCase(".component"))
            format = std::make_unique<juce::AudioUnitPluginFormat>();
        else
#endif
            format = std::make_unique<juce::VST3PluginFormat>();
        juce::OwnedArray<juce::PluginDescription> types;
        format->findAllTypesForFile(types, path);
        if (types.isEmpty())
            return nullptr;
        juce::String error;
        auto p = format->createInstanceFromDescription(*types[0], rate, block, error);
        if (p == nullptr)
            std::cout << "  cannot load: " << error << std::endl;
        return p;
    });
}

// This thread as a host's audio thread: real-time scheduling, as CoreAudio gives its I/O thread (on macOS; an
// ordinary thread wakes late under load and then pulls several blocks at once, which a host never does).
void makeRealtime(double period)
{
#if JUCE_MAC
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const auto toAbs = [&](double s) { return static_cast<uint32_t>(s * 1e9 * tb.denom / tb.numer); };
    thread_time_constraint_policy_data_t policy;
    policy.period = toAbs(period);
    policy.computation = toAbs(period * 0.5);
    policy.constraint = toAbs(period * 0.9);
    policy.preemptible = 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#else
    juce::ignoreUnused(period);
#endif
}

// Plays `seconds` of audio through the plugin, in real time (or as fast as it goes when offline), with a note held
// from `noteOn` to `noteOff` (seconds; negative: none). Returns the left channel.
std::vector<float> play(juce::AudioPluginInstance& p, double rate, int block, double seconds, double noteOn,
                        double noteOff, bool offline, std::vector<int> notes = {69})
{
    std::vector<float> out;
    juce::AudioBuffer<float> buffer(2, block);
    const auto blocks = static_cast<int>(seconds * rate / block);
    if (!offline)
        makeRealtime(block / rate);
    auto next = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) {
        juce::MidiBuffer midi;
        const double t0 = b * block / rate, t1 = (b + 1) * block / rate;
        for (const int n : notes) {
            if (noteOn >= 0 && noteOn >= t0 && noteOn < t1)
                midi.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8) 100), static_cast<int>((noteOn - t0) * rate));
            if (noteOff >= 0 && noteOff >= t0 && noteOff < t1)
                midi.addEvent(juce::MidiMessage::noteOff(1, n), static_cast<int>((noteOff - t0) * rate));
        }
        buffer.clear();
        {
            const juce::ScopedLock lock(p.getCallbackLock());
            p.processBlock(buffer, midi);
        }
        out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + block);
        if (!offline) {
            next += std::chrono::microseconds(static_cast<long long>(1e6 * block / rate));
            std::this_thread::sleep_until(next);
        }
    }
    return out;
}

float peak(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    float m = 0;
    for (auto i = from; i < std::min(to, v.size()); ++i)
        m = std::max(m, std::abs(v[i]));
    return m;
}

// The frequency of a steady tone in v[from, to), by zero crossings.
double frequency(const std::vector<float>& v, std::size_t from, std::size_t to, double rate)
{
    double first = -1, last = -1;
    int n = 0;
    for (auto i = from; i + 1 < std::min(to, v.size()); ++i)
        if (v[i] < 0 && v[i + 1] >= 0) {
            const double t = static_cast<double>(i) + v[i] / (v[i] - v[i + 1]);
            if (first < 0)
                first = t;
            last = t;
            ++n;
        }
    return n > 2 ? (n - 1) * rate / (last - first) : 0.0;
}

// The longest run of exact zeros in v[from, to) (a dropout in a steady tone).
std::size_t longestSilence(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    std::size_t best = 0, run = 0;
    for (auto i = from; i < std::min(to, v.size()); ++i) {
        run = v[i] == 0.0f ? run + 1 : 0;
        best = std::max(best, run);
    }
    return best;
}

// What a JUCE VST3 host stores: its own binary XML ("VST3PluginState") with the plugin's component state as base64
// under "IComponent"; in that, G2fresh's ValueTree (type "G2fresh"), possibly followed by the wrapper's own data.
struct HostState {
    std::unique_ptr<juce::XmlElement> xml;
    juce::MemoryBlock component;
    std::size_t at = 0, size = 0; // where G2fresh's tree is in `component`
    juce::ValueTree tree;
};

HostState parseHostState(const juce::MemoryBlock& hostState)
{
    HostState h;
    h.xml = juce::AudioProcessor::getXmlFromBinary(hostState.getData(), static_cast<int>(hostState.getSize()));
    if (h.xml == nullptr)
        return h;
    if (auto* c = h.xml->getChildByName("IComponent"))
        h.component.fromBase64Encoding(c->getAllSubText().trim());
    const auto* bytes = static_cast<const char*>(h.component.getData());
    for (std::size_t i = 0; i + 8 <= h.component.getSize(); ++i)
        if (std::memcmp(bytes + i, "G2fresh", 7) == 0) {
            h.tree = juce::ValueTree::readFromData(bytes + i, h.component.getSize() - i);
            if (h.tree.isValid()) {
                juce::MemoryOutputStream o;
                h.tree.writeToStream(o);
                h.at = i;
                h.size = o.getDataSize();
                break;
            }
        }
    return h;
}

juce::ValueTree pluginState(const juce::MemoryBlock& hostState) { return parseHostState(hostState).tree; }

// `hostState` with G2fresh's tree replaced by `tree`.
juce::MemoryBlock withPluginState(const juce::MemoryBlock& hostState, const juce::ValueTree& tree)
{
    auto h = parseHostState(hostState);
    if (!h.tree.isValid())
        return {};
    juce::MemoryOutputStream component;
    component.write(h.component.getData(), h.at);
    tree.writeToStream(component);
    component.write(static_cast<const char*>(h.component.getData()) + h.at + h.size, h.component.getSize() - h.at - h.size);
    if (auto* c = h.xml->getChildByName("IComponent")) {
        c->deleteAllTextElements();
        c->addTextElement(component.getMemoryBlock().toBase64Encoding());
    }
    juce::MemoryBlock out;
    juce::AudioProcessor::copyXmlToBinary(*h.xml, out);
    return out;
}

void waitMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A project reopened: the plugin restored from the state bytes a host saved (e.g. the AU's ClassInfo from a Live
// set), then played; prints the level and writes the state the plugin gives back.
void restore(const juce::String& path, const juce::File& saved, double seconds)
{
    constexpr double rate = 48000;
    const int block = juce::SystemStats::getEnvironmentVariable("G2TEST_BLOCK", "256").getIntValue(); // Live: 512
    juce::MemoryBlock bytes;
    check(saved.loadFileAsData(bytes), "the saved state reads (" + juce::String(bytes.getSize()) + " bytes)");
    auto p = load(path, rate, block);
    check(p != nullptr, "the plugin loads");
    if (p == nullptr)
        return;
    onMessageThread([&] {
        p->setStateInformation(bytes.getData(), static_cast<int>(bytes.getSize()));
        p->prepareToPlay(rate, block);
    });
    // a chord held over the last 2 s (as played in a set: A minor 7, four voices)
    const auto out = play(*p, rate, block, seconds, seconds - 2.5, seconds - 0.2, false, {57, 60, 64, 67});
    const auto tail = static_cast<std::size_t>((seconds - 2.0) * rate);
    const auto level = peak(out, tail, out.size());
    std::cout << "  last 2 s: peak " << level << " (" << juce::Decibels::gainToDecibels(level) << " dBFS), samples over 1.0: "
              << std::count_if(out.begin() + static_cast<std::ptrdiff_t>(tail), out.end(), [](float v) { return std::abs(v) > 1.0f; })
              << ", longest silence " << longestSilence(out, tail, out.size()) << " samples, latency " << p->getLatencySamples()
              << std::endl;
    juce::MemoryBlock after;
    onMessageThread([&] { p->getStateInformation(after); });
    saved.withFileExtension(".after").replaceWithData(after.getData(), after.getSize());
    onMessageThread([&] { p.reset(); });
}

// The level of `hz` in v[from, to) (Goertzel), as the amplitude of a sine at that frequency.
double toneLevel(const std::vector<float>& v, std::size_t from, std::size_t to, double rate, double hz)
{
    const double w = 2 * juce::MathConstants<double>::pi * hz / rate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    std::size_t n = 0;
    for (auto i = from; i < std::min(to, v.size()); ++i, ++n) {
        const double s = v[i] + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double power = s1 * s1 + s2 * s2 - c * s1 * s2;
    return n > 0 ? 2 * std::sqrt(std::max(0.0, power)) / static_cast<double>(n) : 0.0;
}

// The plugin's outputs: a patch with a 440 Hz sine on Out 1/2 and a 659 Hz one on Out 3/4, played with the second
// output bus on (each pair on its own) and off (Out 3/4 mixed into Out 1/2).
void outputs(const juce::String& path, const juce::File& patch)
{
    constexpr double rate = 48000;
    constexpr int block = 256;
    for (const bool aux : {true, false}) {
        std::cout << (aux ? "Out 3/4 bus on" : "Out 3/4 bus off") << std::endl;
        auto p = load(path, rate, block);
        if (p == nullptr)
            return;
        bool laidOut = false;
        onMessageThread([&] {
            auto layout = p->getBusesLayout();
            if (layout.outputBuses.size() > 1)
                layout.outputBuses.getReference(1) = aux ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled();
            laidOut = p->setBusesLayout(layout);
            juce::MemoryBlock empty, data;
            p->getStateInformation(empty);
            patch.loadFileAsData(data);
            auto s = pluginState(empty);
            s.setProperty("data", data.toBase64Encoding(), nullptr);
            const auto wrapped = withPluginState(empty, s);
            p->setStateInformation(wrapped.getData(), static_cast<int>(wrapped.getSize()));
            p->prepareToPlay(rate, block);
        });
        const int channels = p->getTotalNumOutputChannels();
        check(laidOut && channels == (aux ? 4 : 2), "the layout is accepted: " + juce::String(channels) + " channels");
        std::vector<std::vector<float>> out(static_cast<std::size_t>(channels));
        juce::AudioBuffer<float> buffer(channels, block);
        const int blocks = static_cast<int>(6.0 * rate / block);
        auto next = std::chrono::steady_clock::now();
        makeRealtime(block / rate);
        for (int b = 0; b < blocks; ++b) {
            juce::MidiBuffer midi;
            buffer.clear();
            {
                const juce::ScopedLock lock(p->getCallbackLock());
                p->processBlock(buffer, midi);
            }
            for (int c = 0; c < channels; ++c)
                out[static_cast<std::size_t>(c)].insert(out[static_cast<std::size_t>(c)].end(), buffer.getReadPointer(c), buffer.getReadPointer(c) + block);
            next += std::chrono::microseconds(static_cast<long long>(1e6 * block / rate));
            std::this_thread::sleep_until(next);
        }
        const auto from = static_cast<std::size_t>(4 * rate), to = static_cast<std::size_t>(6 * rate);
        for (int c = 0; c < channels; ++c) {
            const auto& v = out[static_cast<std::size_t>(c)];
            const double a = toneLevel(v, from, to, rate, 440.0), e = toneLevel(v, from, to, rate, 659.255);
            std::cout << "  channel " << c + 1 << ": 440 Hz " << juce::String(a, 3) << ", 659 Hz " << juce::String(e, 3) << std::endl;
            const bool has440 = a > 0.05, has659 = e > 0.05;
            if (aux)
                check(c < 2 ? (has440 && !has659) : (has659 && !has440), c < 2 ? "Out 1/2 alone on its bus" : "Out 3/4 alone on its bus");
            else
                check(has440 && has659, "Out 1/2 with Out 3/4 mixed in");
        }
        onMessageThread([&] { p.reset(); });
    }
}

// A host's transport: a tempo, playing from the top, the beat position advancing with the samples played.
struct Transport final : juce::AudioPlayHead {
    double bpm = 100, rate = 48000;
    std::atomic<std::int64_t> sample{0};
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setBpm(bpm);
        p.setIsPlaying(true);
        p.setTimeInSamples(sample.load());
        p.setPpqPosition(static_cast<double>(sample.load()) / rate * bpm / 60.0);
        return p;
    }
};

// The host's tempo drives the emulated G2's master clock: 100 BPM, playing; the G2's Master Clock display then says
// so (read from the capture's report).
void clock(const juce::String& path)
{
    constexpr double rate = 48000;
    constexpr int block = 512;
    Transport transport;
    auto p = load(path, rate, block);
    if (p == nullptr)
        return;
    onMessageThread([&] {
        p->setPlayHead(&transport);
        p->prepareToPlay(rate, block);
    });
    const auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
#if JUCE_MAC
                            .getChildFile("Application Support")
#endif
                            .getChildFile("G2fresh");
    const auto report = juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("G2fresh capture.txt");
    report.deleteFile();
    juce::AudioBuffer<float> buffer(2, block);
    makeRealtime(block / rate);
    auto next = std::chrono::steady_clock::now();
    const double seconds = juce::SystemStats::getEnvironmentVariable("G2TEST_SECONDS", "12").getDoubleValue();
    for (int b = 0; b < static_cast<int>(seconds * rate / block); ++b) {
        if (b == static_cast<int>((seconds - 6.0) * rate / block))
            folder.getChildFile("capture").create(); // a 2 s capture (G2FRESH_CAPTURE_SECONDS), 6 s before the end
        juce::MidiBuffer midi;
        buffer.clear();
        {
            const juce::ScopedLock lock(p->getCallbackLock());
            p->processBlock(buffer, midi);
        }
        transport.sample += block;
        next += std::chrono::microseconds(static_cast<long long>(1e6 * block / rate));
        std::this_thread::sleep_until(next);
    }
    waitMs(2000);
    const auto text = report.loadFileAsString();
    std::cout << text;
    check(text.contains("Ext 100"), "the G2's master clock follows the host's 100 BPM");
    check(text.contains("Run"), "and runs with the host's transport");
    onMessageThread([&] { p.reset(); });
}

void run(const juce::String& path, const juce::File& patch)
{
    constexpr double rate = 48000;
    constexpr int block = 256;

    std::cout << "A new instance" << std::endl;
    auto a = load(path, rate, block);
    check(a != nullptr, "the plugin loads");
    if (a == nullptr)
        return;
    onMessageThread([&] {
        // a project with this patch (a drone: it sounds without notes), in the host's wrapping of the state
        juce::MemoryBlock data;
        if (patch.loadFileAsData(data)) {
            juce::MemoryBlock empty;
            a->getStateInformation(empty);
            auto s = pluginState(empty);
            s.setProperty("name", patch.getFileNameWithoutExtension(), nullptr);
            s.setProperty("data", data.toBase64Encoding(), nullptr);
            const auto wrapped = withPluginState(empty, s);
            if (wrapped.getSize() == 0)
                std::cout << "  (cannot wrap the patch into the host's state)" << std::endl;
            a->setStateInformation(wrapped.getData(), static_cast<int>(wrapped.getSize()));
        }
        a->prepareToPlay(rate, block);
    });
    // the emulated G2 starts by itself; boot, USB sync and the upload take about 3 s
    play(*a, rate, block, 5.0, -1, -1, false);
    check(a->getLatencySamples() > 0, "it reports its latency: " + juce::String(a->getLatencySamples()) + " samples ("
                                          + juce::String(a->getLatencySamples() * 1000.0 / rate, 1) + " ms)");
    auto sound = play(*a, rate, block, 2.0, 0.2, 1.5, false);
    const auto level = peak(sound, 0, sound.size());
    check(level > 0.01f, "it plays the patch: peak " + juce::String(level, 3) + " ("
                             + juce::String(juce::Decibels::gainToDecibels(level), 1) + " dBFS), "
                             + juce::String(frequency(sound, 0, sound.size(), rate), 1) + " Hz by zero crossings");
    check(longestSilence(sound, 0, sound.size()) < static_cast<std::size_t>(rate * 0.005), "without dropouts in real time");

    std::cout << "Its state" << std::endl;
    waitMs(2500); // the flash goes into the state once quiet for a second (the timer ticks every second)
    juce::MemoryBlock state;
    onMessageThread([&] { a->getStateInformation(state); });
    const auto tree = pluginState(state);
    check(tree.hasType("G2fresh"), "the state is G2fresh's (" + juce::String(state.getSize()) + " bytes)");
    check(static_cast<bool>(tree.getProperty("emulated", false)), "it says the emulated G2 runs");
    const auto flash = tree["emulatorFlash"].toString();
    check(flash.isNotEmpty(), "it holds the synth's memory (" + juce::String(flash.length()) + " base64 characters)");

    std::cout << "A second instance from that state (a project reopened: the first one is gone)" << std::endl;
    onMessageThread([&] { a.reset(); }); // one emulated G2 starts by itself per host
    auto b = load(path, rate, block);
    check(b != nullptr, "the plugin loads again");
    if (b == nullptr)
        return;
    onMessageThread([&] {
        b->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        b->prepareToPlay(rate, block);
    });
    play(*b, rate, block, 5.0, -1, -1, false);
    waitMs(2500);
    juce::MemoryBlock stateB;
    onMessageThread([&] { b->getStateInformation(stateB); });
    const auto treeB = pluginState(stateB);
    check(static_cast<bool>(treeB.getProperty("emulated", false)), "its emulated G2 runs again");
    check(b->getLatencySamples() > 0, "and reports its latency");
    check(treeB["emulatorFlash"].toString() == flash, "with the project's synth memory");

    std::cout << "Offline rendering (a bounce or a freeze)" << std::endl;
    // as a host does for a bounce: offline, then a new set-up (the VST3 process mode reaches the plugin there)
    onMessageThread([&] {
        b->setNonRealtime(true);
        b->prepareToPlay(rate, block);
    });
    const auto t0 = std::chrono::steady_clock::now();
    auto bounce = play(*b, rate, block, 4.0, 0.5, 3.5, true);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    b->setNonRealtime(false);
    std::cout << "  (4 s rendered in " << took << " s; output peak " << peak(bounce, 0, bounce.size()) << ")" << std::endl;
    {
        std::size_t run = 0, best = 0, end = 0;
        for (std::size_t i = 0; i < bounce.size(); ++i) {
            run = bounce[i] == 0.0f ? run + 1 : 0;
            if (run > best) {
                best = run;
                end = i;
            }
        }
        if (best > 0)
            std::cout << "  (longest silence: " << best << " samples ending at " << end / rate << " s)" << std::endl;
    }
    check(peak(bounce, 0, bounce.size()) > 0.01f, "it renders the patch");
    check(longestSilence(bounce, 0, bounce.size()) < static_cast<std::size_t>(rate * 0.005),
          "without dropouts, faster or slower than real time");

    onMessageThread([&] { b.reset(); });
}

} // namespace

class TestApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "g2plugintest"; }
    const juce::String getApplicationVersion() override { return "1"; }
    void initialise(const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();
        if (args.isEmpty()) {
            std::cerr << "usage: g2plugintest <G2fresh.vst3|.component> [patch.pch2 that sounds without notes]\n"
                         "       g2plugintest <plugin> --restore <state saved by a host>\n";
            setApplicationReturnValue(2);
            quit();
            return;
        }
        const juce::String path(args[0]);
        if (args.size() > 1 && args[1] == "--clock") {
            const juce::String path(args[0]);
            test_ = std::thread([this, path] {
                clock(path);
                std::cout << (failures == 0 ? "all checks passed" : juce::String(failures) + " check(s) failed") << std::endl;
                juce::MessageManager::callAsync([this] {
                    setApplicationReturnValue(failures == 0 ? 0 : 1);
                    quit();
                });
            });
            return;
        }
        if (args.size() > 2 && args[1] == "--outputs") {
            const juce::String path(args[0]);
            const auto patch = juce::File::getCurrentWorkingDirectory().getChildFile(args[2]);
            test_ = std::thread([this, path, patch] {
                outputs(path, patch);
                std::cout << (failures == 0 ? "all checks passed" : juce::String(failures) + " check(s) failed") << std::endl;
                juce::MessageManager::callAsync([this] {
                    setApplicationReturnValue(failures == 0 ? 0 : 1);
                    quit();
                });
            });
            return;
        }
        const bool restoring = args.size() > 2 && args[1] == "--restore";
        const juce::File file = args.size() > (restoring ? 2 : 1) ? juce::File::getCurrentWorkingDirectory().getChildFile(args[restoring ? 2 : 1]) : juce::File();
        test_ = std::thread([this, path, file, restoring] {
            if (restoring)
                restore(path, file, juce::SystemStats::getEnvironmentVariable("G2TEST_SECONDS", "8").getDoubleValue());
            else
                run(path, file);
            std::cout << (failures == 0 ? "all checks passed" : juce::String(failures) + " check(s) failed") << std::endl;
            juce::MessageManager::callAsync([this] {
                setApplicationReturnValue(failures == 0 ? 0 : 1);
                quit();
            });
        });
    }
    void shutdown() override
    {
        if (test_.joinable())
            test_.join();
    }

private:
    std::thread test_;
};

START_JUCE_APPLICATION(TestApp)
