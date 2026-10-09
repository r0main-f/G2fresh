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
                 public juce::TooltipClient,
                 private juce::ChangeListener,
                 private juce::Timer {
public:
    AreaView(PatchDocument& doc, g2::Location location);
    ~AreaView() override;

    // Shows hover/drag information (parameter values, connector names).
    std::function<void(const juce::String&)> onStatus;
    // Asks to zoom by `factor` around `where` (local coordinates), from
    // Cmd + scroll wheel or a trackpad pinch.
    std::function<void(float factor, juce::Point<int> where)> onZoom;

    g2::Location location() const { return location_; }

    // Selection (click; Shift-click toggles; drag a rectangle on the background).
    bool hasSelection() const { return !selection_.empty(); }
    const std::vector<std::uint8_t>& selection() const { return selection_; }
    void selectAll();
    void clearSelection();
    void deleteSelected();
    g2::edit::Clipboard copySelection() const;
    // Pastes below the selection (or at the first free row) and selects the result.
    void paste(const g2::edit::Clipboard& clip);
    void duplicateSelection();
    // The editor-wide clipboard (shared by both areas).
    static g2::edit::Clipboard& clipboard()
    {
        static g2::edit::Clipboard clip;
        return clip;
    }
    // Called when the user clicks in this area (it becomes the paste target).
    std::function<void(AreaView*)> onActivated;
    // Adds a module at the first free row of the first column, or at `where`.
    void addModule(std::uint8_t type, std::optional<juce::Point<int>> where = std::nullopt);

    // The module last added, moved, selected or edited here, and when (for
    // zooming onto it). nullopt if none, or if it was deleted.
    std::optional<juce::Point<int>> focusPoint() const;
    juce::uint32 lastTouchTime() const { return touchTime_; }

    void paint(juce::Graphics&) override;
    void resized() override;
    // Re-reads the look and animation settings.
    void settingsChanged();
    // The area always covers at least this size (unzoomed pixels), so it fills
    // its viewport at any zoom.
    void setMinimumSize(int width, int height);
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&, float scaleFactor) override;
    bool keyPressed(const juce::KeyPress&) override;
    // Hovering a module's name shows its description.
    juce::String getTooltip() override;

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
    juce::String jackName(const Jack& j) const;
    juce::String describeCable(const g2::Cable& c) const;
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
    void paintHighlight(juce::Graphics& g, const g2::Cable& cable);
    // Shows (or, with nullopt, clears) a cable clicked on its cord.
    void highlightCable(std::optional<g2::Cable> cable);
    void updateTimer();
    void timerCallback() override;
    void repaintModules();

    void setValue(const Hit& h, int value, bool coalesce);
    void clickControl(const Hit& h);
    void showModuleMenu(std::uint8_t module);
    void showJackMenu(const Jack& jack);
    void showControlMenu(const Hit& h);
    void showCableMenu(const g2::Cable& cable);
    std::optional<g2::Cable> cableAt(juce::Point<int> p) const;
    juce::Path cablePath(juce::Point<float> a, juce::Point<float> b) const;
    bool isSelected(std::uint8_t module) const;
    void select(std::uint8_t module, bool toggle);
    void rename(std::uint8_t module);
    void status(const juce::String& s) { if (onStatus) onStatus(s); }
    void touch(std::uint8_t module)
    {
        touched_ = module;
        touchTime_ = juce::Time::getMillisecondCounter();
    }

    PatchDocument& doc_;
    g2::Location location_;
    std::vector<std::uint8_t> selection_;
    Hit hover_;

    // Value: knob drag; Range: Alt-drag sets the knob's morph range;
    // Module: moves the selection; Cable: patching; Band: rubber-band selection.
    enum class Drag { None, Value, Range, Module, Cable, Band };
    Drag drag_ = Drag::None;
    Hit dragHit_;
    int dragStartValue_ = 0;
    int dragStartRange_ = 0;
    std::uint8_t morphGroupForRange_ = 0; // the group Alt-drag assigns to unassigned knobs
    juce::Point<int> dragOffset_;   // module drag: mouse offset in the module
    juce::Point<int> dragPos_;      // current mouse position
    std::optional<Jack> cableFrom_;
    // The cable clicked on its cord: drawn on top, the others dimmed, its two
    // ends ringed and labelled.
    std::optional<g2::Cable> highlighted_;
    std::optional<juce::Point<int>> dropCell_; // browser drag-over preview

    std::unique_ptr<ModulesLayer> modules_;
    std::unique_ptr<Overlay> overlay_;
    float flowPhase_ = 0.0f; // pixels the pulses have travelled
    int minWidth_ = 0, minHeight_ = 0;
    std::uint8_t touched_ = 0;
    juce::uint32 touchTime_ = 0;
};

} // namespace g2ui
