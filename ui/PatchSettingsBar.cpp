#include "PatchSettingsBar.h"

#include "ModulePainter.h"
#include "g2/param_text.hpp"

namespace g2ui {

namespace {
const juce::Colour kInk(0xffe6e8ec), kDim(0xff9aa0aa), kAccent(0xff3478d4), kPanel(0xff2a2c31);
namespace edit = g2::edit;
using edit::Setting;
} // namespace

class PatchSettingsBar::Control : public juce::Component {
public:
    enum class Kind { Knob, Menu, Toggle };

    Kind kind = Kind::Menu;
    juce::String name;
    std::function<int()> get;
    std::function<int()> max;
    std::function<juce::String(int)> text; // text of any value
    std::function<void(int value, bool coalesce)> set;
    std::function<void()> onRename;        // optional: double-click the name
    std::function<void(const juce::String&)> status;
    std::function<juce::Colour()> colour;  // optional knob colour

    int preferredWidth() const { return kind == Kind::Knob ? 46 : 58; }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour(kDim);
        g.setFont(juce::FontOptions(10.5f));
        g.drawFittedText(name, r.removeFromTop(13.0f).toNearestInt(), juce::Justification::centred, 1, 0.8f);
        const int v = get(), m = std::max(1, max());
        if (kind == Kind::Knob) {
            auto area = r.removeFromTop(r.getHeight() - 12.0f).withSizeKeepingCentre(24.0f, 24.0f);
            const auto c = area.getCentre();
            const float radius = 11.0f, start = juce::degreesToRadians(-135.0f);
            const float angle = start + juce::degreesToRadians(270.0f) * static_cast<float>(v) / static_cast<float>(m);
            juce::Path track, arc;
            track.addCentredArc(c.x, c.y, radius, radius, 0.0f, start, -start, true);
            arc.addCentredArc(c.x, c.y, radius, radius, 0.0f, start, angle, true);
            g.setColour(juce::Colour(0xff4a4e57));
            g.strokePath(track, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(hovered_ ? juce::Colour(0xffff8c1a) : colour ? colour() : kAccent);
            g.strokePath(arc, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour(juce::Colour(0xffd9dce2));
            g.fillEllipse(c.x - 7.0f, c.y - 7.0f, 14.0f, 14.0f);
            g.setColour(juce::Colour(0xff2b2d33));
            g.drawLine(c.x + 2.0f * std::sin(angle), c.y - 2.0f * std::cos(angle), c.x + 6.0f * std::sin(angle),
                       c.y - 6.0f * std::cos(angle), 1.8f);
            g.setColour(kInk);
            g.setFont(juce::FontOptions(10.5f));
            g.drawFittedText(text(v), r.toNearestInt(), juce::Justification::centred, 1, 0.75f);
            return;
        }
        auto box = r.reduced(2.0f, 3.0f).withHeight(18.0f).withY(r.getY() + 4.0f);
        const bool on = kind == Kind::Toggle && v != 0;
        g.setColour(on ? kAccent : juce::Colour(0xff3a3d44));
        g.fillRoundedRectangle(box, 4.0f);
        g.setColour(hovered_ ? juce::Colour(0xffff8c1a) : juce::Colour(0xff555a64));
        g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
        g.setColour(on ? juce::Colours::white : kInk);
        g.setFont(juce::FontOptions(11.0f));
        if (kind == Kind::Menu) {
            g.drawFittedText(text(v), box.withTrimmedRight(9.0f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
            juce::Path p;
            const float x = box.getRight() - 7.0f, y = box.getCentreY();
            p.addTriangle(x - 3.0f, y - 1.5f, x + 3.0f, y - 1.5f, x, y + 2.0f);
            g.fillPath(p);
        } else {
            g.drawFittedText(text(v), box.toNearestInt(), juce::Justification::centred, 1, 0.7f);
        }
    }

    void mouseEnter(const juce::MouseEvent&) override
    {
        hovered_ = true;
        repaint();
        if (status)
            status(name + ": " + text(get()) + (onRename ? "   (double-click the name to rename)" : ""));
    }
    void mouseExit(const juce::MouseEvent&) override
    {
        hovered_ = false;
        repaint();
    }

    void mouseDown(const juce::MouseEvent&) override { startValue_ = get(); }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (kind != Kind::Knob)
            return;
        const int m = std::max(1, max());
        const int perStep = std::max(1, 200 / (m + 1)) * (e.mods.isShiftDown() ? 4 : 1);
        set(juce::jlimit(0, m, startValue_ + (e.getMouseDownY() - e.y) / perStep), true);
        if (status)
            status(name + ": " + text(get()));
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (kind == Kind::Knob || e.getDistanceFromDragStart() > 4)
            return;
        if (kind == Kind::Toggle) {
            set(get() ? 0 : 1, false);
            return;
        }
        juce::PopupMenu menu;
        const int current = get();
        for (int i = 0; i <= max(); ++i)
            menu.addItem(i + 1, text(i), true, i == current);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                           [safe = juce::Component::SafePointer<Control>(this)](int r) {
                               if (safe && r > 0)
                                   safe->set(r - 1, false);
                           });
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (onRename && e.y < 14)
            onRename();
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (kind != Kind::Knob || w.deltaY == 0.0f)
            return;
        set(juce::jlimit(0, max(), get() + (w.deltaY > 0 ? 1 : -1)), true);
    }

private:
    bool hovered_ = false;
    int startValue_ = 0;
};

PatchSettingsBar::Control& PatchSettingsBar::add(Group& group, std::unique_ptr<Control> c)
{
    c->status = [this](const juce::String& s) {
        if (onStatus)
            onStatus(s);
    };
    addAndMakeVisible(*c);
    group.controls.push_back(c.get());
    return *controls_.add(c.release());
}

PatchSettingsBar::PatchSettingsBar(PatchDocument& doc) : doc_(doc)
{
    // A control bound to a patch setting: its value in the current variation.
    auto setting = [this](Group& group, Setting s, std::uint8_t param, Control::Kind kind,
                          const juce::String& name) -> Control& {
        auto c = std::make_unique<Control>();
        c->kind = kind;
        c->name = name;
        const auto* def = edit::settingDef(s);
        const std::uint8_t textFunc = def ? def->params[param].textFunc : 0;
        c->get = [this, s, param] {
            return static_cast<int>(edit::settingValue(doc_.patch(), s, param, static_cast<std::uint8_t>(doc_.variation())));
        };
        c->max = [def, param] { return def ? static_cast<int>(def->params[param].max) : 127; };
        c->text = [textFunc](int v) { return juce::String(g2::paramtext::formatSingle(textFunc, static_cast<std::uint8_t>(v))); };
        c->set = [this, s, param, name](int v, bool coalesce) {
            const auto variation = static_cast<std::uint8_t>(doc_.variation());
            auto edit = [=](g2::Patch& p) { edit::setSetting(p, s, param, variation, static_cast<std::uint8_t>(v)); };
            if (coalesce)
                doc_.performCoalesced("setting:" + name, edit);
            else
                doc_.perform("Change " + name, edit);
        };
        return add(group, std::move(c));
    };

    groups_.reserve(9);
    // Voices and category (patch header).
    auto& patchGroup = groups_.emplace_back(Group{"Patch", {}});
    {
        auto c = std::make_unique<Control>();
        c->name = "Voices";
        // Menu entries: 0 = Legato, 1 = Mono, 2.. = 1..32 voices.
        c->get = [this] {
            const auto& h = doc_.patch().header;
            return h.monoMode == 2 ? 0 : h.monoMode == 1 ? 1 : 1 + h.voiceCount;
        };
        c->max = [] { return 33; };
        c->text = [](int v) { return v == 0 ? juce::String("Legato") : v == 1 ? juce::String("Mono") : juce::String(v - 1); };
        c->set = [this](int v, bool) {
            doc_.perform("Change voices", [v](g2::Patch& p) {
                if (v == 0)
                    edit::setVoices(p, edit::VoiceMode::Legato);
                else if (v == 1)
                    edit::setVoices(p, edit::VoiceMode::Mono);
                else
                    edit::setVoices(p, edit::VoiceMode::Poly, static_cast<std::uint8_t>(v - 1));
            });
        };
        add(patchGroup, std::move(c));
    }
    {
        auto c = std::make_unique<Control>();
        c->name = "Category";
        c->get = [this] { return static_cast<int>(doc_.patch().header.category); };
        c->max = [] { return 15; };
        c->text = [](int v) { return juce::String(edit::categoryName(static_cast<std::uint8_t>(v))); };
        c->set = [this](int v, bool) {
            doc_.perform("Change category", [v](g2::Patch& p) { edit::setCategory(p, static_cast<std::uint8_t>(v)); });
        };
        add(patchGroup, std::move(c));
    }

    auto& volume = groups_.emplace_back(Group{"Volume", {}});
    setting(volume, Setting::Gain, 0, Control::Kind::Knob, "Level");
    setting(volume, Setting::Gain, 1, Control::Kind::Toggle, "Active");

    auto& glide = groups_.emplace_back(Group{"Glide", {}});
    setting(glide, Setting::Glide, 0, Control::Kind::Menu, "Mode");
    setting(glide, Setting::Glide, 1, Control::Kind::Knob, "Time");

    auto& bend = groups_.emplace_back(Group{"Bend", {}});
    setting(bend, Setting::Bend, 0, Control::Kind::Toggle, "On");
    setting(bend, Setting::Bend, 1, Control::Kind::Menu, "Range");

    auto& vibrato = groups_.emplace_back(Group{"Vibrato", {}});
    setting(vibrato, Setting::Vibrato, 0, Control::Kind::Menu, "Source");
    setting(vibrato, Setting::Vibrato, 1, Control::Kind::Knob, "Depth");
    setting(vibrato, Setting::Vibrato, 2, Control::Kind::Knob, "Rate");

    auto& arp = groups_.emplace_back(Group{"Arpeggiator", {}});
    setting(arp, Setting::Arpeggiator, 0, Control::Kind::Toggle, "Run");
    setting(arp, Setting::Arpeggiator, 1, Control::Kind::Menu, "Rate");
    setting(arp, Setting::Arpeggiator, 2, Control::Kind::Menu, "Dir");
    setting(arp, Setting::Arpeggiator, 3, Control::Kind::Menu, "Range");

    auto& keyboard = groups_.emplace_back(Group{"Keyboard", {}});
    setting(keyboard, Setting::Misc, 0, Control::Kind::Menu, "Octave");
    setting(keyboard, Setting::Misc, 1, Control::Kind::Toggle, "Sustain");

    // Morph groups: dial value (knob, named after the group) and source.
    auto& morph = groups_.emplace_back(Group{"Morph", {}});
    for (int i = 0; i < 8; ++i) {
        auto& knob = setting(morph, Setting::Morph, static_cast<std::uint8_t>(i), Control::Kind::Knob, {});
        knob.colour = [i] { return ModulePainter::morphColour(i); };
        knob.onRename = [this, i] {
            auto* w = new juce::AlertWindow("Rename morph group", "Name (up to 7 characters):", juce::MessageBoxIconType::NoIcon);
            w->addTextEditor("name", juce::String(edit::morphLabel(doc_.patch(), i)));
            w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            w->enterModalState(true, juce::ModalCallbackFunction::create([this, i, w](int r) {
                if (r != 1)
                    return;
                const auto label = w->getTextEditorContents("name").substring(0, 7).toStdString();
                doc_.perform("Rename morph group", [=](g2::Patch& p) { edit::setMorphLabel(p, i, label); });
            }), true);
        };
    }
    for (int i = 0; i < 8; ++i)
        setting(morph, Setting::Morph, static_cast<std::uint8_t>(8 + i), Control::Kind::Menu, {});

    doc_.addChangeListener(this);
    changeListenerCallback(nullptr);
}

PatchSettingsBar::~PatchSettingsBar()
{
    doc_.removeChangeListener(this);
}

void PatchSettingsBar::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // Morph knobs are named after their group label ("Wheel", "Vel", ...).
    auto& morph = groups_.back();
    for (int i = 0; i < 8; ++i) {
        morph.controls[static_cast<std::size_t>(i)]->name = edit::morphLabel(doc_.patch(), i);
        morph.controls[static_cast<std::size_t>(8 + i)]->name = {}; // the knob above names the group
    }
    repaint();
    for (auto* c : controls_)
        c->repaint();
}

void PatchSettingsBar::paint(juce::Graphics& g)
{
    g.fillAll(kPanel);
    g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
    for (const auto& group : groups_) {
        if (group.controls.empty())
            continue;
        auto first = group.controls.front()->getBounds();
        auto last = group.controls.back()->getBounds();
        if (&group == &groups_.back()) // morph: two rows
            last = group.controls[7]->getBounds();
        const auto frame = first.getUnion(last).withTop(2).withBottom(getHeight() - 2).expanded(4, 0).toFloat();
        g.setColour(juce::Colour(0xff32353b));
        g.fillRoundedRectangle(frame, 5.0f);
        g.setColour(kDim.withAlpha(0.8f));
        g.drawText(group.title.toUpperCase(), frame.withHeight(12.0f).translated(5.0f, 0.0f).toNearestInt(),
                   juce::Justification::centredLeft, false);
    }
}

void PatchSettingsBar::resized()
{
    int x = 8;
    const int top = 12, height = getHeight() - top - 2;
    for (auto& group : groups_) {
        const bool isMorph = &group == &groups_.back();
        if (isMorph) {
            // Knobs on top, source menus below, one column per group.
            const int w = 46;
            for (int i = 0; i < 8; ++i) {
                group.controls[static_cast<std::size_t>(i)]->setBounds(x + i * w, top, w, height * 3 / 5);
                group.controls[static_cast<std::size_t>(8 + i)]->setBounds(x + i * w, top + height * 3 / 5 - 12, w,
                                                                           height - height * 3 / 5 + 12);
            }
            x += 8 * w + 12;
            continue;
        }
        for (auto* c : group.controls) {
            c->setBounds(x, top, c->preferredWidth(), height);
            x += c->preferredWidth();
        }
        x += 12;
    }
}

} // namespace g2ui
