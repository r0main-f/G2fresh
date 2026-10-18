#include "PluginProcessor.h"

#include "EmulatorFollow.h"

#include <algorithm>
#include "AudioIo.h"
#include "PluginEditor.h"
#include "Skin.h"

#if G2FRESH_EMULATOR
#include "EmulatedMachine.h"
#include "EmulatedSoundEngine.h"
#include "G2BenchData.h"
#include "g2emu/speedtest.hpp"
#include "g2emu/transport.hpp"
#endif

// The state stored in the host project (and by the stand-alone app between
// sessions): the patch or performance as a .pch2/.prf2, its name and file.
std::vector<std::uint8_t> G2EditorProcessor::encodeState() const
{
    const auto& doc = document_;
    juce::ValueTree state("G2fresh");
    const auto bytes = doc.saveBytes();
    juce::MemoryBlock data(bytes.data(), bytes.size());
    state.setProperty("version", 1, nullptr);
    state.setProperty("name", doc.name(), nullptr);
    state.setProperty("file", doc.file().getFullPathName(), nullptr);
    state.setProperty("edited", doc.isDirty(), nullptr); // edits never saved to the file stay marked edited
    state.setProperty("convertedFrom", doc.convertedFrom(), nullptr); // an old file not saved since, which Save asks about
    state.setProperty("data", data.toBase64Encoding(), nullptr);
    state.setProperty("layout", doc.layoutJson(), nullptr); // cable shapes
    state.setProperty("midiOut", midiOut_.deviceIdentifier(), nullptr);
    state.setProperty("midiOutName", midiOut_.deviceName(), nullptr);
    state.setProperty("midiChannel", midiOut_.channel(), nullptr);
    state.setProperty("emulated", emulatorWanted_, nullptr);
    state.setProperty("hostClock", hostClockOn_.load(), nullptr);
    state.setProperty("masterLevel", static_cast<double>(masterLevel_.load()), nullptr);
    state.setProperty("mixOut34", mixOut34_.load(), nullptr);
    state.setProperty("micInput", micInput_.load(), nullptr);
    state.setProperty("micLevel", static_cast<double>(micLevel_.load()), nullptr);
    if (flashGz_.getSize() > 0)
        state.setProperty("emulatorFlash", flashGz_.toBase64Encoding(), nullptr);
    juce::MemoryOutputStream out;
    state.writeToStream(out);
    const auto* p = static_cast<const std::uint8_t*>(out.getData());
    return {p, p + out.getDataSize()};
}

// The G2's jacks as buses (AudioIo.h). In a host: In 1/2 and Out 1/2, and a second stereo pair each way, if routed.
// The stand-alone app plays an audio device: its inputs 1-4 are IN 1-4, its outputs Out 1/2.
juce::AudioProcessor::BusesProperties G2EditorProcessor::buses()
{
    using Set = juce::AudioChannelSet;
    if (juce::PluginHostType::getPluginLoadedAs() == wrapperType_Standalone)
        return BusesProperties().withInput("In 1-4", Set::discreteChannels(4), true).withOutput("Out 1/2", Set::stereo(), true);
    return BusesProperties()
        .withInput("In 1/2", Set::stereo(), true)
        .withInput("In 3/4", Set::stereo(), false)
        .withOutput("Out 1/2", Set::stereo(), true)
        .withOutput("Out 3/4", Set::stereo(), false);
}

namespace {

// A bus's channels for g2io::layoutSupported: mono, stereo or off; for an input also plain channels (the
// stand-alone app's device inputs, which JUCE's player may also name by a surround format of that size).
int channelsOf(const juce::AudioChannelSet& set, bool input)
{
    if (set.isDisabled())
        return 0;
    if (set == juce::AudioChannelSet::mono())
        return 1;
    if (set == juce::AudioChannelSet::stereo())
        return 2;
    const int n = set.size();
    return input && (set == juce::AudioChannelSet::discreteChannels(n) || set == juce::AudioChannelSet::canonicalChannelSet(n)) ? n : -1;
}

} // namespace

G2EditorProcessor::G2EditorProcessor() : AudioProcessor(buses())
{
    stateBytes_ = encodeState();
    document_.addChangeListener(this);
    automation_ = std::make_unique<AutomationBank>(*this, document_);
    midiOut_.onChange = [this] { refreshState(); };
    synth_.addChangeListener(this);
#if G2FRESH_EMULATOR
    // A new instance plays the emulated G2 at once; a project's instance once its state is in (setStateInformation
    // comes right after the constructor, before this runs).
    juce::MessageManager::callAsync([this, alive = std::weak_ptr<int>(alive_)] {
        if (alive.lock())
            autoStartEmulator();
    });
    startTimer(1000);
#endif
}

G2EditorProcessor::~G2EditorProcessor()
{
    stopTimer();
    synth_.removeChangeListener(this);
    document_.removeChangeListener(this);
    synth_.disconnect(); // the emulated G2's link, before the emulator goes
#if G2FRESH_EMULATOR
    stopExpansionTest_ = true;
    if (expansionTest_.joinable())
        expansionTest_.join();
#endif
}

