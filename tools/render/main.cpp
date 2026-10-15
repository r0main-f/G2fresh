// g2render: draws modules or a whole patch to a PNG with the editor's own
// painter, for checking the rendering without opening the app.
//
//   g2render modules <out.png> [typeId...]   (all selectable modules if none given)
//   g2render patch <file.pch2> <out.png>
//   g2render ui <file.pch2> <out.png>        (the whole editor window)
//   g2render synthsettings <out.png>         (the Synth Settings window, over the virtual G2)
//   g2render library <out.png> [--select <name>] [--search <words>] [--size <w>x<h>] <folder>...
//                                            (the Patch Library window over these folders of patches; nothing
//                                            of the user's settings is read or written)
//   g2render audition                        (checks the library's audition on the virtual G2: exit code 0 if the
//                                            document and the synth are left as they were)
//
// Set G2_LOOK=classic to draw with the original bitmaps instead of the modern look
// (they come from Clavia's download: G2_CLAVIA_DMG=<the .dmg>, or the one chosen in
// the app; without it, the modern look is drawn), and G2_ZOOM=<factor> to render the
// editor window zoomed.
#include "LiveView.h"
#include "MainView.h"
#include "MutatorWindow.h"
#include "PatchLibrary.h"
#include "SynthSettingsWindow.h"
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
    g2ui::LiveView view(&host);
    view.setSize(1400, 700);
    view.setHost(&host); // takes the snapshot
    writePng(view.createComponentSnapshot(view.getLocalBounds(), true, 1.0f), out);
    std::cout << "rendered the Live view\n";
    return 0;
}

// The Synth Settings window's content, over the virtual G2.
int renderSynthSettings(const juce::File& out)
{
    PatchDocument doc;
    SynthSync synth(doc);
    synth.connectVirtual();
    // no message loop here: the link's timer is run by hand until the virtual G2 is read
    for (int i = 0; i < 500 && !synth.ready(); ++i) {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
    }
    const auto panel = makeSynthSettingsPanel(synth);
    if (panel == nullptr) {
        std::cerr << "the virtual G2 did not come up\n";
        return 1;
    }
    writePng(panel->createComponentSnapshot(panel->getLocalBounds(), true, 2.0f), out);
    std::cout << "rendered the Synth Settings window\n";
    return 0;
}

// The Patch Library window's content over the given folders, once they are read. Its folders, stars and index are
// kept in memory and in a temporary file: the user's own library is not touched.
int renderLibrary(const juce::File& out, juce::StringArray args)
{
    juce::String select, search, size;
    for (auto* option : {&select, &search, &size}) {
        const int at = args.indexOf(option == &select ? "--select" : option == &search ? "--search" : "--size");
        if (at >= 0 && at + 1 < args.size()) {
            *option = args[at + 1];
            args.removeRange(at, 2);
        }
    }
    juce::StringArray folders;
    for (const auto& a : args)
        folders.add(juce::File::getCurrentWorkingDirectory().getChildFile(a).getFullPathName());
    auto values = std::make_shared<juce::StringPairArray>();
    values->set("libraryFolders", folders.joinIntoString("\n"));
    const auto cache = juce::File::createTempFile(".g2library");
    LibraryStore store;
    store.get = [values](const juce::String& key) { return (*values)[key]; };
    store.set = [values](const juce::String& key, const juce::String& value) { values->set(key, value); };
    store.cacheFile = cache;

    PatchDocument doc;
    SynthSync synth(doc);
    synth.connectVirtual(); // so that the audition controls show as with a synth
    for (int i = 0; i < 500 && !synth.ready(); ++i) {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
    }
    {
        auto panel = makePatchLibraryPanel({doc, &synth, {}, {}}, store);
        if (size.isNotEmpty())
            panel->setSize(size.upToFirstOccurrenceOf("x", false, true).getIntValue(),
                           size.fromFirstOccurrenceOf("x", false, true).getIntValue());
        const auto start = juce::Time::getMillisecondCounterHiRes();
        // no message loop here: the panel's timer is run by hand until the folders are read
        for (int i = 0; i < 6000 && libraryScanning(*panel); ++i) {
            juce::Thread::sleep(10);
            juce::Timer::callPendingTimersSynchronously();
        }
        std::cout << "folders read in " << juce::roundToInt(juce::Time::getMillisecondCounterHiRes() - start) << " ms\n";
        if (search.isNotEmpty())
            librarySearch(*panel, search);
        if (select.isNotEmpty()) {
            const auto picture = juce::Time::getMillisecondCounterHiRes();
            librarySelect(*panel, select);
            std::cout << "detail pane and picture in " << juce::roundToInt(juce::Time::getMillisecondCounterHiRes() - picture)
                      << " ms\n";
        }
        writePng(panel->createComponentSnapshot(panel->getLocalBounds(), true, 2.0f), out);
    }
    cache.deleteFile();
    std::cout << "rendered the Patch Library window\n";
    return 0;
}

