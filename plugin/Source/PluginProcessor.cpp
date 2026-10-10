#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Skin.h"

#if G2FRESH_EMULATOR
#include "EmulatedSoundEngine.h"
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
    state.setProperty("data", data.toBase64Encoding(), nullptr);
    state.setProperty("layout", doc.layoutJson(), nullptr); // cable shapes
    state.setProperty("midiOut", midiOut_.deviceIdentifier(), nullptr);
    state.setProperty("midiOutName", midiOut_.deviceName(), nullptr);
    state.setProperty("midiChannel", midiOut_.channel(), nullptr);
    state.setProperty("emulated", emulatorWanted_, nullptr);
    if (flashGz_.getSize() > 0)
        state.setProperty("emulatorFlash", flashGz_.toBase64Encoding(), nullptr);
    juce::MemoryOutputStream out;
    state.writeToStream(out);
    const auto* p = static_cast<const std::uint8_t*>(out.getData());
    return {p, p + out.getDataSize()};
}

G2EditorProcessor::G2EditorProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
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

void G2EditorProcessor::timerCallback()
{
    if (emulated_ == nullptr)
        return;
    // The emulated G2 runs but the link gave up on it (it ran too slowly to answer in time, e.g. while the host
    // loaded a project): talk to it again, and send it the patch once it answers.
    if (synth_.kind() == g2ui::SynthSync::Kind::Emulated && synth_.linkFailed()) {
        emulatedSent_ = false;
        emulatedLocal_ = false;
        synth_.restartLink();
        return;
    }
    // The OS changed the flash (a stored patch, a setting): into the state once it is quiet for a second.
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
    if (!emulatorWanted_ || emulated_ != nullptr)
        return;
    if (emulated_ == nullptr && synth_.kind() != g2ui::SynthSync::Kind::None)
        return; // the editor talks to another synth
    const juce::File firmware{g2ui::userSettings().getValue("emulatorFirmware")};
    if (!firmware.exists())
        return; // never set up: Synth > Connect to Emulated G2 asks for it
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
        engine = std::make_unique<EmulatedSoundEngine>(fw, startingFlash(), saveTo);
    } catch (const std::exception& e) {
        return juce::String("the emulated G2 cannot start: ") + e.what();
    }
    engine->prepare(sampleRate_ > 0 ? sampleRate_ : 48000.0, blockSize_);
    flashSeen_ = engine->flashChanges();
    synth_.connectEmulated(g2emu::emulatedG2Link(engine->machine()));
    emulatedSent_ = false;
    emulatedLocal_ = false;
    emulatorWanted_ = true;
    setLatencySamples(engine->latencySamples());
    setEmulated(std::move(engine));
    refreshState();
    return {};
}
#endif

#if G2FRESH_EMULATOR
void G2EditorProcessor::emulatorMidi(std::span<const std::uint8_t> bytes)
{
    // The machine's MIDI queue is thread-safe; emulated_ changes only on this thread.
    if (emulated_ != nullptr)
        static_cast<EmulatedSoundEngine&>(*emulated_).machine().midiIn(bytes);
}
#endif

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
        // An erased flash leaves the OS's MIDI Local Off: its panel's keys and controls would only go out as MIDI.
        if (synth_.kind() == Kind::Emulated && synth_.ready() && !emulatedLocal_) {
            emulatedLocal_ = true;
            if (const auto* link = synth_.link(); link != nullptr && !link->state().settings.localOn) {
                auto settings = link->state().settings;
                settings.localOn = true;
                synth_.setSynthSettings(settings);
            }
        }
        // Once up, the emulated G2 plays the document (later, the user sends what they want).
        if (synth_.kind() == Kind::Emulated && synth_.ready() && !synth_.bound() && !emulatedSent_) {
            emulatedSent_ = true;
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
    const juce::ScopedLock lock(getCallbackLock());
    sampleRate_ = sampleRate;
    blockSize_ = samplesPerBlock;
    if (emulated_ != nullptr) {
        emulated_->prepare(sampleRate, samplesPerBlock);
#if G2FRESH_EMULATOR
        setLatencySamples(static_cast<EmulatedSoundEngine&>(*emulated_).latencySamples());
#endif
    }
}

bool G2EditorProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void G2EditorProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    midiOut_.process(midi); // the track's MIDI to the G2's MIDI IN
#if G2FRESH_EMULATOR
    if (emulated_ != nullptr) { // the editor's synth is the emulated G2: it plays
        auto& emulated = static_cast<EmulatedSoundEngine&>(*emulated_);
        emulated.setOffline(isNonRealtime());
        emulated.render(buffer, midi);
        midi.clear();
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
        document_.applyLayoutJson(state["layout"].toString());
        midiOut_.restore(state["midiOut"].toString(), state["midiOutName"].toString(), state["midiChannel"]);
        emulatorWanted_ = static_cast<bool>(state.getProperty("emulated", true));
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
    } catch (const std::exception& e) {
        DBG("G2fresh: could not restore the patch from the host: " << e.what());
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new G2EditorProcessor();
}
