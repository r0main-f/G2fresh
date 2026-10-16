// One patch area (VA or FX): draws its modules and cables and handles all
// editing gestures in it. Lives inside a juce::Viewport.
#pragma once

#include "HintOverlay.h"
#include "ModulePainter.h"
#include "PatchDocument.h"
#include "LiveLeds.h"
#include "SpecialControls.h"

#include "g2/special.hpp"

#include <functional>
#include <map>
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
    // A cable clicked on its cord: Delete removes its serial chain (as the
    // jack popup's Delete).
    bool hasHighlightedCable() const { return highlighted_.has_value(); }
    void deleteHighlightedCable();
    // Esc: the drag in progress stops and its release does nothing (CPatch::AbortUserActions @000d38d0 ->
    // CPatchBackground::ReleaseAllDragOperations @000e18d2): no module moves, no cable is made, the rubber band adds
    // nothing; a knob keeps the value it was dragged to, a held push button is released. False if none.
    bool abortDrag();
    const std::vector<std::uint8_t>& selection() const { return selection_; }
    void selectAll();
    // The module the parameter focus moved to: selected alone unless it is selected already.
    void selectFocused(std::uint8_t module);
    // Shake (Control+Space): a new twist for the area's curved cables.
    void shakeCables();
    void clearSelection();
    void deleteSelected();
    g2::edit::Clipboard copySelection() const;
    // Pastes below the selection (or at the first free row) and selects the result.
    void paste(const g2::edit::Clipboard& clip);
    void duplicateSelection();
    // Edit > Paste Params (Cmd-E): the clipboard's values into the selected modules of the same types.
    void pasteParams(const g2::edit::Clipboard& clip);
    // The editor-wide clipboard (shared by both areas).
    static g2::edit::Clipboard& clipboard()
    {
        static g2::edit::Clipboard clip;
        return clip;
    }
    // Called when the user clicks in this area (it becomes the paste target).
    std::function<void(AreaView*)> onActivated;
    // The arrow keys, for the patch window's parameter focus (its viewport would scroll with them).
    std::function<bool(const juce::KeyPress&)> onArrowKey;
    // The synth's part of the background pop-up (BackgroundMenu.h), with what each item does, from the window.
    std::function<void(juce::PopupMenu&)> addSynthItems;
    // The last MIDI CC the synth received (CPatch::GetLastUsedMidiController), for a parameter's MIDI Controller menu.
    std::function<std::optional<std::uint8_t>()> lastMidiCc;
    // The toolbar's colour (manual p60, p82): what new modules get; colourSelection paints the selected modules with one.
    void setNewModuleColour(std::uint8_t colour) { newModuleColour_ = colour; }
    void colourSelection(std::uint8_t colour);
    // F5-F8 held: the hint boxes over every parameter (HintOverlay.h); None takes them down.
    void setHintMode(HintMode mode);
    // Adds a module at the first free row of the first column, or at `where`.
    void addModule(std::uint8_t type, std::optional<juce::Point<int>> where = std::nullopt);
    // Adds a module from the module bar (double-click or Return), where the
    // original puts it: below the selection (g2::edit::insertPosition), or at
    // 0,0; then the selection is cleared.
    void insertModule(std::uint8_t type);

    // The module last added, moved, selected or edited here, and when (for
    // zooming onto it). nullopt if none, or if it was deleted.
    std::optional<juce::Point<int>> focusPoint() const;
    // How far the modules reach from the top-left corner, at 100% (0, 0 without modules).
    juce::Point<int> contentExtent() const;
    juce::uint32 lastTouchTime() const { return touchTime_; }

    void paint(juce::Graphics&) override;
    void resized() override;
    // Re-reads the look and animation settings.
    void settingsChanged();
    // Live LED and meter values to show (the synth connection), or nullptr.
    void setLiveLeds(const LiveLeds* leds);
    // The LED source started or stopped delivering values.
    void liveLedsChanged();
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
    void paintHints(juce::Graphics& g);
    HintMode hintMode_ = HintMode::None;
    void paintCables(juce::Graphics& g);
    void paintLiveLeds(juce::Graphics& g);
    void paintCable(juce::Graphics& g, juce::Point<float> from, juce::Point<float> to, juce::Colour c, bool flowing,
                    std::optional<g2::CableBend> bend = std::nullopt);
    void paintHighlight(juce::Graphics& g, const g2::Cable& cable);
    // Shows (or, with nullopt, clears) a cable clicked on its cord.
    void highlightCable(std::optional<g2::Cable> cable);
    // The highlighted cable as it is in the patch now (its bend may have changed).
    std::optional<g2::Cable> highlightedCable() const;
    void updateTimer();
    void timerCallback() override;
    void repaintModules();

    void setValue(const Hit& h, int value, bool coalesce);
    // A reset knob's triangle clicked (ResetKnob.h): the centre value, or (morph) the focused group's range cleared.
    void resetKnob(const Hit& h, bool morph);
    // Push buttons (momentary parameters): pressed while the mouse is down.
    bool isPushButton(const Hit& h) const;
    void pressMomentary(const Hit& h, int value);
    void clickControl(const Hit& h);
    void clickSpecial(const Hit& h, SpecialControls::Hit part);
    void showModuleMenu(std::uint8_t module);
    void showBackgroundMenu(juce::Point<int> where);
    // The module's Replace button: the modules of its group (g2::replace::menu), and the replacement as one undo step.
    void showReplaceMenu(std::uint8_t module);
    std::uint8_t newModuleColour_ = 0;
    void replaceModule(std::uint8_t module, std::uint8_t newType);
    void showJackMenu(const Jack& jack);
    void showControlMenu(const Hit& h);
    void showCableMenu(const g2::Cable& cable);
    std::optional<g2::Cable> cableAt(juce::Point<int> p) const;
    // A cable's curve; with a bend it passes through the point the user pulled.
    juce::Path cablePath(juce::Point<float> a, juce::Point<float> b, std::optional<g2::CableBend> bend = std::nullopt) const;
    // Both ends of a cable, or nullopt if a module is missing.
    std::optional<std::pair<juce::Point<float>, juce::Point<float>>> cableEnds(const g2::Cable& c) const;
    // A cable's bend point (where its curve passes), or nullopt if it has none.
    std::optional<juce::Point<float>> bendPoint(const g2::Cable& c) const;
    // The bent cable whose bend point is under p.
    std::optional<g2::Cable> bendPointAt(juce::Point<int> p) const;
    // Double-click on a cord: adds a bend point at `where`, or removes the cable's one.
    void toggleBendPoint(const g2::Cable& cable, juce::Point<float> where);
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

    // Value: knob drag; Range: Alt-drag sets the knob's morph range in the
    // focused morph group;
    // Module: moves the selection; Cable: patching; Band: rubber-band selection.
    // Bend: pulling a cable into shape.
    enum class Drag { None, Value, Range, Module, Cable, Band, Bend };
    Drag drag_ = Drag::None;
    bool pushed_ = false; // a push button is held
    Hit dragHit_;
    int dragStartValue_ = 0;
    int dragStartRange_ = 0;
    int rangeDragDistance_ = 0; // morph drag: pixels moved, pulled back at the ends of the range
    int rangeDragLastY_ = 0;
    juce::Point<int> dragOffset_;   // module drag: mouse offset in the module
    juce::Point<int> dragPos_;      // current mouse position
    std::optional<Jack> cableFrom_;
    bool moveDrag_ = false; // the cable drag began with a double click: it moves the jack's cables
    // The cable clicked on its cord: drawn on top, the others dimmed, its two
    // ends ringed and labelled; the rest of its serial chain glows with it.
    std::optional<g2::Cable> highlighted_;
    std::vector<g2::Cable> chain_;
    // While a jack is held: its serial chain, light blue (CPatchBackground::CableHighLight).
    std::vector<g2::Cable> heldChain_;
    // A knob's inc/dec pair held: which parameter, which way, whether Option made it a morph step, and the press's
    // number (each press is one undo step).
    struct IncDecPress {
        std::uint8_t module = 0, param = 0;
        int delta = 0;
        bool morph = false;
        int id = 0;
    };
    void startIncDec(const Hit& h, int delta, bool morph);
    void stepIncDec();
    std::optional<IncDecPress> incDec_;
    int incDecPresses_ = 0;
    struct Repeat : juce::Timer {
        std::function<void()> onTick;
        void timerCallback() override { onTick(); }
    };
    Repeat incDecRepeat_;
    // The area's cable twist (CCableRenderer+0x13): rand() & 0xFF when the area is made, never saved.
    int cableSeed_ = juce::Random::getSystemRandom().nextInt(256);
    bool showsCable(const g2::Cable& c) const;
    static juce::Colour cableColour(g2::CableColor c);
    const LiveLeds* liveLeds_ = nullptr;
    std::uint32_t ledGeneration_ = 0;
    juce::Rectangle<int> cableBounds_; // what the cables covered at the last paint
    // DrumSynth preset selectors' state (last preset), per module, as the original keeps it.
    std::map<std::uint8_t, g2::special::DrumPresetSelector> drumSelectors_;
    int shownGeneration_ = -1, shownSlot_ = -1; // the document and slot the selectors belong to
    std::optional<juce::Point<int>> dropCell_; // browser drag-over preview

    std::unique_ptr<ModulesLayer> modules_;
    std::unique_ptr<Overlay> overlay_;
    float flowPhase_ = 0.0f; // pixels the pulses have travelled
    int minWidth_ = 0, minHeight_ = 0;
    std::uint8_t touched_ = 0;
    juce::uint32 touchTime_ = 0;
};

} // namespace g2ui