// The library's audition (SynthSync::audition) on the virtual G2: a patch, then a performance, are played over a
// document that is live on the synth and over one that is not; the document never changes, and what the synth had
// comes back.
int checkAudition()
{
    int failures = 0;
    auto check = [&failures](bool ok, const char* what) {
        std::cout << (ok ? "ok    " : "FAIL  ") << what << "\n";
        failures += ok ? 0 : 1;
    };
    PatchDocument doc;
    SynthSync synth(doc);
    synth.connectVirtual();
    // no message loop here: the link's timer and the change messages are run by hand, until the link has nothing
    // left to send or to wait for (however slow the machine is)
    auto settle = [&synth, &doc] {
        auto* link = dynamic_cast<const g2::proto::LocalLink*>(synth.link());
        int quiet = 0;
        for (int i = 0; i < 4000 && quiet < 12; ++i) {
            juce::Thread::sleep(3);
            juce::Timer::callPendingTimersSynchronously();
            doc.dispatchPendingMessages();
            synth.dispatchPendingMessages();
            const bool idle = link == nullptr || const_cast<g2::proto::LocalLink*>(link)->client().idle();
            quiet = idle && i >= 12 ? quiet + 1 : 0;
        }
    };
    for (int i = 0; i < 500 && !synth.ready(); ++i) {
        juce::Thread::sleep(10);
        juce::Timer::callPendingTimersSynchronously();
    }
    if (!synth.ready()) {
        std::cerr << "the virtual G2 did not come up\n";
        return 1;
    }
    auto named = [](const char* module) {
        g2::Patch p = g2::Patch::makeDefault();
        for (const auto& def : g2::db::modules())
            if (juce::String(def.shortName) == module)
                g2::edit::addModule(p, g2::Location::Va, def.typeId, 0, 0);
        return p;
    };
    const g2::Patch mine = named("OscA"), other = named("OscB"), third = named("Noise");
    auto slotPatch = [&synth](int slot) { return g2::savePatch(synth.link()->state().slots[static_cast<std::size_t>(slot)].patch); };
    auto slotName = [&synth](int slot) { return synth.link()->state().slots[static_cast<std::size_t>(slot)].name; };

    // ---- a patch document, live on slot B
    doc.loadPatch(mine, "Mine");
    synth.sendPatch(1);
    settle();
    check(synth.boundSlot() == 1 && slotPatch(1) == g2::savePatch(mine), "the document is live on slot B");
    const auto slotA = slotPatch(0);
    const bool dirty = doc.isDirty();

    synth.audition(other, "Other");
    settle();
    check(synth.auditioning(), "auditioning");
    check(slotPatch(1) == g2::savePatch(other) && slotName(1) == "Other", "the synth plays the auditioned patch in the document's slot");
    check(g2::savePatch(doc.patch()) == g2::savePatch(mine) && doc.name() == "Mine" && doc.isDirty() == dirty && !doc.canUndo(),
          "the document is untouched");
    check(synth.boundSlot() == 1, "and still bound");

    synth.audition(third, "Third");
    settle();
    check(slotPatch(1) == g2::savePatch(third), "another audition replaces the first");
    check(g2::savePatch(doc.patch()) == g2::savePatch(mine), "the document is untouched");

    // an edit made meanwhile waits, and is there when the audition ends
    doc.perform("Voices", [](g2::Patch& p) { g2::edit::setVoices(p, g2::edit::VoiceMode::Poly, 5); });
    settle();
    check(slotPatch(1) == g2::savePatch(third), "an edit of the document does not reach the auditioned slot");
    const auto edited = g2::savePatch(doc.patch());
    synth.endAudition();
    settle();
    check(!synth.auditioning() && slotPatch(1) == edited && slotName(1) == "Mine", "the end puts the document back, as it is now");
    check(slotPatch(0) == slotA, "the other slots never changed");
    check(g2::savePatch(doc.patch()) == edited, "the document is untouched");
    doc.perform("Add module", [](g2::Patch& p) { g2::edit::addModule(p, g2::Location::Va, 5, 1, 0); });
    settle();
    check(slotPatch(1) == g2::savePatch(doc.patch()), "and live editing goes on");

    // the auditioned patch opened: it stays in the slot, and becomes the live document
    synth.audition(other, "Other");
    settle();
    check(synth.adoptAudition(0) && !synth.auditioning(), "the auditioned patch is adopted for a document that loads it");
    doc.loadPatch(other, "Other");
    settle();
    check(slotPatch(1) == g2::savePatch(other) && slotName(1) == "Other" && synth.boundSlot() == 1, "it stays in the slot, bound");
    doc.perform("Add module", [](g2::Patch& p) { g2::edit::addModule(p, g2::Location::Va, 5, 1, 0); });
    settle();
    check(slotPatch(1) == g2::savePatch(doc.patch()), "and is edited live");
    check(!synth.adoptAudition(0), "nothing to adopt without an audition");
    doc.loadPatch(mine, "Mine");
    settle();
    check(slotPatch(1) == g2::savePatch(mine), "(the first patch again, sent by the live editing)");
    doc.perform("Voices", [](g2::Patch& p) { g2::edit::setVoices(p, g2::edit::VoiceMode::Poly, 7); });
    juce::Thread::sleep(700); // a voice count goes out after 0.6 s
    settle();

    // ---- a performance auditioned over it
    g2::Performance perf = g2::Performance::playing(other, "Perf A");
    perf.slots[2] = third;
    perf.header.slots[2].patchName = "Perf C";
    const auto before = g2::savePatch(doc.patch());
    const bool modeBefore = synth.link()->state().settings.perfMode;
    synth.audition(perf, "Audition");
    settle();
    check(slotPatch(0) == g2::savePatch(other) && slotPatch(2) == g2::savePatch(third) && synth.link()->state().perfName == "Audition",
          "the synth plays the auditioned performance");
    check(g2::savePatch(doc.patch()) == before && !doc.isPerformance(), "the document is untouched");
    synth.audition(mine, "Mine again"); // a patch after a performance: the slots come back first
    settle();
    check(slotPatch(0) == slotA && slotPatch(1) == g2::savePatch(mine), "a patch auditioned after a performance plays over the old slots");
    synth.audition(perf, "Audition");
    settle();
    synth.endAudition();
    settle();
    check(slotPatch(0) == slotA && slotPatch(1) == before && slotName(1) == "Mine" && slotPatch(2) != g2::savePatch(third),
          "the end puts back every slot, the document's with the document");
    check(synth.link()->state().settings.perfMode == modeBefore, "and the synth's performance mode");
    check(synth.boundSlot() == 1 && g2::savePatch(doc.patch()) == before, "the document is untouched and still bound");

    // ---- a document that is not live: the synth gets back what it had
    synth.unbind();
    const auto synthB = slotPatch(1);
    const auto nameB = slotName(1);
    synth.audition(other, "Other");
    settle();
    check(g2::savePatch(doc.patch()) == before, "not bound: the document is untouched");
    synth.endAudition();
    settle();
    check(slotPatch(1) == synthB && slotName(1) == nameB, "not bound: the slot gets back the patch it had");
    check(!synth.bound(), "and the document is still not bound");
    // stopped and started again at once, before the synth has taken anything: still the first patch that comes back
    synth.audition(other, "Other");
    synth.endAudition();
    synth.audition(third, "Third");
    synth.audition(perf, "Audition");
    synth.endAudition();
    synth.audition(other, "Other");
    synth.endAudition();
    settle();
    check(slotPatch(1) == synthB && slotName(1) == nameB && slotPatch(0) == slotA, "auditions in a hurry leave the synth as it was");

    // ---- a performance document, live
    g2::Performance own = g2::Performance::playing(mine, "Own A");
    own.header.slots[1].enabled = 1;
    own.slots[1] = third;
    doc.loadPerformance(own, "Own");
    synth.sendPerformance();
    settle();
    const auto ownBytes = g2::savePerformance(*doc.performance());
    synth.audition(perf, "Audition");
    settle();
    check(g2::savePerformance(*doc.performance()) == ownBytes && doc.slot() == 0, "a live performance is untouched by an auditioned one");
    synth.endAudition();
    settle();
    check(slotPatch(0) == g2::savePatch(mine) && slotPatch(1) == g2::savePatch(third) && synth.link()->state().perfName == "Own"
              && synth.performanceBound(),
          "and comes back on the synth");
    // a patch after a performance, with an edit of another slot meanwhile: the performance is back around the patch
    synth.audition(perf, "Audition");
    settle();
    doc.setSlot(1);
    synth.audition(other, "Other");
    settle();
    check(slotPatch(0) == g2::savePatch(mine) && slotPatch(1) == g2::savePatch(other) && synth.link()->state().perfName == "Own",
          "a patch after a performance: the live performance is back around the auditioned slot");
    doc.setSlot(0);
    doc.perform("Add module", [](g2::Patch& p) { g2::edit::addModule(p, g2::Location::Va, 5, 1, 0); });
    settle();
    check(slotPatch(0) == g2::savePatch(doc.slotPatch(0)), "and its other slots are edited live");
    synth.endAudition();
    settle();
    check(slotPatch(1) == g2::savePatch(third) && slotPatch(0) == g2::savePatch(doc.slotPatch(0)), "the end puts the slot's own patch back");

    doc.setSlot(1);
    synth.audition(other, "Other"); // into the slot shown
    settle();
    check(slotPatch(1) == g2::savePatch(other) && slotPatch(0) == g2::savePatch(doc.slotPatch(0)), "a patch is auditioned in the slot shown");
    synth.endAudition();
    settle();
    check(slotPatch(1) == g2::savePatch(third), "and the slot's own patch comes back");

    std::cout << (failures == 0 ? "all checks passed" : "some checks FAILED") << "\n";
    return failures == 0 ? 0 : 1;
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
        if (args.size() == 2 && args[0] == "synthsettings")
            return renderSynthSettings(juce::File::getCurrentWorkingDirectory().getChildFile(args[1]));
        if (args.size() >= 3 && args[0] == "library") {
            juce::StringArray rest(args);
            rest.removeRange(0, 2);
            return renderLibrary(juce::File::getCurrentWorkingDirectory().getChildFile(args[1]), rest);
        }
        if (args.size() == 1 && args[0] == "audition")
            return checkAudition();
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
    std::cerr << "usage: g2render modules <out.png> [typeId...] | g2render patch <in.pch2> <out.png> | g2render ui <in.pch2> <out.png> | g2render live <out.png> [--off] | g2render library <out.png> <folder>... | g2render audition\n";
    return 2;
}
