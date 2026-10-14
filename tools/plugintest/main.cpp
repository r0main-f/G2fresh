// g2plugintest <G2fresh.vst3>: the plugin in a host, as a DAW uses it (see CMakeLists.txt). Prints each check and
// exits with 0 when all pass.
#include <juce_audio_processors/juce_audio_processors.h>

#if JUCE_MAC
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
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

#if JUCE_PLUGINHOST_AU && JUCE_MAC
// An Audio Unit is found by the system, not by its path: a .component's path gives the one that is installed under
// that name. To test the one at `path` (a build that is not installed), its bundle is loaded here and its factory
// registered in this process under a subtype of its own. Returns the identifier JUCE finds it by ("" if it cannot).
juce::String registerAudioUnit(const juce::String& path)
{
    static juce::String registered, registeredPath;
    if (registeredPath == path)
        return registered;
    registeredPath = path;
    registered = {};
    const auto url = CFURLCreateWithFileSystemPath(nullptr, path.toCFString(), kCFURLPOSIXPathStyle, true);
    const auto bundle = CFBundleCreate(nullptr, url); // stays loaded
    CFRelease(url);
    if (bundle == nullptr)
        return {};
    juce::String factoryName = "G2freshAUFactory";
    if (const auto list = static_cast<CFArrayRef>(CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("AudioComponents")));
        list != nullptr && CFGetTypeID(list) == CFArrayGetTypeID() && CFArrayGetCount(list) > 0)
        if (const auto first = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, 0)); CFGetTypeID(first) == CFDictionaryGetTypeID())
            if (const auto name = static_cast<CFStringRef>(CFDictionaryGetValue(first, CFSTR("factoryFunction"))))
                factoryName = juce::String::fromCFString(name);
    const auto factory = reinterpret_cast<AudioComponentFactoryFunction>(CFBundleGetFunctionPointerForName(bundle, factoryName.toCFString()));
    if (factory == nullptr)
        return {};
    AudioComponentDescription d{};
    d.componentType = 'aumu';
    d.componentSubType = 'G2te';
    d.componentManufacturer = 'G2fr';
    if (AudioComponentRegister(&d, CFSTR("G2fresh: G2fresh (the build under test)"), 1, factory) == nullptr)
        return {};
    registered = "AudioUnit:Synths/aumu,G2te,G2fr";
    return registered;
}
#endif