void G2EditorProcessor::setEmulated(std::unique_ptr<SoundEngine> engine)
{
    {
        const juce::ScopedLock lock(getCallbackLock());
        std::swap(emulated_, engine);
    }
    engine.reset(); // the old one: stops its threads, saves its flash
}

#if G2FRESH_EMULATOR
namespace {

juce::MemoryBlock gzip(const std::vector<std::uint8_t>& bytes)
{
    juce::MemoryOutputStream out;
    {
        juce::GZIPCompressorOutputStream z(out, 9);
        z.write(bytes.data(), bytes.size());
    }
    return out.getMemoryBlock();
}

std::vector<std::uint8_t> gunzip(const juce::MemoryBlock& gz)
{
    juce::MemoryInputStream in(gz, false);
    juce::GZIPDecompressorInputStream z(in);
    juce::MemoryBlock out;
    z.readIntoMemoryBlock(out);
    const auto* p = static_cast<const std::uint8_t*>(out.getData());
    return {p, p + out.getSize()};
}

} // namespace

void G2EditorProcessor::applySynthSettings()
{
    const auto* link = synth_.link();
    if (link == nullptr || !synth_.ready() || synth_.kind() != g2ui::SynthSync::Kind::Emulated)
        return;
    auto settings = link->state().settings;
    bool change = false;
    // A new emulated G2 starts with the OS's own system settings. It gets those a G2 leaves the factory with once,
    // sent as Clavia's editor sends them (g2emu::factorySettings: Global channel Off, Memory Protect On); the OS
    // stores them in its EEPROM. A memory in use keeps what its user set.
    if (emulatedFresh_) {
        emulatedFresh_ = false;
        settings = g2emu::factorySettings(settings);
        change = true;
    }
    // Following the host's tempo (opt-in) needs the G2 to follow an external clock.
    if (hostClockOn_ && settings.ignoreExternalClock) {
        settings.ignoreExternalClock = false;
        change = true;
    }
    if (change)
        synth_.setSynthSettings(settings);
}

void G2EditorProcessor::setMixOut34(bool on)
{
    mixOut34_ = on;
#if G2FRESH_EMULATOR
    if (emulated_ != nullptr)
        static_cast<EmulatedSoundEngine&>(*emulated_).setMixOut34(on);
#endif
    refreshState();
}

void G2EditorProcessor::setMicInput(bool on)
{
    micInput_ = on;
    if (emulated_ != nullptr)
        static_cast<EmulatedSoundEngine&>(*emulated_).setMic(on, micLevel_);
    refreshState();
}

void G2EditorProcessor::setHostClock(bool on)
{
    hostClockOn_ = on;
    resetClock_ = true;
    applySynthSettings();
    refreshState();
}

int G2EditorProcessor::emulatorsInHost() const
{
    return EmulatedSoundEngine::running();
}

namespace {

// The buffer the user chose for this computer (Synth > Emulated G2 Buffer), from the settings every instance
// shares. The environment variable G2FRESH_EMULATOR_BUFFER (low, normal or safe) goes first: for tests, which do
// not touch the user's settings.
g2io::Buffer chosenBuffer()
{
    const auto env = juce::SystemStats::getEnvironmentVariable("G2FRESH_EMULATOR_BUFFER", {});
    const auto key = env.isNotEmpty() ? env : g2ui::userSettings().getValue("emulatorBuffer");
    return g2io::bufferFromKey(key.toStdString(), g2io::kDefaultBuffer);
}

// Whether the user fitted the voice expansion board (Synth > Emulated G2 Voice Expansion Board), from the settings
// every instance shares: a choice for this computer, which must run twice the DSPs. G2FRESH_EMULATOR_EXPANSION (on or
// off) goes first: for tests, which do not touch the user's settings.
bool chosenExpansion()
{
    const auto env = juce::SystemStats::getEnvironmentVariable("G2FRESH_EMULATOR_EXPANSION", {});
    const auto key = env.isNotEmpty() ? env : g2ui::userSettings().getValue("emulatorExpansion");
    return g2io::expansionFromKey(key.toStdString());
}

// The DSP threads the computer test found fastest with the board, for this computer (-1, never tested:
// Machine::autoThreads).
int chosenExpansionThreads()
{
    return g2ui::userSettings().getIntValue("emulatorExpansionThreads", -1);
}

} // namespace

bool G2EditorProcessor::emulatorExpansion() const
{
    return chosenExpansion();
}

void G2EditorProcessor::setEmulatorExpansion(bool on)
{
    const auto key = g2io::expansionKey(on);
    g2ui::userSettings().setValue("emulatorExpansion", juce::String(key.data(), key.size()));
    // The OS finds the board at power-on only (re/notes §3.12): the running emulated G2 restarts, its memory kept.
    // Other instances take the choice when they next start.
    if (emulated_ != nullptr && static_cast<EmulatedSoundEngine&>(*emulated_).expansion() != on)
        if (const auto error = startEmulator(emulatorFirmware_); error.isNotEmpty())
            DBG("G2fresh: " << error);
}

