// g2render: draws modules or a whole patch to a PNG with the editor's own
// painter, for checking the rendering without opening the app.
//
//   g2render modules <out.png> [typeId...]   (all selectable modules if none given)
//   g2render patch <file.pch2> <out.png>
//   g2render ui <file.pch2> <out.png>        (the whole editor window)
//
// Set G2_LOOK=classic to draw with the original bitmaps instead of the modern look,
// and G2_ZOOM=<factor> to render the editor window zoomed.
#include "MainView.h"
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
        if (args.size() == 3 && args[0] == "ui")
            return renderUi(juce::File::getCurrentWorkingDirectory().getChildFile(args[1]),
                            juce::File::getCurrentWorkingDirectory().getChildFile(args[2]));
        if (args.size() == 3 && args[0] == "patch")
            return renderPatch(juce::File::getCurrentWorkingDirectory().getChildFile(args[1]),
                               juce::File::getCurrentWorkingDirectory().getChildFile(args[2]));
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "usage: g2render modules <out.png> [typeId...] | g2render patch <in.pch2> <out.png> | g2render ui <in.pch2> <out.png>\n";
    return 2;
}
