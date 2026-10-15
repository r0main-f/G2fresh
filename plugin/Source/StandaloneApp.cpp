// The stand-alone app: one native window with the editor, the menu bar for
// File/Edit/View/Options (instead of JUCE's in-window "Options" strip), and
// the last session's patch restored at startup. Audio/MIDI Settings chooses
// the audio device, the MIDI inputs into the emulated G2's MIDI IN and the
// MIDI output its MIDI OUT goes to.
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "StandaloneAudio.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

#include <optional>
#include <string>
#include <vector>

namespace {

class MainWindow final : public juce::DocumentWindow {
public:
    explicit MainWindow(juce::StandalonePluginHolder& holder)
        : DocumentWindow(JucePlugin_Name, juce::Colour(0xff2e3035), DocumentWindow::allButtons), holder_(holder)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        if (auto* editor = holder.processor->createEditorAndMakeActive()) {
            if (auto* g2 = dynamic_cast<G2EditorView*>(editor))
                g2->mainView().onAudioSettings = [this] { holder_.showAudioSettingsDialog(); };
            setContentOwned(editor, true);
        }
        if (auto* props = holder.settings.get(); props && props->containsKey("windowBounds"))
            restoreWindowStateFromString(props->getValue("windowBounds"));
        else
            centreWithSize(getWidth(), getHeight());
    }

    ~MainWindow() override
    {
        if (auto* props = holder_.settings.get())
            props->setValue("windowBounds", getWindowStateAsString());
        clearContentComponent();
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    g2ui::MainView* mainView()
    {
        auto* editor = dynamic_cast<G2EditorView*>(getContentComponent());
        return editor ? &editor->mainView() : nullptr;
    }

private:
    juce::StandalonePluginHolder& holder_;
};

class G2App final : public juce::JUCEApplication, private juce::ChangeListener {
public:
    G2App()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = JucePlugin_Name;
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName = JucePlugin_Name;
        properties_.setStorageParameters(options);
    }