bool G2EditorProcessor::testExpansion(std::function<void(const g2ui::ExpansionTest&)> done)
{
    if (testingExpansion_)
        return false;
    if (expansionTest_.joinable())
        expansionTest_.join(); // the last one's thread, done
    const juce::File firmware = emulatorFirmware_ != juce::File() ? emulatorFirmware_
                                                                 : juce::File(g2ui::userSettings().getValue("emulatorFirmware"));
    if (!firmware.exists())
        return false;
    // The running emulated G2 would take the CPU the test measures: it stops (its memory kept) and starts again after
    const bool wasRunning = emulated_ != nullptr;
    if (wasRunning) {
        captureFlash();
        synth_.disconnect();
        setEmulated(nullptr);
    }
    testingExpansion_ = true;
    stopExpansionTest_ = false;
    expansionTest_ = std::thread([this, firmware, wasRunning, done = std::move(done), alive = std::weak_ptr<int>(alive_)] {
        g2ui::ExpansionTest t;
        std::vector<std::pair<int, double>> tried;
        try {
            const auto fw = g2emu::Firmware::load(firmware.getFullPathName().toStdString());
            const auto* data = reinterpret_cast<const std::uint8_t*>(G2BenchData::B13_Voices_Heavy_pch2);
            const auto patch = std::get<g2::Patch>(
                g2::load(std::span(data, static_cast<std::size_t>(G2BenchData::B13_Voices_Heavy_pch2Size))).content);
            for (const int threads : g2io::kExpansionTestThreads) {
                g2emu::Machine::Options o;
                g2io::pluginMachine(o, true, threads);
                const auto r = g2emu::measureSpeed(fw, o, patch, g2io::kExpansionTestNotes, g2io::kExpansionTestSeconds,
                                                   &stopExpansionTest_);
                if (!r.error.empty()) {
                    t.error = r.error;
                    break;
                }
                tried.emplace_back(r.threads, r.speed);
                if (r.speed > t.speed) {
                    t.speed = r.speed;
                    t.voices = r.voices;
                }
            }
        } catch (const std::exception& e) {
            t.error = e.what();
        }
        t.threads = g2io::fastestThreads(tried);
        const auto advice = g2io::expansionAdvice(t.speed);
        t.advice = advice == g2io::ExpansionAdvice::Holds       ? g2ui::ExpansionTest::Holds
                 : advice == g2io::ExpansionAdvice::QuietOnly   ? g2ui::ExpansionTest::QuietOnly
                                                                 : g2ui::ExpansionTest::TooSlow;
        t.othersRunning = EmulatedSoundEngine::running();
        juce::MessageManager::callAsync([this, t, firmware, wasRunning, done, alive] {
            if (!alive.lock())
                return;
            testingExpansion_ = false;
            if (t.error.isEmpty())
                g2ui::userSettings().setValue("emulatorExpansionThreads", t.threads);
            if (wasRunning && emulated_ == nullptr)
                if (const auto error = startEmulator(firmware); error.isNotEmpty())
                    DBG("G2fresh: " << error);
            if (done)
                done(t);
        });
    });
    return true;
}

int G2EditorProcessor::emulatorBuffer() const
{
    return static_cast<int>(chosenBuffer());
}

void G2EditorProcessor::setEmulatorBuffer(int buffer)
{
    const auto key = g2io::bufferKey(g2io::bufferOf(buffer));
    g2ui::userSettings().setValue("emulatorBuffer", juce::String(key.data(), key.size()));
    applyBuffer();
}

void G2EditorProcessor::applyBuffer()
{
    if (emulated_ == nullptr)
        return;
    auto& engine = static_cast<EmulatedSoundEngine&>(*emulated_);
    if (const auto chosen = chosenBuffer(); engine.buffer() != chosen) {
        engine.setBuffer(chosen);
        setLatencySamples(engine.latencySamples()); // the host compensates the new latency
    }
}

double G2EditorProcessor::emulatorLatencyMs(int buffer) const
{
    if (emulated_ == nullptr || sampleRate_ <= 0)
        return 0.0;
    return static_cast<EmulatedSoundEngine&>(*emulated_).latencySamples(g2io::bufferOf(buffer)) * 1000.0 / sampleRate_;
}

int G2EditorProcessor::emulatorDropouts() const
{
    return emulated_ != nullptr ? static_cast<int>(static_cast<EmulatedSoundEngine&>(*emulated_).dropouts()) : 0;
}

std::vector<std::uint8_t> G2EditorProcessor::startingFlash() const
{
    // the project's, else (a new instance, or the stand-alone app) the one kept next to the settings
    if (wrapperType != wrapperType_Standalone && flashGz_.getSize() > 0)
        return gunzip(flashGz_);
    return EmulatedSoundEngine::readFlash(EmulatedSoundEngine::defaultFlashFile());
}

void G2EditorProcessor::captureFlash()
{
    if (emulated_ == nullptr)
        return;
    const auto& engine = static_cast<EmulatedSoundEngine&>(*emulated_);
    flashSeen_ = engine.flashChanges();
    flashGz_ = gzip(engine.flashSnapshot());
    refreshState();
}

