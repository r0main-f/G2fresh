#include "MutatorWindow.h"

#include "MutatorSet.h"

#include "Theme.h"

namespace g2ui {
namespace {

namespace mut = g2::mutate;
const juce::Colour kPanel(0xff2a2c31), kInk(0xffe6e8ec), kDim(0xff9aa0aa);

// One individual's box: click to audition it, right-click for more.
class BoxButton : public juce::Component {
public:
    std::function<void()> onClick;
    std::function<void()> onMenu;
    juce::String label;
    juce::Colour tint{0xff3478d4};
    bool filled = false, focused = false;

    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced(2.0f);
        g.setColour(filled ? tint.withAlpha(over_ ? 1.0f : 0.8f) : juce::Colour(0xff3a3d44));
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(focused ? juce::Colour(0xffff8c1a) : juce::Colour(0xff555a64));
        g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, focused ? 2.5f : 1.0f);
        g.setColour(filled ? juce::Colours::white : kDim);
        g.setFont(theme::font(focused));
        g.drawFittedText(label, getLocalBounds().reduced(4), juce::Justification::centred, 2);
    }
    void mouseEnter(const juce::MouseEvent&) override { over_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { over_ = false; repaint(); }
    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) {
            if (onMenu)
                onMenu();
        } else if (onClick && e.getDistanceFromDragStart() < 4) {
            onClick();
        }
    }

private:
    bool over_ = false;
};

} // namespace

