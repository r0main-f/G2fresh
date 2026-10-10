// g2plugintest <G2fresh.vst3>: the plugin in a host, as a DAW uses it (see CMakeLists.txt). Prints each check and
// exits with 0 when all pass.
#include <juce_audio_processors/juce_audio_processors.h>

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
        juce::VST3PluginFormat format;
        juce::OwnedArray<juce::PluginDescription> types;
        format.findAllTypesForFile(types, path);
        if (types.isEmpty())
            return nullptr;
        juce::String error;
        auto p = format.createInstanceFromDescription(*types[0], rate, block, error);
        if (p == nullptr)
            std::cout << "  cannot load: " << error << std::endl;
        return p;
    });
}

// Plays `seconds` of audio through the plugin, in real time (or as fast as it goes when offline), with a note held
// from `noteOn` to `noteOff` (seconds; negative: none). Returns the left channel.
std::vector<float> play(juce::AudioPluginInstance& p, double rate, int block, double seconds, double noteOn,
                        double noteOff, bool offline)
{
    std::vector<float> out;
    juce::AudioBuffer<float> buffer(2, block);
    const auto blocks = static_cast<int>(seconds * rate / block);
    auto next = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) {
        juce::MidiBuffer midi;
        const double t0 = b * block / rate, t1 = (b + 1) * block / rate;
        if (noteOn >= 0 && noteOn >= t0 && noteOn < t1)
            midi.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 100), static_cast<int>((noteOn - t0) * rate));
        if (noteOff >= 0 && noteOff >= t0 && noteOff < t1)
            midi.addEvent(juce::MidiMessage::noteOff(1, 69), static_cast<int>((noteOff - t0) * rate));
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

    std::cout << "A second instance from that state (a project reopened)" << std::endl;
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
    b->setNonRealtime(true);
    const auto t0 = std::chrono::steady_clock::now();
    auto bounce = play(*b, rate, block, 4.0, 0.5, 3.5, true);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    b->setNonRealtime(false);
    std::cout << "  (4 s rendered in " << took << " s; output peak " << peak(bounce, 0, bounce.size()) << ")" << std::endl;
    check(peak(bounce, 0, bounce.size()) > 0.01f, "it renders the patch");
    check(longestSilence(bounce, 0, bounce.size()) < static_cast<std::size_t>(rate * 0.005),
          "without dropouts, faster or slower than real time");

    onMessageThread([&] {
        a.reset();
        b.reset();
    });
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
            std::cerr << "usage: g2plugintest <G2fresh.vst3> [patch.pch2 that sounds without notes]\n";
            setApplicationReturnValue(2);
            quit();
            return;
        }
        const juce::String path(args[0]);
        const juce::File patch = args.size() > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile(args[1]) : juce::File();
        test_ = std::thread([this, path, patch] {
            run(path, patch);
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