void G2EditorProcessor::checkCapture()
{
    const auto folder = EmulatedSoundEngine::defaultFlashFile().getParentDirectory();
    const auto rate = sampleRate_ > 0 ? sampleRate_ : 48000.0;
    if (!capturing_ && folder.getChildFile("capture").existsAsFile()) {
        folder.getChildFile("capture").deleteFile();
        const auto seconds = juce::jlimit(1, 120, juce::SystemStats::getEnvironmentVariable("G2FRESH_CAPTURE_SECONDS", "20").getIntValue());
        capture_.assign(static_cast<std::size_t>(rate * seconds) * 2, 0.0f);
        captured_ = 0;
        captureMissing_ = emulated_ != nullptr ? static_cast<EmulatedSoundEngine&>(*emulated_).framesMissing() : 0;
        capturing_.store(true, std::memory_order_release);
        return;
    }
    if (!capturing_ || captured_.load(std::memory_order_acquire) < capture_.size() / 2)
        return;
    capturing_ = false;
    const auto missing = (emulated_ != nullptr ? static_cast<EmulatedSoundEngine&>(*emulated_).framesMissing() : 0) - captureMissing_;
    const auto file = juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("G2fresh capture.wav");
    juce::AudioBuffer<float> audio(2, static_cast<int>(capture_.size() / 2));
    for (int i = 0; i < audio.getNumSamples(); ++i)
        for (int c = 0; c < 2; ++c)
            audio.setSample(c, i, capture_[static_cast<std::size_t>(i) * 2 + static_cast<std::size_t>(c)]);
    file.deleteFile();
    if (auto stream = file.createOutputStream()) {
        juce::WavAudioFormat wav;
        if (auto writer = std::unique_ptr<juce::AudioFormatWriter>(wav.createWriterFor(stream.release(), rate, 2, 32, {}, 0)))
            writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    }
    file.withFileExtension(".txt").replaceWithText("sample rate " + juce::String(rate) + ", block " + juce::String(blockSize_)
                                                   + ", wrapper " + juce::String(static_cast<int>(wrapperType))
                                                   + ", 96 kHz frames missing during the capture: " + juce::String(missing)
                                                   + (emulated_ != nullptr ? "\n" + static_cast<EmulatedSoundEngine&>(*emulated_).diagnostics() : juce::String())
                                                   + "\n");
}

void G2EditorProcessor::timerCallback()
{
    checkCapture();
    if (emulated_ == nullptr)
        return;
    applyBuffer(); // another instance may have changed the setting
    // dropouts are for the user to see (the editor's status line): never traded silently for latency
    if (const int dropouts = emulatorDropouts(); dropouts != dropoutsShown_) {
        dropoutsShown_ = dropouts;
        synth_.sendChangeMessage();
    }
    // The emulated G2 runs but the link gave up on it (it ran too slowly to answer in time, e.g. while the host
    // loaded a project): talk to it again, and send it the patch once it answers.
    if (synth_.kind() == g2ui::SynthSync::Kind::Emulated && synth_.linkFailed()) {
        emulatedSentGeneration_ = -1;
        emulatedLocal_ = false;
        synth_.restartLink();
        return;
    }
    // The OS changed the flash (a stored patch, a setting): into the state once it is quiet for a second. Not before a
    // new G2 got its factory settings: until then the project keeps the empty memory, so that they still apply.
    if (emulatedFresh_)
        return;
    const auto changes = static_cast<EmulatedSoundEngine&>(*emulated_).flashChanges();
    if (changes == flashSeen_) {
        flashQuiet_ = 0;
        return;
    }
    if (++flashQuiet_ < 2)
        return;
    flashQuiet_ = 0;
    captureFlash();
}

void G2EditorProcessor::autoStartEmulator()
{
    if (!emulatorWanted_ || emulated_ != nullptr || testingExpansion_)
        return; // (the computer test stopped it and starts it again)
    if (emulated_ == nullptr && synth_.kind() != g2ui::SynthSync::Kind::None)
        return; // the editor talks to another synth
    const juce::File firmware{g2ui::userSettings().getValue("emulatorFirmware")};
    if (!firmware.exists())
        return; // never set up: Synth > Connect to Emulated G2 asks for it
    // One emulated G2 starts by itself per host: each takes about 1.5 cores, and a few of them overload a laptop
    // (they then drop out, which sounds like distortion). The others start from the Synth menu or the Live view.
    if (EmulatedSoundEngine::running() > 0)
        return;
    if (const auto error = startEmulator(firmware); error.isNotEmpty())
        DBG("G2fresh: " << error);
}

