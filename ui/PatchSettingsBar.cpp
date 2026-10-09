#include "PatchSettingsBar.h"

#include "ModulePainter.h"
#include "Skin.h"

#include <cstdlib>
#include "g2/param_text.hpp"

namespace g2ui {

namespace {
const juce::Colour kInk(0xffe6e8ec), kDim(0xff9aa0aa), kAccent(0xff3478d4), kPanel(0xff2a2c31);
namespace edit = g2::edit;
using edit::Setting;
} // namespace

class PatchSettingsBar::Control : public juce::Component {
public:
    // Knob: rotary; Menu: drop-down; Toggle: on/off button; Bar: horizontal
    // value bar with a colour chip and name; Cycle: button stepping through
    // the values on each click.
    enum class Kind { Knob, Menu, Toggle, Bar, Cycle };

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
        if (kind == Kind::Bar) {
            paintBar(g, r);
            return;
        }
        if (kind == Kind::Cycle) {
            const int v = get();
            const auto box = r.reduced(1.0f, 1.0f);
            const auto tint = colour ? colour() : kAccent;
            g.setColour(v != 0 ? tint.withAlpha(0.85f) : juce::Colour(0xff3a3d44));
            g.fillRoundedRectangle(box, 4.0f);
            g.setColour(hovered_ ? juce::Colour(0xffff8c1a) : juce::Colour(0xff555a64));
            g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
            g.setColour(v != 0 ? juce::Colour(0xff1d1f24) : kInk);
            g.setFont(juce::FontOptions(10.5f));
            g.drawFittedText(text(v), box.toNearestInt(), juce::Justification::centred, 1, 0.7f);
            return;
        }
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