std::unique_ptr<juce::AudioPluginInstance> load(const juce::String& file, double rate, int block)
{
    return *onMessageThread([&]() -> std::unique_ptr<juce::AudioPluginInstance> {
        // a .component is an Audio Unit (as Live uses it), anything else a VST3
        std::unique_ptr<juce::AudioPluginFormat> format;
        juce::String path = file;
#if JUCE_PLUGINHOST_AU && JUCE_MAC
        if (path.endsWithIgnoreCase(".component")) {
            format = std::make_unique<juce::AudioUnitPluginFormat>();
            // the bundle at that path, unless it is an installed one (G2TEST_AU_INSTALLED=1: the system's)
            if (juce::SystemStats::getEnvironmentVariable("G2TEST_AU_INSTALLED", {}).isEmpty()) {
                path = registerAudioUnit(file);
                if (path.isEmpty()) {
                    std::cout << "  cannot register the Audio Unit at " << file << std::endl;
                    return nullptr;
                }
            }
        } else
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
    // as many channels as the plugin's buses have (an Audio Unit has all four pairs; Out 1 is channel 0 either way)
    juce::AudioBuffer<float> buffer(std::max({2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()}), block);
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

#if JUCE_MAC
juce::MemoryBlock withAuPluginState(const juce::MemoryBlock& hostState, const std::function<void(juce::ValueTree&)>& edit);
#endif

// G2fresh's tree in a VST3 host's state, or in an Audio Unit's (below)
juce::ValueTree pluginState(const juce::MemoryBlock& hostState)
{
    auto tree = parseHostState(hostState).tree;
#if JUCE_MAC
    if (!tree.isValid())
        withAuPluginState(hostState, [&](juce::ValueTree& t) { tree = t; });
#endif
    return tree;
}

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

#if JUCE_MAC
// The same for an Audio Unit: a JUCE AU host stores the unit's ClassInfo, a property list; G2fresh's tree is the
// data under "jucePluginState".
juce::MemoryBlock withAuPluginState(const juce::MemoryBlock& hostState, const std::function<void(juce::ValueTree&)>& edit)
{
    juce::MemoryBlock result;
    const auto data = CFDataCreate(nullptr, static_cast<const UInt8*>(hostState.getData()), static_cast<CFIndex>(hostState.getSize()));
    const auto plist = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListMutableContainers, nullptr, nullptr);
    CFRelease(data);
    if (plist == nullptr)
        return result;
    if (CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        const auto dict = static_cast<CFMutableDictionaryRef>(const_cast<void*>(plist));
        if (const auto held = static_cast<CFDataRef>(CFDictionaryGetValue(dict, CFSTR("jucePluginState")))) {
            auto tree = juce::ValueTree::readFromData(CFDataGetBytePtr(held), static_cast<size_t>(CFDataGetLength(held)));
            edit(tree);
            juce::MemoryOutputStream o;
            tree.writeToStream(o);
            const auto changed = CFDataCreate(nullptr, static_cast<const UInt8*>(o.getData()), static_cast<CFIndex>(o.getDataSize()));
            CFDictionarySetValue(dict, CFSTR("jucePluginState"), changed);
            CFRelease(changed);
            if (const auto out = CFPropertyListCreateData(nullptr, dict, kCFPropertyListBinaryFormat_v1_0, 0, nullptr)) {
                result.append(CFDataGetBytePtr(out), static_cast<size_t>(CFDataGetLength(out)));
                CFRelease(out);
            }
        }
    }
    CFRelease(plist);
    return result;
}
#endif

// The state a host would have saved for `p` with G2fresh's own tree changed by `edit` (a project with this patch).
juce::MemoryBlock projectState(juce::AudioPluginInstance& p, bool au, const std::function<void(juce::ValueTree&)>& edit)
{
    juce::MemoryBlock empty;
    p.getStateInformation(empty);
#if JUCE_MAC
    if (au)
        return withAuPluginState(empty, edit);
#else
    juce::ignoreUnused(au);
#endif
    auto s = pluginState(empty);
    edit(s);
    return withPluginState(empty, s);
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

// How late the tone `hz` in v[from, to) is, in samples, if v is sin(2 pi hz (n - delay) / rate): by its phase, which
// gives the delay within one period; `about` says which period.
double toneDelay(const std::vector<float>& v, std::size_t from, std::size_t to, double rate, double hz, double about)
{
    const double w = 2 * juce::MathConstants<double>::pi * hz / rate;
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
    for (auto i = from; i < std::min(to, v.size()); ++i) {
        const double s = std::sin(w * static_cast<double>(i)), c = std::cos(w * static_cast<double>(i));
        ss += s * s;
        cc += c * c;
        sc += s * c;
        xs += v[i] * s;
        xc += v[i] * c;
    }
    const double det = ss * cc - sc * sc;
    if (det == 0.0)
        return about;
    const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det; // v = a sin(w n) + b cos(w n)
    const double period = rate / hz, delay = -std::atan2(b, a) / w;
    return delay + period * std::round((about - delay) / period);
}

// The plugin's outputs: a patch with a 440 Hz sine on Out 1/2 and a 659 Hz one on Out 3/4, played with the second
// output bus on (each pair on its own), off (Out 1/2 only, as the G2's headphones), and off with Out 3/4 mixed in.
void outputs(const juce::String& path, const juce::File& patch)
{
    constexpr double rate = 48000;
    constexpr int block = 256;
    for (const int mode : {0, 1, 2}) {
        const bool aux = mode == 0, mix = mode == 2;
        std::cout << (aux ? "Out 3/4 bus on" : mix ? "Out 3/4 bus off, mixed into Out 1/2" : "Out 3/4 bus off") << std::endl;
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
            s.setProperty("mixOut34", mix, nullptr);
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
            else if (mix)
                check(has440 && has659, "Out 1/2 with Out 3/4 mixed in");
            else
                check(has440 && !has659, "Out 1/2 only");
        }
        onMessageThread([&] { p.reset(); });
    }
}

// The plugin's inputs: a patch that passes In 1-4 to Out 1-4 (4-In -> 4-Out), a tone of its own on each input,
// with both stereo pairs routed (4 in, 4 out), with In 1/2 and Out 1/2 alone, and with no input at all (an
// instrument as before). Checks the level (an input of 1.0 comes out as 1.0), that each input reaches its own
// output only, the delay against the latency the plugin reports, and that the tone has no gaps.
void inputs(const juce::String& path, const juce::File& patch)
{
    // (G2TEST_RATE, G2TEST_BLOCK: another sample rate or block size)
    const double rate = juce::SystemStats::getEnvironmentVariable("G2TEST_RATE", "48000").getDoubleValue();
    const int block = juce::SystemStats::getEnvironmentVariable("G2TEST_BLOCK", "256").getIntValue();
    constexpr double hz[4] = {440.0, 659.255, 880.0, 1174.66};
    constexpr float level = 0.5f;
    // An Audio Unit has no bus that is off: all four pairs are there, and a host leaves unconnected what it does
    // not use (as the first case here; the other two are the VST3's).
    const bool au = path.endsWithIgnoreCase(".component");
    for (const int mode : {0, 1, 2}) {
        const bool four = mode == 0, none = mode == 2;
        if (au && !four)
            break;
        std::cout << (four ? "In 1/2 and In 3/4, Out 1/2 and Out 3/4" : none ? "No input bus" : "In 1/2 and Out 1/2 only") << std::endl;
        auto p = load(path, rate, block);
        if (p == nullptr)
            return;
        bool laidOut = false;
        onMessageThread([&] {
            auto layout = p->getBusesLayout();
            std::cout << "  buses: " << layout.inputBuses.size() << " in, " << layout.outputBuses.size() << " out; by default "
                      << p->getTotalNumInputChannels() << " input and " << p->getTotalNumOutputChannels() << " output channels" << std::endl;
            const auto pair = four ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled();
            if (layout.inputBuses.size() > 0)
                layout.inputBuses.getReference(0) = none ? juce::AudioChannelSet::disabled() : juce::AudioChannelSet::stereo();
            if (layout.inputBuses.size() > 1)
                layout.inputBuses.getReference(1) = pair;
            if (layout.outputBuses.size() > 1)
                layout.outputBuses.getReference(1) = pair;
            laidOut = layout == p->getBusesLayout() || p->setBusesLayout(layout);
            juce::MemoryBlock data;
            patch.loadFileAsData(data);
            const auto wrapped = projectState(*p, au, [&](juce::ValueTree& s) { s.setProperty("data", data.toBase64Encoding(), nullptr); });
            if (wrapped.getSize() == 0)
                std::cout << "  (cannot wrap the patch into the host's state)" << std::endl;
            p->setStateInformation(wrapped.getData(), static_cast<int>(wrapped.getSize()));
            p->prepareToPlay(rate, block);
        });
        const int ins = p->getTotalNumInputChannels(), outs = p->getTotalNumOutputChannels();
        check(laidOut && ins == (four ? 4 : none ? 0 : 2) && outs == (four ? 4 : 2),
              "the layout is accepted: " + juce::String(ins) + " input and " + juce::String(outs) + " output channels");
        const int channels = std::max(ins, outs);
        std::vector<std::vector<float>> out(static_cast<std::size_t>(outs));
        juce::AudioBuffer<float> buffer(channels, block);
        // 5 s for the emulated G2 to start and take the patch, then the tones from an exact sample on
        const int quiet = static_cast<int>(5.0 * rate / block), blocks = quiet + static_cast<int>(3.0 * rate / block);
        auto next = std::chrono::steady_clock::now();
        makeRealtime(block / rate);
        for (int b = 0; b < blocks; ++b) {
            juce::MidiBuffer midi;
            buffer.clear();
            if (b >= quiet)
                for (int c = 0; c < ins; ++c)
                    for (int i = 0; i < block; ++i)
                        buffer.setSample(c, i, level * static_cast<float>(std::sin(2 * juce::MathConstants<double>::pi * hz[c]
                                                                                    * ((b - quiet) * block + i) / rate)));
            {
                const juce::ScopedLock lock(p->getCallbackLock());
                p->processBlock(buffer, midi);
            }
            if (b >= quiet)
                for (int c = 0; c < outs; ++c)
                    out[static_cast<std::size_t>(c)].insert(out[static_cast<std::size_t>(c)].end(), buffer.getReadPointer(c), buffer.getReadPointer(c) + block);
            next += std::chrono::microseconds(static_cast<long long>(1e6 * block / rate));
            std::this_thread::sleep_until(next);
        }
        const int latency = p->getLatencySamples();
        const auto from = static_cast<std::size_t>(1 * rate), to = static_cast<std::size_t>(3 * rate);
        for (int c = 0; c < outs; ++c) {
            const auto& v = out[static_cast<std::size_t>(c)];
            juce::String line = "  Out " + juce::String(c + 1) + ":";
            bool own = false, others = false;
            for (int k = 0; k < 4; ++k) {
                const double a = toneLevel(v, from, to, rate, hz[k]);
                line += " " + juce::String(hz[k], 0) + " Hz " + juce::String(a, 4);
                if (k == c)
                    own = std::abs(a / level - 1.0) < 0.03; // within 0.25 dB of the input
                else
                    others = others || a > 0.001;
            }
            std::cout << line << std::endl;
            if (c >= ins) {
                check(peak(v, 0, v.size()) < 1e-4f, "Out " + juce::String(c + 1) + " is silent without its input");
                continue;
            }
            check(own && !others, "In " + juce::String(c + 1) + " comes out of Out " + juce::String(c + 1) + " alone, at the level it went in");
            // The delay. Roughly: where the tone first reaches a fifth of its level (the converters' filters ring a
            // little before a start: far below that), less the time a sine takes to get there. Exactly: the steady
            // tone's phase, in the period that estimate points at.
            std::size_t onset = 0;
            while (onset < v.size() && std::abs(v[onset]) < 0.2f * level)
                ++onset;
            const double rise = std::asin(0.2) / (2 * juce::MathConstants<double>::pi * hz[c]) * rate;
            const double measured = toneDelay(v, from, to, rate, hz[c], static_cast<double>(onset) - rise);
            check(std::abs(measured - latency) < 2.0,
                  "it comes " + juce::String(measured, 2) + " samples after it went in; the plugin reports " + juce::String(latency)
                      + " (" + juce::String(latency * 1000.0 / rate, 2) + " ms)");
            // no gaps: no run of zeros, and the tone's level over every tenth of a second stays the same
            double low = 1e9, high = 0;
            const auto window = static_cast<std::size_t>(rate / 10);
            for (auto at = from; at + window <= to; at += window) {
                const double a = toneLevel(v, at, at + window, rate, hz[c]);
                low = std::min(low, a);
                high = std::max(high, a);
            }
            check(low > 0.97 * high && longestSilence(v, from, to) < 8,
                  "without gaps (level over 0.1 s between " + juce::String(low, 4) + " and " + juce::String(high, 4) + ", longest silence "
                      + juce::String(static_cast<int>(longestSilence(v, from, to))) + " samples)");
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
            // (an Audio Unit's state is wrapped as an AU host keeps it, a VST3's as a VST3 host does)
            const auto wrapped = projectState(*a, path.endsWithIgnoreCase(".component"), [&](juce::ValueTree& s) {
                s.setProperty("name", patch.getFileNameWithoutExtension(), nullptr);
                s.setProperty("data", data.toBase64Encoding(), nullptr);
            });
            if (wrapped.getSize() == 0)
                std::cout << "  (cannot wrap the patch into the host's state)" << std::endl;
            else
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
#if JUCE_PLUGINHOST_AU && JUCE_MAC
        // JUCE's Audio Unit host does not pass that on: as an AU host does it, the unit's offline-render property
        if (path.endsWithIgnoreCase(".component"))
            if (auto* client = b->getAudioUnitClient(); client != nullptr && client->getAudioUnitHandle() != nullptr) {
                const auto unit = client->getAudioUnitHandle();
                const UInt32 on = 1;
                AudioUnitSetProperty(unit, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &on, sizeof on);
            }
#endif
        b->prepareToPlay(rate, block);
    });
    const auto t0 = std::chrono::steady_clock::now();
    auto bounce = play(*b, rate, block, 4.0, 0.5, 3.5, true);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    b->setNonRealtime(false);
#if JUCE_PLUGINHOST_AU && JUCE_MAC
    if (path.endsWithIgnoreCase(".component"))
        if (auto* client = b->getAudioUnitClient(); client != nullptr && client->getAudioUnitHandle() != nullptr) {
            const auto unit = client->getAudioUnitHandle();
            const UInt32 off = 0;
            AudioUnitSetProperty(unit, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &off, sizeof off);
        }
#endif
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
                         "       g2plugintest <plugin> --restore <state saved by a host>\n"
                         "       g2plugintest <plugin> --outputs <patch with 440 Hz on Out 1/2 and 659 Hz on Out 3/4>\n"
                         "       g2plugintest <plugin> --inputs <patch that passes In 1-4 to Out 1-4>, e.g. from\n"
                         "           g2mkpatch in.pch2 in=4-In out=4-Out 'in.0>out.0' 'in.1>out.1' 'in.2>out.2' 'in.3>out.3' gain=127\n"
                         "       g2plugintest <plugin> --clock\n";
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
        if (args.size() > 2 && (args[1] == "--outputs" || args[1] == "--inputs")) {
            const juce::String path(args[0]);
            const auto patch = juce::File::getCurrentWorkingDirectory().getChildFile(args[2]);
            test_ = std::thread([this, path, patch, in = args[1] == "--inputs"] {
                if (in)
                    inputs(path, patch);
                else
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