juce::String G2EditorProcessor::startEmulator(const juce::File& firmware)
{
    // One emulated G2 at a time: the running one hands its flash over.
    if (emulated_ != nullptr) {
        captureFlash();
        synth_.disconnect();
        setEmulated(nullptr);
    }
    std::unique_ptr<EmulatedSoundEngine> engine;
    try {
        const auto fw = g2emu::Firmware::load(firmware.getFullPathName().toStdString());
        if (fw.code() == nullptr)
            return "no G2 OS in " + firmware.getFileName();
        // The stand-alone app keeps its flash in a file; a plugin instance in the host's project.
        const auto saveTo = wrapperType == wrapperType_Standalone ? EmulatedSoundEngine::defaultFlashFile() : juce::File();
        auto flash = startingFlash();
        emulatedFresh_ = g2emu::newMemory(flash);
        engine = std::make_unique<EmulatedSoundEngine>(fw, std::move(flash), saveTo, chosenExpansion(),
                                                       chosenExpansionThreads());
    } catch (const std::exception& e) {
        return juce::String("the emulated G2 cannot start: ") + e.what();
    }
    emulatorFirmware_ = firmware;
    engine->setBuffer(chosenBuffer());
    engine->prepare(sampleRate_ > 0 ? sampleRate_ : 48000.0, blockSize_);
    engine->setMixOut34(mixOut34_);
    engine->setMic(micInput_, micLevel_);
    engine->machine().panelAnalog(g2emu::PanelAnalog::MasterLevel, masterLevel_); // the knob where it was left
    flashSeen_ = engine->flashChanges();
    synth_.connectEmulated(g2emu::emulatedG2Link(engine->machine()));
    emulatedSentGeneration_ = -1;
    emulatedLocal_ = false;
    emulatorWanted_ = true;
    dropoutsShown_ = 0;
    setLatencySamples(engine->latencySamples());
    setEmulated(std::move(engine));
    // A new instance that started from the stand-alone app's memory keeps it in the project from now on, so that the
    // project reopens with it and not with what the stand-alone app holds by then. (A new G2's empty memory goes in
    // once it has its factory settings: checkFlash.)
    if (wrapperType != wrapperType_Standalone && !emulatedFresh_ && flashGz_.getSize() == 0)
        captureFlash();
    refreshState();
    return {};
}
#else
// Without the emulator: no emulated G2 to keep or set up.
void G2EditorProcessor::timerCallback() {}
void G2EditorProcessor::applySynthSettings() {}
#endif

#if G2FRESH_EMULATOR
void G2EditorProcessor::emulatorMidi(std::span<const std::uint8_t> bytes)
{
    // The machine's MIDI queue is thread-safe; emulated_ changes only on this thread.
    if (emulated_ != nullptr)
        static_cast<EmulatedSoundEngine&>(*emulated_).machine().midiIn(bytes);
}
#endif

#if G2FRESH_EMULATOR
namespace {

// The Live view's panel (ui/G2Panel.h) <-> the emulated panel (g2emu/panel.hpp).
std::optional<g2emu::PanelButton> toEmu(g2ui::PanelButton b)
{
    using U = g2ui::PanelButton;
    using E = g2emu::PanelButton;
    const int i = static_cast<int>(b);
    auto offset = [](E first, int n) { return static_cast<E>(static_cast<int>(first) + n); };
    if (b >= U::Knob1 && b <= U::Knob8)
        return offset(E::Button1, i - static_cast<int>(U::Knob1));
    if (b >= U::Var1 && b <= U::Var8)
        return offset(E::Variation1, i - static_cast<int>(U::Var1));
    if (b >= U::PageA && b <= U::PageE)
        return offset(E::PageA, i - static_cast<int>(U::PageA));
    if (b >= U::Column1 && b <= U::Column3)
        return offset(E::Page1, i - static_cast<int>(U::Column1));
    if (b >= U::SlotA && b <= U::SlotD)
        return offset(E::SlotA, i - static_cast<int>(U::SlotA));
    switch (b) {
    case U::System: return E::System;
    case U::Patch: return E::Patch;
    case U::Store: return E::Store;
    case U::DisplayMode: return E::DisplayMode;
    case U::NavUp: return E::NavUp;
    case U::NavLeft: return E::NavLeft;
    case U::NavRight: return E::NavRight;
    case U::NavDown: return E::NavDown;
    case U::LoadPatch: return E::LoadPatch;
    case U::PerformanceMode: return E::PerfMode;
    case U::KbSplit: return E::KbSplit;
    case U::OctaveDown: return E::OctaveDown;
    case U::OctaveUp: return E::OctaveUp;
    case U::KbHold: return E::KbHold;
    case U::FocusCopy: return E::FocusCopy;
    case U::Shift: return E::Shift;
    case U::Morph: return E::Morph;
    case U::PatchSettings: return E::PatchSettings;
    default: return std::nullopt;
    }
}

// The view's LED for each emulated one (g2emu::PanelLed order).
g2ui::PanelLed toUi(g2emu::PanelLed l)
{
    using U = g2ui::PanelLed;
    using E = g2emu::PanelLed;
    const int i = static_cast<int>(l);
    auto from = [i](E first, U uiFirst) { return static_cast<U>(static_cast<int>(uiFirst) + i - static_cast<int>(first)); };
    if (l >= E::SlotA && l <= E::SlotD) return from(E::SlotA, U::FocusA);       // Active Slots/Focus, below the buttons
    if (l >= E::SlotKbA && l <= E::SlotKbD) return from(E::SlotKbA, U::SlotA);  // Keyboard Assign, above them
    if (l >= E::OctaveMinus2 && l <= E::OctavePlus2) return from(E::OctaveMinus2, U::Octave1);
    if (l >= E::KbSplit1 && l <= E::KbSplit4) return from(E::KbSplit1, U::Split1);
    if (l >= E::Variation1 && l <= E::Variation8) return from(E::Variation1, U::Var1);
    if (l >= E::PageA && l <= E::PageE) return from(E::PageA, U::PageA);
    if (l >= E::Page1 && l <= E::Page3) return from(E::Page1, U::Column1);
    if (l >= E::Knob1 && l <= E::Knob8) return from(E::Knob1, U::KnobUpper1);
    if (l >= E::Button1 && l <= E::Button8) return from(E::Button1, U::KnobLower1);
    switch (l) {
    case E::Midi: return U::Midi;
    case E::Mic20: return U::MicLow;
    case E::Mic12: return U::MicMid;
    case E::Mic0: return U::MicHigh;
    case E::System: return U::System;
    case E::Patch: return U::Patch;
    case E::Store: return U::Store;
    case E::LoadPatch: return U::LoadPatch;
    case E::KbHold: return U::KbHold;
    case E::KbSplit: return U::KbSplit;
    case E::PerfMode: return U::PerformanceMode;
    case E::SubFunc: return U::SubFunc;
    case E::PatchSettings: return U::PatchSettings;
    case E::GlobalPanel: return U::GlobalPanel;
    case E::Morph: return U::Morph;
    default: return U::Count;
    }
}

} // namespace

