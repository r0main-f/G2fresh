// One patch area (VA or FX): draws its modules and cables and handles all
// editing gestures in it. Lives inside a juce::Viewport.
#pragma once

#include "ModulePainter.h"
#include "PatchDocument.h"

#include <functional>
#include <optional>

namespace g2ui {

class AreaView : public juce::Component,
                 public juce::DragAndDropTarget,
                 private juce::ChangeListener,
                 private juce::Timer {
public:
    AreaView(PatchDocument& doc, g2::Location location);
    ~AreaView() override;

    // Shows hover/drag information (parameter values, connector names).
    std::function<void(const juce::String&)> onStatus;

    g2::Location location() const { return location_; }
    void deleteSelected();
    bool hasSelection() const { return selected_ != 0; }
    // Adds a module at the first free row of the first column, or at `where`.
    void addModule(std::uint8_t type, std::optional<juce::Point<int>> where = std::nullopt);

    void paint(juce::Graphics&) override;
    void resized() override;
    // Re-reads the look and animation settings.
    void settingsChanged();
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

private:
    struct Hit {
        std::uint8_t module = 0;            // 0: empty background
        const PanelElement* element = nullptr; // nullptr: module body
        juce::Point<int> local;              // point relative to the module
    };
    struct Jack {
        std::uint8_t module;
        std::uint8_t conn;
        bool isOutput;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void updateSize();
    Hit hitAt(juce::Point<int> p) const;
    std::optional<juce::Point<float>> jackCentre(const Jack& j) const;
    std::optional<Jack> jackAt(juce::Point<int> p) const;
    juce::String describe(const Hit& h) const;
    ModuleContext context(const g2::Module& m) const;
    juce::Point<int> gridCell(juce::Point<int> p) const;

    // Modules are drawn on a layer cached as an image; cables, drag previews
    // and the flow animation on an overlay above it, so animating cables
    // never re-renders the modules.
    class ModulesLayer;
    class Overlay;
    void paintModules(juce::Graphics& g);
    void paintOverlay(juce::Graphics& g);
    void paintCables(juce::Graphics& g);
    void paintCable(juce::Graphics& g, juce::Point<float> from, juce::Point<float> to, juce::Colour c, bool flowing);
    void timerCallback() override;
    void repaintModules();

    void setValue(const Hit& h, int value, bool coalesce);
    void clickControl(const Hit& h);
    void showModuleMenu(std::uint8_t module);
    void showJackMenu(const Jack& jack);
    void status(const juce::String& s) { if (onStatus) onStatus(s); }

    PatchDocument& doc_;
    g2::Location location_;
    std::uint8_t selected_ = 0;
    Hit hover_;

    enum class Drag { None, Value, Module, Cable };
    Drag drag_ = Drag::None;
    Hit dragHit_;
    int dragStartValue_ = 0;
    juce::Point<int> dragOffset_;   // module drag: mouse offset in the module
    juce::Point<int> dragPos_;      // current mouse position
    std::optional<Jack> cableFrom_;
    std::optional<juce::Point<int>> dropCell_; // browser drag-over preview

    std::unique_ptr<ModulesLayer> modules_;
    std::unique_ptr<Overlay> overlay_;
    float flowPhase_ = 0.0f; // pixels the pulses have travelled
};

} // namespace g2ui
