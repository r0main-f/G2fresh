// The stand-alone app: one native window with the editor, the menu bar for
// File/Edit/View/Options (instead of JUCE's in-window "Options" strip), and
// the last session's patch restored at startup.
#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

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

class G2App final : public juce::JUCEApplication {
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
        // The holder runs the (silent) processor and restores the last
        // session's patch from the settings.
        holder_ = std::make_unique<juce::StandalonePluginHolder>(properties_.getUserSettings(), false);
        window_ = std::make_unique<MainWindow>(*holder_);
        window_->setVisible(true);
        anotherInstanceStarted(commandLine);
    }

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
