// g2render: draws modules or a whole patch to a PNG with the editor's own
// painter, for checking the rendering without opening the app.
//
//   g2render modules <out.png> [typeId...]   (all selectable modules if none given)
//   g2render patch <file.pch2> <out.png>
//   g2render ui <file.pch2> <out.png>        (the whole editor window)
//
// Set G2_LOOK=classic to draw with the original bitmaps instead of the modern look,
// and G2_ZOOM=<factor> to render the editor window zoomed.
#include "LiveView.h"
#include "MainView.h"
#include "MutatorWindow.h"
#include "ModulePainter.h"

#include "g2/edit.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdlib>
#include <iostream>

using namespace g2ui;

namespace {

void writePng(const juce::Image& img, const juce::File& out)
{
    out.deleteFile();
    juce::FileOutputStream stream(out);
    juce::PNGImageFormat().writeImageToStream(img, stream);
}

void drawArea(juce::Graphics& g, const g2::Patch& p, g2::Location loc, int originY)
{
    for (const auto& m : p.area(loc).modules) {
        const auto b = ModulePainter::moduleBounds(m).translated(0, originY);
        juce::Graphics::ScopedSaveState state(g);
        g.setOrigin(b.getPosition());
        g.reduceClipRegion(b.withZeroOrigin());
        ModulePainter::paint(g, {p, loc, m, 0, ModulePainter::panelFor(m), currentLook()});
    }
}

int renderModules(const juce::File& out, const juce::StringArray& types)
{
    g2::Patch p = g2::Patch::makeDefault();
    int col = 0, row = 0, colHeight = 0;
    auto add = [&](std::uint8_t type) {
        const auto* def = g2::db::find(type);
        if (!def || def->kind != g2::db::ModuleKind::Module)
            return;
        if (row + def->height > 60) {
            ++col;
            row = 0;
        }
        g2::edit::addModule(p, g2::Location::Va, type, static_cast<std::uint8_t>(col), static_cast<std::uint8_t>(row));
        row += def->height;
        colHeight = std::max(colHeight, row);
    };
    if (types.isEmpty())
        for (const auto& d : g2::db::modules()) {
            if (d.selectable)
                add(d.typeId);
        }
    else
        for (const auto& t : types)
            add(static_cast<std::uint8_t>(t.getIntValue()));
    juce::Image img(juce::Image::ARGB, (col + 1) * kModuleWidth, std::max(1, colHeight) * kRowHeight, true);
    juce::Graphics g(img);
    g.fillAll(juce::Colour(0xff4a4a4a));
    drawArea(g, p, g2::Location::Va, 0);
    writePng(img, out);
    std::cout << "rendered " << p.va.modules.size() << " modules to " << out.getFullPathName() << "\n";
    return 0;
}

int renderPatch(const juce::File& in, const juce::File& out)
{
    juce::MemoryBlock mb;
    if (!in.loadFileAsData(mb)) {
        std::cerr << "cannot read " << in.getFullPathName() << "\n";
        return 1;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(mb.getData());
    const g2::Patch p = g2::loadPatch(std::vector<std::uint8_t>(bytes, bytes + mb.getSize()));
    int width = kModuleWidth, vaHeight = kRowHeight, fxHeight = kRowHeight;
    for (auto loc : {g2::Location::Va, g2::Location::Fx})
        for (const auto& m : p.area(loc).modules) {
            const auto b = ModulePainter::moduleBounds(m);
            width = std::max(width, b.getRight());
            (loc == g2::Location::Va ? vaHeight : fxHeight) = std::max(loc == g2::Location::Va ? vaHeight : fxHeight, b.getBottom());
        }
    const int gap = 20;
    juce::Image img(juce::Image::ARGB, width, vaHeight + gap + fxHeight, true);
    juce::Graphics g(img);
    g.fillAll(juce::Colour(0xff4a4a4a));
    g.setColour(juce::Colour(0xff202020));
    g.fillRect(0, vaHeight, width, gap);
    drawArea(g, p, g2::Location::Va, 0);
    drawArea(g, p, g2::Location::Fx, vaHeight + gap);
    writePng(img, out);
    std::cout << "rendered " << p.va.modules.size() << " VA + " << p.fx.modules.size() << " FX modules\n";
    return 0;
}

int renderTooltip(int type, const juce::File& out)
{
    const auto* def = g2::db::find(static_cast<std::uint8_t>(type));
    if (!def)
        return 1;
    theme::LookAndFeel lnf;
    const auto text = moduleTooltip(*def);
    const auto b = lnf.getTooltipBounds(text, {0, 0}, {0, 0, 2000, 2000});
    juce::Image img(juce::Image::ARGB, b.getWidth(), b.getHeight(), true);
    juce::Graphics g(img);
    lnf.drawTooltip(g, text, b.getWidth(), b.getHeight());
    writePng(img, out);
    std::cout << text << "\n";
    return 0;
}

int renderMutator(const juce::File& out)
{
    PatchDocument doc;
    MutatorWindow w(doc);
    auto* content = w.getContentComponent();
    writePng(content->createComponentSnapshot(content->getLocalBounds(), true, 1.0f), out);
    return 0;
}

int renderUi(const juce::File& in, const juce::File& out)
{
    PatchDocument doc;
    if (in != juce::File()) {
        juce::MemoryBlock mb;
        in.loadFileAsData(mb);
        const auto* bytes = static_cast<const std::uint8_t*>(mb.getData());
        doc.loadBytes(std::vector<std::uint8_t>(bytes, bytes + mb.getSize()));
        doc.setFile(in);
    }
    MainView view(doc, false);
    view.setSize(1400, 860);
    if (const char* z = std::getenv("G2_ZOOM"))
        view.setZoom(juce::String(z).getFloatValue(), nullptr, std::nullopt, false);
    const auto img = view.createComponentSnapshot(view.getLocalBounds(), true, 1.0f);
    writePng(img, out);
    std::cout << "rendered the editor window\n";
    return 0;
}

// The Live view with a made-up panel state (the G2 OS draws the real one), to review the drawing.
int renderLive(const juce::File& out, bool live)
{
    struct DemoHost : g2ui::EmulatorHost {
        bool on = true;
        bool emulatorAvailable() const override { return true; }
        bool emulatorRunning() const override { return on; }
        g2ui::PanelSnapshot panel() const override
        {
            g2ui::PanelSnapshot s;
            s.live = on;
            if (!on)
                return s;
            s.generation = 1;
            s.displays[0] = {16, 2, {"A:Drone      Pa", "Bank 1  Patch 3 "}};
            const char* const text[4][2] = {{"OscA1-----------", "Coarse   Fine   "}, {"FltLP1----------", "Freq     Res    "},
                                            {"EnvADSR1--------", "Attack   Decay  "}, {"Mix2-1A1--------", "Lev1     Lev2   "}};
            for (int i = 0; i < 4; ++i)
                s.displays[static_cast<std::size_t>(i + 1)] = {16, 2, {text[i][0], text[i][1]}};
            using L = g2ui::PanelLed;
            for (auto l : {L::Patch, L::SlotA, L::FocusA, L::Octave3, L::Var1, L::PageA, L::Column1, L::Midi, L::ModWheel})
                s.leds[static_cast<std::size_t>(l)] = 1.0f;
            for (int k = 0; k < g2ui::kPanelKnobs; ++k) {
                s.rings[static_cast<std::size_t>(k)].assign(25, 0.0f);
                for (int i = 0; i <= 3 + k * 2 && i < 25; ++i)
                    s.rings[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)] = 1.0f;
            }
            return s;
        }
    } host;
    host.on = live;
    g2ui::LiveView view(&host, {});
    view.setSize(1400, 700);
    view.setHost(&host); // takes the snapshot
    writePng(view.createComponentSnapshot(view.getLocalBounds(), true, 1.0f), out);
    std::cout << "rendered the Live view\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juce;
    const juce::StringArray args(argv + 1, argc - 1);
    try {
        if (args.size() >= 2 && args[0] == "modules") {
            juce::StringArray types(args);
            types.removeRange(0, 2);
            return renderModules(juce::File::getCurrentWorkingDirectory().getChildFile(args[1]), types);
        }
        if (args.size() == 2 && args[0] == "mutator")
            return renderMutator(juce::File::getCurrentWorkingDirectory().getChildFile(args[1]));
        if (args.size() == 3 && args[0] == "tooltip")
            return renderTooltip(args[1].getIntValue(), juce::File::getCurrentWorkingDirectory().getChildFile(args[2]));
        if (args.size() == 3 && args[0] == "ui")
            return renderUi(juce::File::getCurrentWorkingDirectory().getChildFile(args[1]),
                            juce::File::getCurrentWorkingDirectory().getChildFile(args[2]));
        if (args.size() >= 2 && args[0] == "live")
            return renderLive(juce::File::getCurrentWorkingDirectory().getChildFile(args[1]), !args.contains("--off"));
        if (args.size() == 3 && args[0] == "patch")
            return renderPatch(juce::File::getCurrentWorkingDirectory().getChildFile(args[1]),
                               juce::File::getCurrentWorkingDirectory().getChildFile(args[2]));
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "usage: g2render modules <out.png> [typeId...] | g2render patch <in.pch2> <out.png> | g2render ui <in.pch2> <out.png> | g2render live <out.png> [--off]\n";
    return 2;
}