g2ui::PanelSnapshot G2EditorProcessor::panel() const
{
    g2ui::PanelSnapshot s;
    if (emulated_ == nullptr)
        return s;
    const auto p = static_cast<EmulatedSoundEngine&>(*emulated_).machine().panel();
    s.live = true;
    s.generation = p.generation;
    for (std::size_t d = 0; d < p.displays.size() && d < s.displays.size(); ++d) {
        if (!p.displays[d].on)
            continue;
        auto& out = s.displays[d];
        out.columns = g2emu::PanelDisplay::Columns;
        out.rows = g2emu::PanelDisplay::Rows;
        for (int r = 0; r < out.rows; ++r)
            out.text.push_back(p.displays[d].text(r));
    }
    for (int i = 0; i < g2emu::PanelLedCount; ++i)
        if (const auto u = toUi(static_cast<g2emu::PanelLed>(i)); u != g2ui::PanelLed::Count)
            s.leds[static_cast<std::size_t>(u)] = p.leds[static_cast<std::size_t>(i)] ? 1.0f : 0.0f;
    // the wheels' LEDs are not the OS's: lit while the instrument is on
    for (auto l : {g2ui::PanelLed::ModWheel, g2ui::PanelLed::GlobalWheel1, g2ui::PanelLed::GlobalWheel2})
        s.leds[static_cast<std::size_t>(l)] = 1.0f;
    for (std::size_t k = 0; k < s.rings.size(); ++k)
        for (const bool on : p.rings[k])
            s.rings[k].push_back(on ? 1.0f : 0.0f);
    return s;
}

void G2EditorProcessor::panelButton(g2ui::PanelButton button, bool down)
{
    if (emulated_ != nullptr)
        if (const auto b = toEmu(button))
            static_cast<EmulatedSoundEngine&>(*emulated_).machine().panelButton(*b, down);
}

void G2EditorProcessor::panelEncoder(int encoder, int steps)
{
    if (emulated_ != nullptr && encoder >= 0 && encoder < g2emu::PanelEncoderCount)
        static_cast<EmulatedSoundEngine&>(*emulated_).machine().panelEncoder(static_cast<g2emu::PanelEncoder>(encoder), steps);
}

void G2EditorProcessor::panelAnalog(g2ui::PanelAnalog control, float value)
{
    using U = g2ui::PanelAnalog;
    using E = g2emu::PanelAnalog;
    if (control == U::MasterLevel) { // kept with the project, as the knob stays where it is on a G2
        masterLevel_ = std::clamp(value, 0.0f, 1.0f);
        refreshState();
    }
    if (control == U::MicLevel) { // not the OS's: the microphone preamp's gain (kept with the project too)
        micLevel_ = std::clamp(value, 0.0f, 1.0f);
        if (emulated_ != nullptr)
            static_cast<EmulatedSoundEngine&>(*emulated_).setMic(micInput_, micLevel_);
        refreshState();
        return;
    }
    if (emulated_ == nullptr)
        return;
    const E e = control == U::MasterLevel ? E::MasterLevel : control == U::PitchStick ? E::PitchStick
              : control == U::ModWheel    ? E::ModWheel    : control == U::GlobalWheel1 ? E::GlobalWheel1
                                                                                         : E::GlobalWheel2;
    static_cast<EmulatedSoundEngine&>(*emulated_).machine().panelAnalog(e, value);
}

bool G2EditorProcessor::panelKey(int midiNote, bool down, float velocity)
{
    // The G2X's 61 keys: key 0 is C1 (MIDI 36), as the OS plays it (test "on a G2X: the lowest key...").
    const int key = midiNote - 36;
    if (emulated_ == nullptr || key < 0 || key > 60)
        return false;
    static_cast<EmulatedSoundEngine&>(*emulated_).machine().panelKey(key, down, std::clamp(static_cast<int>(std::lround(velocity * 127)), 1, 127));
    return true;
}
#endif