class MutatorWindow::Panel : public juce::Component, private juce::ChangeListener {
public:
    explicit Panel(PatchDocument& doc)
        : doc_(doc), seed_(static_cast<std::uint32_t>(juce::Time::currentTimeMillis()))
    {
        auto op = [this](juce::TextButton& b, const juce::String& tip, std::function<void()> f) {
            b.setTooltip(tip);
            b.onClick = [this, f] {
                f();
                refresh();
            };
            addAndMakeVisible(b);
        };
        op(randomize_, "Six random versions of the current variation", [this] {
            mutator().randomize(doc_.patch(), static_cast<std::uint8_t>(doc_.variation()));
            status("Six random children of variation " + juce::String(doc_.variation() + 1));
        });
        op(mutate_, "Six small variations of the Mother (or of the current variation if the Mother box is empty)", [this] {
            if (!mutator().hasData(mut::kMother))
                copyFromVariation(mut::kMother);
            mutator().mutate(doc_.patch(), mut::kMother);
            status("Six mutants of the Mother");
        });
        op(cross_, "Six children mixing the Mother and the Father", [this] {
            if (mutator().cross(mut::kMother, mut::kFather))
                status("Six crosses of Mother and Father");
            else
                status("Cross needs a Mother and a Father (right-click a box to set them)");
        });
        op(interpolate_, "Six steps between the Mother and the Father", [this] {
            if (mutator().interpolate(mut::kMother, mut::kFather))
                status("Six steps from Mother to Father");
            else
                status("Interpolate needs a Mother and a Father (right-click a box to set them)");
        });

        for (int i = 0; i < 8; ++i) {
            auto* b = boxes_.add(new BoxButton());
            const mut::Box box{mut::BoxKind::Population, i};
            b->label = i == mut::kMotherBox ? "Mother" : i == mut::kFatherBox ? "Father" : juce::String::charToString(static_cast<juce::juce_wchar>('A' + i));
            b->tint = i == mut::kMotherBox ? juce::Colour(0xffd0608e) : i == mut::kFatherBox ? juce::Colour(0xff4a7fd0) : juce::Colour(0xff2f9e8f);
            b->onClick = [this, box] { audition(box); };
            b->onMenu = [this, box] { boxMenu(box); };
            addAndMakeVisible(b);
        }
        for (int i = 0; i < mut::kGeneBankSize; ++i) {
            auto* b = bank_.add(new BoxButton());
            const mut::Box box{mut::BoxKind::GeneBank, i};
            b->label = juce::String(i % 8 + 1);
            b->tint = juce::Colour(0xff8a6fd0);
            b->onClick = [this, box] { audition(box); };
            b->onMenu = [this, box] { boxMenu(box); };
            addAndMakeVisible(b);
        }
        for (int row = 0; row < 3; ++row) {
            auto* copy = rowCopy_.add(new juce::TextButton("To variations"));
            copy->setTooltip("Copy this row's 8 slots to variations 1-8");
            copy->onClick = [this, row] {
                doc_.perform("Copy gene bank to variations", [&](g2::Patch& p) {
                    const g2::Patch before = p;
                    mutator().copyRowToVariations(p, row);
                    // CDialogMutaBackground @0013f7b2: each variation's dump, no 6A.
                    for (std::uint8_t v = 0; v < g2::kUserVariations; ++v)
                        g2::live::recordVariationDump(before, p, v);
                });
                refresh();
            };
            auto* clear = rowClear_.add(new juce::TextButton("Clear"));
            clear->onClick = [this, row] {
                mutator().clearRow(row);
                refresh();
            };
            addAndMakeVisible(copy);
            addAndMakeVisible(clear);
        }

        auto slider = [this](juce::Slider& s, juce::Label& l, const juce::String& name, double value, double max) {
            s.setSliderStyle(juce::Slider::LinearHorizontal);
            s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 22);
            s.setRange(0.0, max, 1.0);
            s.setTextValueSuffix("%");
            s.setValue(value * 100.0, juce::dontSendNotification);
            l.setText(name, juce::dontSendNotification);
            l.setFont(theme::font());
            addAndMakeVisible(s);
            addAndMakeVisible(l);
        };
        // Probability 0-100 %, Range 0-50 % (the original's knob, CSmallKnob(..., 0x32)), Crossover 0-100 %.
        slider(probability_, probabilityLabel_, "Probability", mutator().settings.probability, 100.0);
        slider(range_, rangeLabel_, "Range", mutator().settings.range, mut::kMaxRange * 100.0);
        slider(crossover_, crossoverLabel_, "Crossover", mutator().settings.crossover, 100.0);
        probability_.onValueChange = [this] {
            mutator().settings.setProbability(probability_.getValue() / 100.0);
            range_.setValue(mutator().settings.range * 100.0, juce::dontSendNotification);
        };
        range_.onValueChange = [this] {
            mutator().settings.setRange(range_.getValue() / 100.0);
            probability_.setValue(mutator().settings.probability * 100.0, juce::dontSendNotification);
        };
        crossover_.onValueChange = [this] { mutator().settings.crossover = crossover_.getValue() / 100.0; };
        link_.setToggleState(mutator().settings.link, juce::dontSendNotification);
        link_.onClick = [this] {
            mutator().settings.setLink(link_.getToggleState());
            range_.setValue(mutator().settings.range * 100.0, juce::dontSendNotification);
        };
        link_.setTooltip("Link Probability and Range as the original does");
        addAndMakeVisible(link_);

        const std::pair<mut::Group, const char*> groups[] = {
            {mut::OscFreq, "Osc pitch"}, {mut::OscFine, "Osc fine"}, {mut::Envelope, "Envelopes"},
            {mut::SeqValue, "Seq values"}, {mut::SeqEvent, "Seq events"}, {mut::Delays, "Delays"},
            {mut::Effects, "Effects"}};
        for (const auto& [group, name] : groups) {
            auto* t = locks_.add(new juce::ToggleButton(name));
            t->setToggleState(mutator().settings.quickLocked(group), juce::dontSendNotification);
            t->setTooltip(juce::String("Keep ") + name + " unchanged");
            const auto gr = group;
            t->onClick = [this, t, gr] { mutator().settings.setQuickLock(gr, t->getToggleState()); };
            addAndMakeVisible(t);
        }
        lockLabel_.setText("Keep unchanged:", juce::dontSendNotification);
        lockLabel_.setFont(theme::font(true));
        addAndMakeVisible(lockLabel_);
        status_.setFont(theme::font());
        status_.setColour(juce::Label::textColourId, kDim);
        addAndMakeVisible(status_);
        status("Randomize or Mutate to make six children; click one to hear it (it plays in the Mutator's own "
               "variation, variations 1-8 stay as they are).");