    void mouseDown(const juce::MouseEvent& e) override
    {
        startValue_ = get();
        if (kind == Kind::Bar && e.y >= kBarTop)
            setFromBar(e.x, true);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (kind == Kind::Bar) {
            if (e.getMouseDownY() >= kBarTop)
                setFromBar(e.x, true);
            if (status)
                status(name + ": " + text(get()));
            return;
        }
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
        if (kind == Kind::Knob || kind == Kind::Bar || e.getDistanceFromDragStart() > 4)
            return;
        if (kind == Kind::Toggle) {
            set(get() ? 0 : 1, false);
            return;
        }
        if (kind == Kind::Cycle) {
            set(get() >= max() ? 0 : get() + 1, false);
            if (status)
                status(name + ": " + text(get()));
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
        if ((kind != Kind::Knob && kind != Kind::Bar) || w.deltaY == 0.0f)
            return;
        set(juce::jlimit(0, max(), get() + (w.deltaY > 0 ? 1 : -1)), true);
    }

private:
    static constexpr int kBarTop = 15; // the bar starts below the name row

    juce::Rectangle<float> barArea() const
    {
        return getLocalBounds().toFloat().withTrimmedTop(static_cast<float>(kBarTop)).withHeight(14.0f).reduced(2.0f, 0.0f);
    }

    void setFromBar(int x, bool coalesce)
    {
        const auto bar = barArea();
        const float t = juce::jlimit(0.0f, 1.0f, (static_cast<float>(x) - bar.getX()) / bar.getWidth());
        set(juce::roundToInt(t * static_cast<float>(max())), coalesce);
    }

    void paintBar(juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto tint = colour ? colour() : kAccent;
        // Name row: colour chip and name.
        auto row = r.removeFromTop(static_cast<float>(kBarTop)).reduced(2.0f, 2.0f);
        g.setColour(tint);
        g.fillRoundedRectangle(row.removeFromLeft(8.0f).withSizeKeepingCentre(8.0f, 8.0f), 2.0f);
        row.removeFromLeft(4.0f);
        g.setColour(kInk);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawFittedText(name, row.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        // Value bar.
        const auto bar = barArea();
        const float t = static_cast<float>(get()) / static_cast<float>(std::max(1, max()));
        g.setColour(juce::Colour(0xff3a3d44));
        g.fillRoundedRectangle(bar, 4.0f);
        g.setColour(tint);
        g.fillRoundedRectangle(bar.withWidth(std::max(8.0f, bar.getWidth() * t)), 4.0f);
        g.setColour(hovered_ ? juce::Colour(0xffff8c1a) : juce::Colour(0xff555a64));
        g.drawRoundedRectangle(bar.reduced(0.5f), 4.0f, 1.0f);
        g.setColour(t > 0.55f ? juce::Colour(0xff1d1f24) : kInk);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText(text(get()), bar.reduced(5.0f, 0.0f).toNearestInt(), juce::Justification::centredRight, false);
    }

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

    // Morph groups: one card per group, a value bar named after the group
    // (double-click the name to rename) and its source (Knob or a controller).
    auto& morph = groups_.emplace_back(Group{"Morph", {}});
    for (int i = 0; i < 8; ++i) {
        auto& bar = setting(morph, Setting::Morph, static_cast<std::uint8_t>(i), Control::Kind::Bar, {});
        bar.colour = [i] { return ModulePainter::morphColour(i); };
        bar.onRename = [this, i] {
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
    for (int i = 0; i < 8; ++i) {
        auto& source = setting(morph, Setting::Morph, static_cast<std::uint8_t>(8 + i), Control::Kind::Cycle, {});
        source.colour = [i] { return ModulePainter::morphColour(i); };
    }

    collapsed_ = userSettings().getBoolValue("settingsCollapsed", false);
    if (const char* env = std::getenv("G2_SETTINGS_COLLAPSED")) // for g2render snapshots
        collapsed_ = juce::String(env) == "1";
    for (auto* c : controls_)
        c->setVisible(!collapsed_);

    doc_.addChangeListener(this);
    changeListenerCallback(nullptr);
}

PatchSettingsBar::~PatchSettingsBar()
{
    doc_.removeChangeListener(this);
}

void PatchSettingsBar::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // Morph bars are named after their group label ("Wheel", "Vel", ...).
    auto& morph = groups_.back();
    for (int i = 0; i < 8; ++i) {
        const auto label = juce::String(edit::morphLabel(doc_.patch(), i));
        morph.controls[static_cast<std::size_t>(i)]->name = label;
        morph.controls[static_cast<std::size_t>(8 + i)]->name = label + " source";
    }
    repaint();
    for (auto* c : controls_)
        c->repaint();
}

juce::String PatchSettingsBar::summary() const
{
    const auto& p = doc_.patch();
    const auto v = static_cast<std::uint8_t>(doc_.variation());
    auto on = [&](Setting s, std::uint8_t param) { return edit::settingValue(p, s, param, v) != 0; };
    juce::StringArray parts;
    const auto voices = juce::String(edit::voicesText(p));
    parts.add(voices.containsOnly("0123456789") ? voices + (voices == "1" ? " voice" : " voices") : voices);
    parts.add(edit::categoryName(p.header.category));
    parts.add("Level " + juce::String(edit::settingText(p, Setting::Gain, 0, v)));
    parts.add("Glide " + juce::String(edit::settingText(p, Setting::Glide, 0, v)));
    parts.add("Bend " + juce::String(on(Setting::Bend, 0) ? edit::settingText(p, Setting::Bend, 1, v) : "Off"));
    parts.add("Vibrato " + juce::String(edit::settingText(p, Setting::Vibrato, 0, v)));
    parts.add("Arp " + juce::String(on(Setting::Arpeggiator, 0) ? "On" : "Off"));
    parts.add("Octave " + juce::String(edit::settingText(p, Setting::Misc, 0, v)));
    return parts.joinIntoString("  \u00b7  ");
}

void PatchSettingsBar::setCollapsed(bool collapsed)
{
    collapsed_ = collapsed;
    userSettings().setValue("settingsCollapsed", collapsed);
    for (auto* c : controls_)
        c->setVisible(!collapsed);
    if (onLayoutChanged)
        onLayoutChanged();
    repaint();
}

void PatchSettingsBar::mouseDown(const juce::MouseEvent& e)
{
    if (e.y < kHeaderHeight)
        setCollapsed(!collapsed_);
}

void PatchSettingsBar::paint(juce::Graphics& g)
{
    g.fillAll(kPanel);
    // Header: chevron, title, and the summary when collapsed.
    auto header = getLocalBounds().removeFromTop(kHeaderHeight);
    {
        juce::Path chevron;
        const float cx = 12.0f, cy = static_cast<float>(header.getCentreY());
        if (collapsed_)
            chevron.addTriangle(cx - 2.5f, cy - 4.0f, cx - 2.5f, cy + 4.0f, cx + 3.0f, cy);
        else
            chevron.addTriangle(cx - 4.0f, cy - 2.5f, cx + 4.0f, cy - 2.5f, cx, cy + 3.0f);
        g.setColour(kDim);
        g.fillPath(chevron);
    }
    g.setColour(kInk);
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText("PATCH SETTINGS", header.withTrimmedLeft(22).withWidth(110), juce::Justification::centredLeft);
    if (collapsed_) {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(summary(), header.withTrimmedLeft(140).reduced(4, 0), juce::Justification::centredLeft, true);
        return;
    }
    g.setColour(kDim.withAlpha(0.7f));
    g.setFont(juce::FontOptions(10.5f));
    g.drawText("variation " + juce::String(doc_.variation() + 1), header.withTrimmedLeft(140).reduced(4, 0),
               juce::Justification::centredLeft, true);

    g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
    for (const auto& group : groups_) {
        if (group.controls.empty())
            continue;
        juce::Rectangle<int> area = group.controls.front()->getBounds();
        for (const auto* c : group.controls)
            area = area.getUnion(c->getBounds());
        const auto frame = area.withTop(area.getY() - 12).withBottom(area.getY() - 12 + kRowHeight - 2)
                               .expanded(4, 0).toFloat();
        g.setColour(juce::Colour(0xff32353b));
        g.fillRoundedRectangle(frame, 5.0f);
        g.setColour(kDim.withAlpha(0.8f));
        g.drawText(group.title.toUpperCase(), frame.withHeight(12.0f).translated(5.0f, 1.0f).toNearestInt(),
                   juce::Justification::centredLeft, false);
    }
}

int PatchSettingsBar::settingsWidth() const
{
    int x = 10;
    for (const auto& group : groups_) {
        if (&group == &groups_.back())
            break;
        for (const auto* c : group.controls)
            x += c->preferredWidth();
        x += 12;
    }
    return x;
}

int PatchSettingsBar::preferredHeight(int width) const
{
    if (collapsed_)
        return kHeaderHeight + 2;
    const bool wrap = (width - settingsWidth() - 10) / 8 < kMinCard;
    return kHeaderHeight + (wrap ? 2 : 1) * kRowHeight + 4;
}

void PatchSettingsBar::resized()
{
    if (collapsed_)
        return;
    const bool wrap = (getWidth() - settingsWidth() - 10) / 8 < kMinCard;
    int x = 10;
    int top = kHeaderHeight + 13;
    const int height = kRowHeight - 17;
    for (auto& group : groups_) {
        if (&group == &groups_.back()) {
            // Morph cards: bar (with name) above, source button below.
            if (wrap) {
                x = 10;
                top += kRowHeight;
            }
            const int w = juce::jlimit(kMinCard, kMaxCard, (getWidth() - x - 14) / 8);
            for (int i = 0; i < 8; ++i) {
                group.controls[static_cast<std::size_t>(i)]->setBounds(x + i * w, top, w - 4, 31);
                group.controls[static_cast<std::size_t>(8 + i)]->setBounds(x + i * w + 2, top + 33, w - 8, 18);
            }
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