void G2EditorProcessor::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &synth_) {
        using Kind = g2ui::SynthSync::Kind;
        if (emulated_ != nullptr && synth_.kind() != Kind::Emulated) { // the editor left the emulated G2
#if G2FRESH_EMULATOR
            captureFlash();
#endif
            emulatorWanted_ = false;
            setLatencySamples(0);
            setEmulated(nullptr);
            refreshState();
        }
        // The G2's system settings, once it is up: only what the user asked for, or a factory G2's.
        if (synth_.kind() == Kind::Emulated && synth_.ready() && !emulatedLocal_) {
            emulatedLocal_ = true;
            applySynthSettings();
        }
        // Once up, the emulated G2 plays the document, and each one opened or made since (EmulatorFollow.h).
        if (g2io::emulatorSendsDocument(synth_.kind() == Kind::Emulated, synth_.ready(), synth_.bound(),
                                        document_.generation(), emulatedSentGeneration_)) {
            emulatedSentGeneration_ = document_.generation();
            if (document_.isPerformance())
                synth_.sendPerformance();
            else
                synth_.sendPatchAlone();
        }
        return;
    }
    refreshState();
}

void G2EditorProcessor::refreshState()
{
    // Runs from the message loop, where an exception would end the app: if the
    // patch cannot be encoded, keep the last state that could.
    std::vector<std::uint8_t> bytes;
    try {
        bytes = encodeState();
    } catch (const std::exception& e) {
        DBG("G2fresh: cannot encode the state: " << e.what());
        return;
    }
    const juce::SpinLock::ScopedLockType lock(stateLock_);
    stateBytes_ = std::move(bytes);
}

void G2EditorProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    midiOut_.prepare(sampleRate);
    clockMidi_.ensureSize(4096); // the track's MIDI plus a block's clock, without allocating on the audio thread
    resetClock_ = true;
    const juce::ScopedLock lock(getCallbackLock());
    sampleRate_ = sampleRate;
    blockSize_ = samplesPerBlock;
    inputs_.setSize(4, std::max(1, samplesPerBlock));
    if (emulated_ != nullptr) {
        emulated_->prepare(sampleRate, samplesPerBlock);
#if G2FRESH_EMULATOR
        setLatencySamples(static_cast<EmulatedSoundEngine&>(*emulated_).latencySamples());
#endif
    }
}

bool G2EditorProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // The rule is g2io::layoutSupported's: Out 1/2 stereo or mono, Out 3/4 off or stereo next to a stereo Out 1/2;
    // In 1/2 off, mono or stereo, In 3/4 off or stereo; or a single input bus of up to 4 channels (stand-alone)
    if (layouts.inputBuses.size() > 2 || layouts.outputBuses.size() > 2)
        return false;
    int in[2] = {0, 0}, out[2] = {0, 0};
    for (int b = 0; b < layouts.inputBuses.size(); ++b)
        in[b] = channelsOf(layouts.getChannelSet(true, b), true);
    for (int b = 0; b < layouts.outputBuses.size(); ++b)
        out[b] = channelsOf(layouts.getChannelSet(false, b), false);
    return g2io::layoutSupported({in, static_cast<std::size_t>(layouts.inputBuses.size())},
                                 {out, static_cast<std::size_t>(layouts.outputBuses.size())});
}