        doc_.addChangeListener(this);
        refresh();
        setSize(760, 470);
    }

    ~Panel() override { doc_.removeChangeListener(this); }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(kPanel);
        g.setColour(kDim);
        g.setFont(theme::font(true));
        g.drawText("Children", childrenTitle_, juce::Justification::centredLeft);
        g.drawText("Gene bank", bankTitle_, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(12);
        auto ops = r.removeFromTop(30);
        for (auto* b : {&randomize_, &mutate_, &cross_, &interpolate_})
            b->setBounds(ops.removeFromLeft(110)), ops.removeFromLeft(6);
        r.removeFromTop(10);
        childrenTitle_ = r.removeFromTop(20);
        auto row = r.removeFromTop(56);
        const int w = row.getWidth() / 8;
        boxes_[mut::kMotherBox]->setBounds(row.removeFromLeft(w));
        boxes_[mut::kFatherBox]->setBounds(row.removeFromLeft(w));
        for (int i = 0; i < mut::kChildren; ++i)
            boxes_[i]->setBounds(row.removeFromLeft(w));
        r.removeFromTop(12);
        for (auto [s, l] : {std::pair{&probability_, &probabilityLabel_}, std::pair{&range_, &rangeLabel_},
                            std::pair{&crossover_, &crossoverLabel_}}) {
            auto line = r.removeFromTop(28);
            l->setBounds(line.removeFromLeft(100));
            if (s == &range_)
                link_.setBounds(line.removeFromRight(70));
            s->setBounds(line);
        }
        r.removeFromTop(8);
        auto lockRow = r.removeFromTop(26);
        lockLabel_.setBounds(lockRow.removeFromLeft(130));
        const int lw = lockRow.getWidth() / locks_.size();
        for (auto* t : locks_)
            t->setBounds(lockRow.removeFromLeft(lw));
        r.removeFromTop(10);
        bankTitle_ = r.removeFromTop(20);
        for (int rowIndex = 0; rowIndex < 3; ++rowIndex) {
            auto line = r.removeFromTop(34);
            rowClear_[rowIndex]->setBounds(line.removeFromRight(60).reduced(2));
            rowCopy_[rowIndex]->setBounds(line.removeFromRight(110).reduced(2));
            const int bw = line.getWidth() / 8;
            for (int i = 0; i < 8; ++i)
                bank_[rowIndex * 8 + i]->setBounds(line.removeFromLeft(bw));
        }
        status_.setBounds(r.removeFromBottom(24));
    }

    // The same keys while the Mutator's own window has the keyboard.
    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto c = key.getTextCharacter();
        const auto m = key.getModifiers();
        const auto k = mutatorKey(c > 0 && c < 128 ? static_cast<char>(c) : 0,
                                  m.isCommandDown() || m.isCtrlDown() || m.isAltDown() || m.isShiftDown(), false);
        if (k == MutatorKey::None)
            return false;
        keyAction(k);
        return true;
    }

    // CPatchView::KeyMuta*: as the buttons and box menus, with the original's conditions (Mutate needs a Mother,
    // Cross and Interpolate a Mother and a Father; O and T copy the current variation, E mutates it).
    void keyAction(MutatorKey k)
    {
        switch (k) {
        case MutatorKey::Mutate:
            if (mutator().hasData(mut::kMother) && mutator().mutate(doc_.patch(), mut::kMother))
                status("Six mutants of the Mother");
            break;
        case MutatorKey::Randomize:
            randomize_.triggerClick();
            return;
        case MutatorKey::Interpolate:
            if (mutator().hasData(mut::kMother) && mutator().hasData(mut::kFather))
                interpolate_.triggerClick();
            return;
        case MutatorKey::Cross:
            if (mutator().hasData(mut::kMother) && mutator().hasData(mut::kFather))
                cross_.triggerClick();
            return;
        case MutatorKey::StoreChild:
            if (mutator().storeFocused())
                status("Stored in the gene bank");
            break;
        case MutatorKey::Mother:
            if (mutator().focus != mut::kMother) {
                copyFromVariation(mut::kMother);
                status("Variation " + juce::String(doc_.variation() + 1) + " is the Mother");
            }
            break;
        case MutatorKey::Father:
            if (mutator().focus != mut::kFather) {
                copyFromVariation(mut::kFather);
                status("Variation " + juce::String(doc_.variation() + 1) + " is the Father");
            }
            break;
        case MutatorKey::MutateCurrent:
            if (mutator().mutate(doc_.patch(), currentVariation()))
                status("Six mutants of variation " + juce::String(doc_.variation() + 1));
            break;
        case MutatorKey::None: return;
        }
        refresh();
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        mutator().sync(doc_.patch());
        // Another patch's Mutator may be the shown one now: its own settings.
        probability_.setValue(mutator().settings.probability * 100.0, juce::dontSendNotification);
        range_.setValue(mutator().settings.range * 100.0, juce::dontSendNotification);
        crossover_.setValue(mutator().settings.crossover * 100.0, juce::dontSendNotification);
        refresh();
    }

    void refresh()
    {
        for (int i = 0; i < boxes_.size(); ++i) {
            const mut::Box box{mut::BoxKind::Population, i};
            boxes_[i]->filled = mutator().hasData(box);
            boxes_[i]->focused = mutator().focus == box;
            boxes_[i]->repaint();
        }
        for (int i = 0; i < bank_.size(); ++i) {
            const mut::Box box{mut::BoxKind::GeneBank, i};
            bank_[i]->filled = mutator().hasData(box);
            bank_[i]->focused = mutator().focus == box;
            bank_[i]->repaint();
        }
    }

    void status(const juce::String& s) { status_.setText(s, juce::dontSendNotification); }

    mut::Box currentVariation() const { return {mut::BoxKind::Variation, doc_.variation()}; }

    void copyFromVariation(mut::Box to)
    {
        g2::Patch scratch = doc_.patch();
        mutator().copy(scratch, currentVariation(), to);
    }

    // Plays an individual as the original does: in the patch's hidden
    // variation 9, which becomes the focused one (on the synth too); the
    // user's variations 1-8 are untouched until it is copied into one.
    // Undoable, as the original's focus molecules.
    void audition(mut::Box box)
    {
        if (!mutator().hasData(box))
            return;
        doc_.perform("Audition mutation", [&](g2::Patch& p) {
            const g2::Patch before = p;
            if (mutator().audition(p, box)) {
                // CMutaSynthData::GetFocusIndividMolecules @00142ef8: the
                // individual in variation 9 (4D x3, 65), then 6A 9.
                constexpr auto v = static_cast<std::uint8_t>(g2::kAuditionVariation);
                p.header.activeVariation = v;
                g2::live::recordVariationDump(before, p, v);
                g2::live::record(g2::live::VariationSelect{v}, g2::live::VariationSelect{before.header.activeVariation});
            }
        });
        status("Playing it in the Mutator's variation (right-click > Copy to Variation keeps it; "
               "click a variation button to go back)");
        refresh();
    }

    void boxMenu(mut::Box box)
    {
        enum { kMother = 1, kFather, kStore, kFromVariation, kClear, kToVariation = 100 };
        juce::PopupMenu toVariation;
        for (int v = 0; v < g2::kUserVariations; ++v)
            toVariation.addItem(kToVariation + v, "Variation " + juce::String(v + 1));
        juce::PopupMenu m;
        const bool has = mutator().hasData(box);
        m.addItem(kMother, "Use as Mother", has && box != mut::kMother);
        m.addItem(kFather, "Use as Father", has && box != mut::kFather);
        m.addItem(kStore, "Store in Gene Bank", has && box.kind == mut::BoxKind::Population);
        m.addSubMenu("Copy to Variation", toVariation, has);
        m.addSeparator();
        m.addItem(kFromVariation, "Fill from Variation " + juce::String(doc_.variation() + 1),
                  box.kind != mut::BoxKind::Population || box == mut::kMother || box == mut::kFather);
        m.addItem(kClear, "Clear", has && (box.kind == mut::BoxKind::GeneBank || box == mut::kMother || box == mut::kFather));
        m.showMenuAsync({}, [safe = juce::Component::SafePointer<Panel>(this), box](int r) {
            if (!safe || r <= 0)
                return;
            auto& self = *safe;
            g2::Patch scratch = self.doc_.patch();
            if (r == kMother)
                self.mutator().copy(scratch, box, mut::kMother);
            else if (r == kFather)
                self.mutator().copy(scratch, box, mut::kFather);
            else if (r == kStore) {
                for (int i = 0; i < mut::kGeneBankSize; ++i)
                    if (!self.mutator().hasData({mut::BoxKind::GeneBank, i})) {
                        self.mutator().copy(scratch, box, {mut::BoxKind::GeneBank, i});
                        break;
                    }
            } else if (r == kFromVariation)
                self.mutator().copy(scratch, self.currentVariation(), box);
            else if (r == kClear)
                self.mutator().clear(box);
            else if (r >= kToVariation) {
                const mut::Box target{mut::BoxKind::Variation, r - kToVariation};
                // GetCopyIndividMolecules @00144528: the variation's dump, then 6A
                // (the original then shows that variation).
                const auto v = static_cast<std::uint8_t>(target.index);
                self.doc_.perform("Copy to variation", [&](g2::Patch& p) {
                    const g2::Patch before = p;
                    self.mutator().copy(p, box, target);
                    p.header.activeVariation = v;
                    g2::live::recordVariationDump(before, p, v);
                    g2::live::record(g2::live::VariationSelect{v}, g2::live::VariationSelect{before.header.activeVariation});
                });
                self.doc_.setVariation(v);
            }
            self.refresh();
        });
    }

    PatchDocument& doc_;
    // One Mutator per patch (MutatorSet.h): the shown slot's, fresh after a load.
    MutatorSet mutators_;
    std::uint32_t seed_ = 1;
    mut::Mutator& mutator() { return mutators_.get(doc_.isPerformance() ? doc_.slot() : 0, doc_.generation(), doc_.patch(), seed_); }
    juce::TextButton randomize_{"Randomize"}, mutate_{"Mutate"}, cross_{"Cross"}, interpolate_{"Interpolate"};
    juce::OwnedArray<BoxButton> boxes_, bank_;
    juce::OwnedArray<juce::TextButton> rowCopy_, rowClear_;
    juce::Slider probability_, range_, crossover_;
    juce::Label probabilityLabel_, rangeLabel_, crossoverLabel_, lockLabel_, status_;
    juce::ToggleButton link_{"Link"};
    juce::OwnedArray<juce::ToggleButton> locks_;
    juce::Rectangle<int> childrenTitle_, bankTitle_;
};

MutatorWindow::MutatorWindow(PatchDocument& doc)
    : DocumentWindow("Patch Mutator", juce::Colour(0xff2a2c31), DocumentWindow::closeButton),
      panel_(std::make_unique<Panel>(doc))
{
    setUsingNativeTitleBar(true);
    setContentNonOwned(panel_.get(), true);
    setAlwaysOnTop(true);
    centreWithSize(getWidth(), getHeight());
}

void MutatorWindow::keyAction(MutatorKey k)
{
    panel_->keyAction(k);
}

MutatorWindow::~MutatorWindow()
{
    clearContentComponent();
}

} // namespace g2ui