    const juce::String getApplicationName() override { return JucePlugin_Name; }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& commandLine) override
    {
        // No audio input until the user chooses one (StandaloneAudio.h): the setup the holder starts from names
        // the output alone.
        startOnOutputAlone();
        // The holder runs the (silent) processor and restores the last
        // session's patch from the settings.
        holder_ = std::make_unique<juce::StandalonePluginHolder>(properties_.getUserSettings(), false);
        // The emulated G2's MIDI OUT goes to the MIDI output chosen in Audio/MIDI Settings (JUCE shows the choice
        // because the processor produces MIDI). The dialog's own label says "MIDI Output:"; here it names the
        // jack, so that it is not taken for Options > Forward MIDI to a Hardware G2, which sends the incoming MIDI
        // to a real G2 instead.
        if (holder_->processor != nullptr && holder_->processor->producesMidi()) {
            juce::LocalisedStrings::setCurrentMappings(
                new juce::LocalisedStrings("language: English\n\n\"MIDI Output:\" = \"G2 MIDI OUT to:\"\n", false));
            holder_->deviceManager.addChangeListener(this);
            changeListenerCallback(nullptr);
        }
        // The audio device's inputs 1-4 are the G2's IN 1-4 (Audio Settings chooses them). JUCE mutes a plugin's
        // inputs until the user says otherwise, against feedback through an effect, and has saved that default since
        // before G2fresh had inputs. A G2 plays its inputs only where a patch has an In module, so they are live from
        // the first start with inputs on; from then on "Mute audio input" in Audio Settings is the user's.
        if (auto* settings = properties_.getUserSettings(); settings != nullptr && !settings->getBoolValue("audioInputsLive")) {
            settings->setValue("audioInputsLive", true);
            holder_->getMuteInputValue().setValue(false);
        }
        window_ = std::make_unique<MainWindow>(*holder_);
        window_->setVisible(true);
        anotherInstanceStarted(commandLine);
    }

    // Writes the setup the holder starts from ("audioSetup", as JUCE saves it) with an output and no input, unless
    // the user chose an input (g2io::standaloneStart). Only the device list is read: nothing is opened here.
    void startOnOutputAlone()
    {
        auto* settings = properties_.getUserSettings();
        if (settings == nullptr)
            return;
        auto xml = settings->getXmlValue("audioSetup");
        if (xml != nullptr && !xml->hasTagName("DEVICESETUP"))
            xml = nullptr;
        std::optional<g2io::SavedAudioSetup> saved;
        if (xml != nullptr) {
            const auto both = xml->getStringAttribute("audioDeviceName"); // one device both ways
            saved = g2io::SavedAudioSetup{
                (both.isNotEmpty() ? both : xml->getStringAttribute("audioInputDeviceName")).toStdString(),
                (both.isNotEmpty() ? both : xml->getStringAttribute("audioOutputDeviceName")).toStdString(),
                xml->getBoolAttribute(kWrittenByG2fresh)};
        }
        // The outputs of the saved setup's device type, or of the first (Core Audio, Windows Audio, ALSA)
        juce::AudioDeviceManager probe;
        juce::AudioIODeviceType* type = nullptr;
        for (auto* t : probe.getAvailableDeviceTypes())
            if (type == nullptr && (xml == nullptr || t->getTypeName() == xml->getStringAttribute("deviceType")))
                type = t;
        if (type == nullptr)
            return;
        type->scanForDevices();
        const auto names = type->getDeviceNames(false);
        std::vector<std::string> outputs;
        for (const auto& n : names)
            outputs.push_back(n.toStdString());
        const auto start = g2io::standaloneStart(saved ? &*saved : nullptr, outputs,
                                                 names[type->getDefaultDeviceIndex(false)].toStdString());
        if (!start || start->output.empty())
            return;
        auto setup = xml != nullptr ? std::make_unique<juce::XmlElement>(*xml) : std::make_unique<juce::XmlElement>("DEVICESETUP");
        setup->setAttribute("deviceType", type->getTypeName());
        setup->removeAttribute("audioDeviceName");
        setup->setAttribute("audioOutputDeviceName", juce::String(start->output));
        setup->setAttribute("audioInputDeviceName", juce::String());
        // G2fresh's choice, not the user's, when nothing was chosen or the chosen output has gone: it follows the
        // system's next time. JUCE writes the setup anew, without this mark, when the user changes it.
        setup->setAttribute(kWrittenByG2fresh, !saved || saved->byG2fresh || saved->output != start->output);
        settings->setValue("audioSetup", setup.get());
    }
    static constexpr const char* kWrittenByG2fresh = "g2freshNoInput";

    // Files opened from the Finder (or passed on the command line).
    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(commandLine.unquoted());
        if (commandLine.isNotEmpty() && file.existsAsFile() && window_)
            if (auto* view = window_->mainView())
                view->confirmDiscard([view, file] { view->openFile(file); });
    }

    void shutdown() override
    {
        if (holder_)
            holder_->deviceManager.removeChangeListener(this);
        if (holder_)
            holder_->savePluginState();
        window_ = nullptr;
        holder_ = nullptr;
        properties_.saveIfNeeded();
    }

    void systemRequestedQuit() override
    {
        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents()) {
            juce::Timer::callAfterDelay(100, [] {
                if (auto* app = juce::JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
            return;
        }
        if (auto* view = window_ ? window_->mainView() : nullptr)
            view->confirmDiscard([this, view] {
                view->discardChanges();
                if (holder_)
                    holder_->savePluginState();
                quit();
            });
        else
            quit();
    }

private:
    // The MIDI output device changed (or the app starts): its sender thread runs, so that JUCE's player sends each
    // message of a block at its own time (MidiOutput::sendBlockOfMessages) instead of all of them at once when the
    // block is made. They leave when the block is computed: ahead of its sound by the audio device's output latency.
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        if (holder_ == nullptr)
            return;
        if (auto* out = holder_->deviceManager.getDefaultMidiOutput(); out != nullptr && !out->isBackgroundThreadRunning())
            out->startBackgroundThread();
    }

    juce::ApplicationProperties properties_;
    std::unique_ptr<juce::StandalonePluginHolder> holder_;
    std::unique_ptr<MainWindow> window_;
};

} // namespace

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication()
{
    return new G2App();
}