void G2EditorProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    midiOut_.process(midi); // the track's MIDI to the G2's MIDI IN
#if G2FRESH_EMULATOR
    if (emulated_ != nullptr) { // the editor's synth is the emulated G2: it plays
        auto& emulated = static_cast<EmulatedSoundEngine&>(*emulated_);
        emulated.setOffline(isNonRealtime());
        // The host's tempo and transport as MIDI clock, next to the track's MIDI
        auto* toG2 = &midi;
        if (resetClock_.exchange(false))
            hostClock_.reset();
        if (hostClockOn_.load(std::memory_order_relaxed)) {
            if (auto* head = getPlayHead()) {
                if (const auto pos = head->getPosition()) {
                    HostClock::Position p;
                    p.bpm = pos->getBpm().orFallback(0.0);
                    p.playing = pos->getIsPlaying();
                    if (const auto ppq = pos->getPpqPosition())
                        p.ppq = *ppq;
                    clockMidi_.clear();
                    clockMidi_.addEvents(midi, 0, buffer.getNumSamples(), 0);
                    hostClock_.process(p, getSampleRate(), buffer.getNumSamples(),
                                       [this](int at, std::uint8_t status, std::uint8_t d1, std::uint8_t d2, int size) {
                                           const std::uint8_t bytes[3] = {status, d1, d2};
                                           clockMidi_.addEvent(bytes, size, at);
                                       });
                    toG2 = &clockMidi_;
                }
            }
        }
        // The host's inputs as IN 1-4 (the first bus from IN 1, "In 3/4" from IN 3), taken before the outputs are
        // written: they share the buffer's channels.
        const int samples = buffer.getNumSamples();
        if (inputs_.getNumSamples() < samples)
            inputs_.setSize(4, samples, false, false, true); // a block larger than the host announced
        inputs_.clear();
        for (int bus = 0; bus < getBusCount(true); ++bus)
            for (int ch = 0; ch < getChannelCountOfBus(true, bus); ++ch)
                if (const int input = g2io::inputOf(bus, ch); input >= 0)
                    inputs_.copyFrom(input, 0, buffer, getChannelIndexInProcessBlockBuffer(true, bus, ch), 0, samples);
        // The outputs are the buffer's first channels; a channel only the inputs use is left silent
        const int outs = std::min(getTotalNumOutputChannels(), buffer.getNumChannels());
        juce::AudioBuffer<float> out(buffer.getArrayOfWritePointers(), outs, samples);
        emulated.render(out, *toG2, &inputs_);
        for (int c = outs; c < buffer.getNumChannels(); ++c)
            buffer.clear(c, 0, samples);
        // The plugin's MIDI output is the G2's MIDI OUT, nothing else: what came in is not passed on (a G2 has a
        // MIDI THRU jack for that, which a host does itself).
        midi.clear();
        emulated.takeMidiOut(midi);
        if (capturing_.load(std::memory_order_acquire)) {
            const auto at = captured_.load(std::memory_order_relaxed);
            const auto n = std::min<std::size_t>(static_cast<std::size_t>(buffer.getNumSamples()), capture_.size() / 2 - at);
            for (std::size_t i = 0; i < n; ++i)
                for (int c = 0; c < 2; ++c)
                    capture_[(at + i) * 2 + static_cast<std::size_t>(c)] = buffer.getSample(std::min(c, outs - 1), static_cast<int>(i));
            captured_.store(at + n, std::memory_order_release);
        }
        return;
    }
#endif
    buffer.clear();
    midi.clear();
}

juce::AudioProcessorEditor* G2EditorProcessor::createEditor()
{
    return new G2EditorView(*this);
}

void G2EditorProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    const juce::SpinLock::ScopedLockType lock(stateLock_);
    destData.replaceAll(stateBytes_.data(), stateBytes_.size());
}

void G2EditorProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::vector<std::uint8_t> bytes(p, p + sizeInBytes);
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        loadState(std::move(bytes));
    else
        juce::MessageManager::callAsync([this, alive = std::weak_ptr<int>(alive_), b = std::move(bytes)]() mutable {
            if (alive.lock())
                loadState(std::move(b));
        });
}

void G2EditorProcessor::loadState(std::vector<std::uint8_t> bytes)
{
    if (bytes.empty())
        return;
    try {
        const auto state = juce::ValueTree::readFromData(bytes.data(), bytes.size());
        if (!state.hasType("G2fresh")) {
            document_.loadBytes(bytes, true); // v0.1: the raw patch file
            return;
        }
        juce::MemoryBlock data;
        data.fromBase64Encoding(state["data"].toString());
        const auto* p = static_cast<const std::uint8_t*>(data.getData());
        document_.loadBytes(std::vector<std::uint8_t>(p, p + data.getSize()), true);
        const juce::String file = state["file"];
        if (file.isNotEmpty() && juce::File::isAbsolutePath(file))
            document_.setFile(juce::File(file));
        document_.setName(state["name"].toString());
        document_.setConvertedFrom(static_cast<int>(state.getProperty("convertedFrom", 0)));
        document_.applyLayoutJson(state["layout"].toString());
        midiOut_.restore(state["midiOut"].toString(), state["midiOutName"].toString(), state["midiChannel"]);
        emulatorWanted_ = g2io::emulatorAtLaunch(wrapperType == wrapperType_Standalone,
                                                 static_cast<bool>(state.getProperty("emulated", true)));
        hostClockOn_ = static_cast<bool>(state.getProperty("hostClock", false));
        masterLevel_ = static_cast<float>(state.getProperty("masterLevel", 1.0f));
        setMixOut34(static_cast<bool>(state.getProperty("mixOut34", false)));
        micLevel_ = static_cast<float>(state.getProperty("micLevel", 0.5f));
        micInput_ = static_cast<bool>(state.getProperty("micInput", false));
#if G2FRESH_EMULATOR
        if (emulated_ != nullptr)
            static_cast<EmulatedSoundEngine&>(*emulated_).setMic(micInput_, micLevel_);
#endif
        flashGz_.reset();
        flashGz_.fromBase64Encoding(state["emulatorFlash"].toString());
#if G2FRESH_EMULATOR
        // The project's emulated G2, with the project's flash. If the new instance's one already runs, it goes
        // without handing its flash over (that would replace the project's).
        if (emulated_ != nullptr && (!emulatorWanted_ || flashGz_.getSize() > 0)) {
            setLatencySamples(0);
            setEmulated(nullptr);
            synth_.disconnect();
        }
        autoStartEmulator();
#endif
        document_.markSaved();
        if (static_cast<bool>(state.getProperty("edited", false)))
            document_.markEdited();
    } catch (const std::exception& e) {
        DBG("G2fresh: could not restore the patch from the host: " << e.what());
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new G2EditorProcessor();
}
