#include "PatchLibrary.h"

#include "ModulePainter.h"
#include "PatchLibraryModel.h"
#include "Skin.h"
#include "Theme.h"

#include "g2/library.hpp"
#include "g2/module_db.hpp"

#include <filesystem>

namespace g2ui {
namespace {

using g2::library::Entry;
using Column = PatchLibraryModel::Column;

const juce::Colour kBackground{0xff2a2c31};
const juce::Colour kListBackground{0xff1f2125};
const juce::Colour kSelection{0xff3478d4};
const juce::Colour kDimText{0xffa8adb6};
const juce::Colour kFaintText{0xff7d828c};
const juce::Colour kStar{0xffffc94a};
const juce::Colour kPlaying{0xff6fd06f};
const juce::Colour kAmber{0xffffb347};
const juce::Colour kProblem{0xffff8a7a};

const char* const kFoldersKey = "libraryFolders";
const char* const kDefaultFolderKey = "libraryDefaultFolderAdded";
const char* const kFavouritesKey = "libraryFavourites";
const char* const kRecentKey = "libraryRecent";

// The index keeps paths and texts as UTF-8.
juce::String text(const std::string& utf8)
{
    return juce::String::fromUTF8(utf8.data(), static_cast<int>(utf8.size()));
}

std::filesystem::path toPath(const juce::File& f)
{
    const auto s = f.getFullPathName().toStdString();
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

juce::File toFile(const std::string& path)
{
    return juce::File(text(path));
}

juce::String slotLetter(int slot)
{
    return juce::String::charToString(static_cast<juce::juce_wchar>('A' + slot));
}

// ---- The scan, on its own thread ---------------------------------------------------------------------------------

// Reads the folders in the background (g2::library::scan) at a low priority, so that neither the window nor the
// audio waits for a disk; what it finds is handed over in batches, taken by the window on its timer.
class Scanner : private juce::Thread, private g2::library::ScanSink {
public:
    struct Update {
        std::vector<Entry> found;
        std::vector<std::string> removed;
        bool finished = false;
        g2::library::ScanStats stats;
        double milliseconds = 0.0;
    };

    explicit Scanner(juce::File cacheFile) : juce::Thread("G2fresh patch library"), cacheFile_(std::move(cacheFile)) {}
    ~Scanner() override { stopThread(20000); }

    // Reads `folders` (again): a scan under way is stopped first.
    void start(std::vector<std::filesystem::path> folders)
    {
        stopThread(20000);
        {
            const juce::ScopedLock lock(lock_);
            pending_ = {};
        }
        folders_ = std::move(folders);
        startThread(juce::Thread::Priority::low);
    }
    bool running() const { return isThreadRunning(); }

    // What was found since the last call (the window's thread).
    Update take()
    {
        const juce::ScopedLock lock(lock_);
        Update out = std::move(pending_);
        pending_ = {};
        return out;
    }

private:
    void run() override
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        if (!cacheLoaded_) {
            cache_.load(toPath(cacheFile_));
            cacheLoaded_ = true;
        }
        const auto stats = g2::library::scan(folders_, cache_, *this);
        // A scan that was stopped keeps what it read: the next one goes on from there.
        if (cache_.dirty() && cacheFile_ != juce::File())
            cache_.save(toPath(cacheFile_));
        const juce::ScopedLock lock(lock_);
        pending_.finished = !stats.cancelled;
        pending_.stats = stats;
        pending_.milliseconds = juce::Time::getMillisecondCounterHiRes() - start;
    }

    void found(std::vector<Entry> entries) override
    {
        const juce::ScopedLock lock(lock_);
        for (auto& e : entries)
            pending_.found.push_back(std::move(e));
    }
    void removed(std::vector<std::string> paths) override
    {
        const juce::ScopedLock lock(lock_);
        for (auto& p : paths)
            pending_.removed.push_back(std::move(p));
    }
    bool cancelled() override { return threadShouldExit(); }

    juce::File cacheFile_;
    std::vector<std::filesystem::path> folders_;
    g2::library::Cache cache_; // the thread's
    bool cacheLoaded_ = false;
    juce::CriticalSection lock_;
    Update pending_;
};

// ---- A small picture of a patch --------------------------------------------------------------------------------------

juce::Rectangle<int> areaBounds(const g2::Area& area)
{
    juce::Rectangle<int> r;
    for (const auto& m : area.modules)
        r = r.getUnion(ModulePainter::moduleBounds(m));
    return r;
}

// Where a jack is in its area (as AreaView::jackCentre).
std::optional<juce::Point<float>> jackCentre(const g2::Area& area, std::uint8_t module, std::uint8_t conn, bool output)
{
    const auto* m = area.find(module);
    if (m == nullptr)
        return std::nullopt;
    const auto origin = ModulePainter::moduleBounds(*m).getPosition().toFloat();
    if (const auto* panel = ModulePainter::panelFor(*m))
        for (const auto& e : panel->elements)
            if (e.kind == (output ? "Output" : "Input") && e.codeRef == conn)
                return origin + ModulePainter::elementBounds(e).toFloat().getCentre();
    return origin + juce::Point<float>(output ? kModuleWidth - 8.0f : 8.0f, 8.0f);
}

// One area with the editor's own module painter, and its cables as plain sagging cords `cord` pixels thick.
void drawArea(juce::Graphics& g, const g2::Patch& patch, g2::Location loc, float cord)
{
    const auto& area = patch.area(loc);
    for (const auto& m : area.modules) {
        const auto b = ModulePainter::moduleBounds(m);
        juce::Graphics::ScopedSaveState state(g);
        g.setOrigin(b.getPosition());
        g.reduceClipRegion(b.withZeroOrigin());
        ModulePainter::paint(g, {patch, loc, m, patch.header.activeVariation < g2::kUserVariations ? patch.header.activeVariation : 0,
                                 ModulePainter::panelFor(m), currentLook()});
    }
    for (const auto& c : area.cables) {
        if (!patch.header.cablesVisible[static_cast<std::size_t>(c.color)])
            continue;
        const auto a = jackCentre(area, c.fromModule, c.fromConn, c.fromIsOutput);
        const auto b = jackCentre(area, c.toModule, c.toConn, false);
        if (!a || !b)
            continue;
        juce::Path cable;
        cable.startNewSubPath(*a);
        const float sag = std::min(60.0f, 0.25f * a->getDistanceFrom(*b));
        cable.quadraticTo((*a + *b) * 0.5f + juce::Point<float>(0.0f, sag), *b);
        g.setColour(ModulePainter::cableColour(c.color).withAlpha(0.9f));
        g.strokePath(cable, juce::PathStrokeType(cord, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

// A patch, voice area above FX area, fitted into width x height points (drawn at `pixels` per point).
juce::Image patchPicture(const g2::Patch& patch, int width, int height, float pixels)
{
    const auto va = areaBounds(patch.va), fx = areaBounds(patch.fx);
    const int gap = fx.isEmpty() || va.isEmpty() ? 0 : 2 * kRowHeight;
    const int w = std::max(va.getRight(), fx.getRight()), h = va.getBottom() + gap + fx.getBottom();
    if (w <= 0 || h <= 0 || width <= 0 || height <= 0)
        return {};
    const float scale = std::min({static_cast<float>(width) / static_cast<float>(w),
                                  static_cast<float>(height) / static_cast<float>(h), 0.5f});
    juce::Image image(juce::Image::ARGB, std::max(1, juce::roundToInt(static_cast<float>(w) * scale * pixels)),
                      std::max(1, juce::roundToInt(static_cast<float>(h) * scale * pixels)), true);
    juce::Graphics g(image);
    g.addTransform(juce::AffineTransform::scale(scale * pixels));
    const float cord = 1.6f / scale; // the same thickness on screen, whatever the patch's size
    drawArea(g, patch, g2::Location::Va, cord);
    if (!fx.isEmpty()) {
        if (gap > 0) { // the line between the two areas
            g.setColour(juce::Colour(0xffb36ad6).withAlpha(0.8f));
            g.fillRect(0.0f, static_cast<float>(va.getBottom()) + static_cast<float>(gap) * 0.5f - 1.0f / scale, static_cast<float>(w),
                       2.0f / scale);
        }
        g.setOrigin(0, va.getBottom() + gap);
        drawArea(g, patch, g2::Location::Fx, cord);
    }
    return image;
}

// A file's picture: its patch, or a performance's slots that hold something, side by side in a grid.
juce::Image filePicture(const PatchDocument& doc, int width, int height, float pixels)
{
    const auto* perf = doc.performance();
    if (perf == nullptr)
        return patchPicture(doc.patch(), width, height, pixels);
    std::vector<int> used;
    for (int s = 0; s < 4; ++s)
        if (!perf->slots[static_cast<std::size_t>(s)].va.modules.empty() || !perf->slots[static_cast<std::size_t>(s)].fx.modules.empty())
            used.push_back(s);
    if (used.empty())
        return {};
    const int columns = used.size() > 1 ? 2 : 1, rows = used.size() > 2 ? 2 : 1;
    const int cellW = width / columns, cellH = height / rows;
    juce::Image image(juce::Image::ARGB, juce::roundToInt(static_cast<float>(width) * pixels),
                      juce::roundToInt(static_cast<float>(height) * pixels), true);
    juce::Graphics g(image);
    g.addTransform(juce::AffineTransform::scale(pixels));
    for (std::size_t i = 0; i < used.size(); ++i) {
        const auto cell = juce::Rectangle<int>(static_cast<int>(i % 2) * cellW, static_cast<int>(i / 2) * cellH, cellW, cellH).reduced(3);
        g.setColour(kFaintText);
        g.setFont(theme::font(true));
        g.drawText(slotLetter(used[i]), cell.withHeight(16), juce::Justification::topLeft);
        const auto picture = patchPicture(perf->slots[static_cast<std::size_t>(used[i])], cell.getWidth() - 16, cell.getHeight(), pixels);
        if (picture.isValid())
            g.drawImage(picture, cell.withTrimmedLeft(16).toFloat(),
                        juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yTop | juce::RectanglePlacement::onlyReduceInSize);
    }
    return image;
}

// ---- The detail pane ---------------------------------------------------------------------------------------------------

class Detail : public juce::Component {
public:
    static constexpr int kPictureHeight = 190;

    Detail()
    {
        body_.setMultiLine(true, true);
        body_.setReadOnly(true);
        body_.setCaretVisible(false);
        body_.setScrollbarsShown(true);
        body_.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        body_.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        body_.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        body_.setWantsKeyboardFocus(false);
        addAndMakeVisible(body_);
    }

    // The room for the picture, in points.
    juce::Rectangle<int> pictureArea() const { return picture_; }

    void show(const Entry* e, bool starred)
    {
        body_.clear();
        if (e == nullptr) {
            name_.clear();
            where_.clear();
            image_ = {};
            repaint();
            return;
        }
        name_ = text(e->name());
        starred_ = starred;
        where_ = text(e->path);
        auto add = [this](const juce::String& s, juce::Colour colour, bool bold = false) {
            body_.setFont(theme::font(bold));
            body_.setColour(juce::TextEditor::textColourId, colour);
            body_.moveCaretToEnd();
            body_.insertTextAtCaret(s);
        };
        for (const auto& line : PatchLibraryModel::facts(*e))
            add(text(line) + "\n", e->readable() ? juce::Colours::white : kProblem);
        add(juce::String(juce::File::descriptionOfSizeInBytes(static_cast<juce::int64>(e->size))) + ", "
                + PatchLibraryModel::date(e->mtime) + "\n",
            kDimText);
        if (e->readable() && !e->modules.empty()) {
            add("\nModules\n", kFaintText, true);
            add(text(g2::library::moduleSummary(*e)) + "\n", kDimText);
        }
        if (!e->notes.empty()) {
            add("\nNotes\n", kFaintText, true);
            add(text(e->notes) + "\n", juce::Colours::white);
        }
        body_.setCaretPosition(0);
        repaint();
    }

    void setPicture(juce::Image image)
    {
        image_ = std::move(image);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff25272b));
        auto r = getLocalBounds().reduced(12, 10);
        if (name_.isEmpty()) {
            g.setColour(kFaintText);
            g.setFont(theme::font());
            g.drawFittedText("Select a file to see what it holds", r.removeFromTop(60), juce::Justification::centredLeft, 2);
            return;
        }
        auto title = r.removeFromTop(24);
        if (starred_) {
            juce::Path star;
            star.addStar(title.removeFromRight(20).toFloat().getCentre(), 5, 3.6f, 8.0f);
            g.setColour(kStar);
            g.fillPath(star);
        }
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
        g.drawText(name_, title, juce::Justification::centredLeft, true);
        g.setColour(kFaintText);
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawFittedText(where_, r.removeFromTop(30), juce::Justification::topLeft, 2, 0.85f);
        g.setColour(juce::Colour(0xff1b1d20));
        g.fillRoundedRectangle(picture_.toFloat(), 4.0f);
        if (image_.isValid())
            g.drawImage(image_, picture_.reduced(4).toFloat(), juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(12, 10);
        r.removeFromTop(24 + 30 + 4);
        picture_ = r.removeFromTop(std::min(kPictureHeight, r.getHeight() / 2));
        r.removeFromTop(8);
        body_.setBounds(r.expanded(4, 0));
    }

private:
    juce::String name_, where_;
    bool starred_ = false;
    juce::Image image_;
    juce::Rectangle<int> picture_;
    juce::TextEditor body_;
};

// The search field: the arrow keys move the list's selection while the typing goes on.
class SearchField : public juce::TextEditor {
public:
    std::function<bool(const juce::KeyPress&)> onListKey;

    bool keyPressed(const juce::KeyPress& key) override
    {
        if ((key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::downKey)
             || key.isKeyCode(juce::KeyPress::pageUpKey) || key.isKeyCode(juce::KeyPress::pageDownKey))
            && onListKey && onListKey(key))
            return true;
        return juce::TextEditor::keyPressed(key);
    }
};

// The list: its keys go to the panel first (Space, typing).
class Table : public juce::TableListBox {
public:
    std::function<bool(const juce::KeyPress&)> onKey;

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (onKey && onKey(key))
            return true;
        return juce::TableListBox::keyPressed(key);
    }
};

} // namespace

LibraryStore LibraryStore::userSettings()
{
    LibraryStore store;
    store.get = [](const juce::String& key) { return g2ui::userSettings().getValue(key); };
    store.set = [](const juce::String& key, const juce::String& value) { g2ui::userSettings().setValue(key, value); };
    store.cacheFile = g2ui::userSettings().getFile().getSiblingFile("PatchLibrary.cache");
    return store;
}

// ---- The window's content -------------------------------------------------------------------------------------------------

class PatchLibraryPanel : public juce::Component,
                          private juce::TableListBoxModel,
                          private juce::Timer,
                          private juce::ChangeListener {
public:
    std::function<void()> onClose;

    PatchLibraryPanel(LibraryHost host, LibraryStore store)
        : host_(std::move(host)), store_(std::move(store)), scanner_(store_.cacheFile)
    {
        setLookAndFeel(&lookAndFeel_.get());
        setWantsKeyboardFocus(true);

        folders_.addLines(store_.get(kFoldersKey));
        folders_.removeEmptyStrings();
        {
            // A first use starts on G2fresh/Patches in the Music folder, made if missing (asked for after hand testing).
            std::vector<std::string> stored;
            for (const auto& f : folders_)
                stored.push_back(f.toStdString());
            const juce::File def{juce::String(defaultPatchFolder(
                juce::File::getSpecialLocation(juce::File::userMusicDirectory).getFullPathName().toStdString()))};
            const auto start = startingFolders(stored, store_.get(kDefaultFolderKey) == "1", def.getFullPathName().toStdString());
            if (start.addedDefault && (def.isDirectory() || def.createDirectory())) {
                folders_.add(def.getFullPathName());
                store_.set(kFoldersKey, folders_.joinIntoString("\n"));
                store_.set(kDefaultFolderKey, "1");
            }
        }
        model_.marks.setFavouritesText(store_.get(kFavouritesKey).toStdString());
        model_.marks.setRecentText(store_.get(kRecentKey).toStdString());

        // ---- search and filters
        search_.setFont(theme::font());
        search_.setTextToShowWhenEmpty("Search names, notes, folders and modules (try \"vocoder\")", kFaintText);
        search_.setSelectAllWhenFocused(true);
        search_.onTextChange = [this] {
            model_.query.text = search_.getText().toStdString();
            queryChanged();
        };
        search_.onReturnKey = [this] { openSelected(); };
        search_.onEscapeKey = [this] { escape(); };
        search_.onListKey = [this](const juce::KeyPress& key) { return listKey(key); };
        addAndMakeVisible(search_);

        type_.addItem("Patches and Performances", 1);
        type_.addItem("Patches", 2);
        type_.addItem("Performances", 3);
        type_.setSelectedId(1, juce::dontSendNotification);
        type_.onChange = [this] {
            model_.query.type = static_cast<g2::library::TypeFilter>(type_.getSelectedId() - 1);
            queryChanged();
        };
        addAndMakeVisible(type_);

        category_.addItem("Every Category", 1);
        for (int c = 0; c < 16; ++c)
            category_.addItem(g2::edit::categoryName(static_cast<std::uint8_t>(c)), 2 + c);
        category_.setSelectedId(1, juce::dontSendNotification);
        category_.onChange = [this] {
            model_.query.category = category_.getSelectedId() - 2;
            queryChanged();
        };
        addAndMakeVisible(category_);

        modules_.onClick = [this] { showModuleMenu(); };
        modules_.setTooltip("Only the files that contain these modules");
        addAndMakeVisible(modules_);

        auto filter = [this](juce::ToggleButton& b, bool g2::library::Query::* field, const juce::String& tip) {
            b.setTooltip(tip);
            b.onClick = [this, &b, field] {
                model_.query.*field = b.getToggleState();
                queryChanged();
            };
            addAndMakeVisible(b);
        };
        filter(starred_, &g2::library::Query::favourites, "Only the files you starred (click a file's star, or Cmd/Ctrl-D)");
        filter(recent_, &g2::library::Query::recent, "Only the files opened from here lately");
        filter(audioIn_, &g2::library::Query::audioIn, "Only the files with an In module that listens to the G2's audio inputs");
        filter(fits_, &g2::library::Query::fitsUnexpanded,
               "Only the files whose voices all fit a G2 without expansion (4 DSPs): G2fresh's load estimate and the "
               "manual's rule, so an estimate");
        filter(unreadable_, &g2::library::Query::unreadable, "The files that could not be read, and why");
        unreadable_.setVisible(false);

        // ---- the list
        auto& header = table_.getHeader();
        const int sortable = juce::TableHeaderComponent::defaultFlags;
        const int fixed = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::sortable;
        header.addColumn({}, static_cast<int>(Column::Favourite), 28, 28, 28, fixed);
        header.addColumn("Name", static_cast<int>(Column::Name), 240, 120, -1, sortable);
        header.addColumn("Type", static_cast<int>(Column::Type), 54, 50, 90, sortable);
        header.addColumn("Category", static_cast<int>(Column::Category), 84, 70, 130, sortable);
        header.addColumn("Voices", static_cast<int>(Column::Voices), 60, 58, 90, sortable);
        header.addColumn("Modules", static_cast<int>(Column::Modules), 68, 66, 90, sortable);
        header.addColumn("Load", static_cast<int>(Column::Load), 54, 52, 90, sortable);
        header.addColumn("Version", static_cast<int>(Column::Version), 62, 60, 90, sortable);
        header.addColumn("Date", static_cast<int>(Column::Date), 92, 90, 120, sortable);
        header.addColumn("Folder", static_cast<int>(Column::Folder), 200, 50, -1, sortable);
        header.setSortColumnId(static_cast<int>(Column::Name), true);
        header.setStretchToFitActive(true); // the columns share the list's width
        table_.getViewport()->setScrollBarsShown(true, false);
        header.setColour(juce::TableHeaderComponent::backgroundColourId, juce::Colour(0xff26282c));
        header.setColour(juce::TableHeaderComponent::textColourId, kDimText);
        header.setColour(juce::TableHeaderComponent::outlineColourId, juce::Colour(0xff3a3d44));
        header.setColour(juce::TableHeaderComponent::highlightColourId, juce::Colour(0xff32353b));
        table_.setModel(this);
        table_.setRowHeight(22);
        table_.setHeaderHeight(24);
        table_.setColour(juce::ListBox::backgroundColourId, kListBackground);
        table_.onKey = [this](const juce::KeyPress& key) { return tableKey(key); };
        addAndMakeVisible(table_);
        for (auto* link : {&factoryLink_, &communityLink_}) {
            link->setFont(theme::font(), false, juce::Justification::centred);
            addChildComponent(*link);
        }
        showFolder_.onClick = [this] {
            if (!folders_.isEmpty())
                juce::File(folders_[0]).revealToUser();
        };
        addChildComponent(showFolder_);
        addAndMakeVisible(detail_);

        // ---- the bottom row
        status_.setFont(theme::font());
        status_.setMinimumHorizontalScale(0.8f);
        addAndMakeVisible(status_);
        foldersButton_.onClick = [this] { showFoldersMenu(); };
        foldersButton_.setTooltip("The folders of patches this library shows, with their subfolders");
        rescan_.onClick = [this] { rescan(); };
        rescan_.setTooltip("Look in the folders again for new, changed and removed files (Cmd/Ctrl-R)");
        onSelect_.setTooltip("Audition every file as it is selected (off: Space auditions the selected file)");
        onSelect_.onClick = [this] {
            if (onSelect_.getToggleState())
                auditionSelected();
        };
        audition_.onClick = [this] { toggleAudition(); };
        slotButton_.onClick = [this] { showSlotMenu(); };
        open_.onClick = [this] { openSelected(); };
        open_.setTooltip("Open the file in the editor, as File > Open (Return, or a double-click)");
        for (auto* b : {&foldersButton_, &rescan_, &audition_, &slotButton_, &open_})
            addAndMakeVisible(*b);
        addAndMakeVisible(onSelect_);

        if (host_.synth != nullptr)
            host_.synth->addChangeListener(this);
        host_.doc.addChangeListener(this);
        updateModulesButton();
        setSize(1180, 700);
        rescan();
    }

    ~PatchLibraryPanel() override
    {
        stopAudition();
        host_.doc.removeChangeListener(this);
        if (host_.synth != nullptr)
            host_.synth->removeChangeListener(this);
        table_.setModel(nullptr);
        setLookAndFeel(nullptr);
    }

    bool scanning() const { return scanning_; }

    // The window opened (again): what changed on disk since shows, and typing searches.
    void opened()
    {
        if (!scanning_)
            rescan();
        startTimerHz(20);
        search_.grabKeyboardFocus();
    }
    // The window closed.
    void closed()
    {
        stopAudition();
        onSelect_.setToggleState(false, juce::dontSendNotification);
    }

    void searchFor(const juce::String& words)
    {
        search_.setText(words, false);
        model_.query.text = words.toStdString();
        queryChanged();
    }

    void selectNamed(const juce::String& name)
    {
        for (int row = 0; row < model_.rowCount(); ++row)
            if (text(model_.entry(row).name()).containsIgnoreCase(name)) {
                model_.select(row);
                rowsChanged(true);
                break;
            }
        showPicture(); // now, not when the timer comes to it
    }

    void paint(juce::Graphics& g) override { g.fillAll(kBackground); }

    void paintOverChildren(juce::Graphics& g) override
    {
        // An empty list says what to do about it.
        juce::String hint;
        if (folders_.isEmpty())
            hint = "Add a folder of G2 patches (.pch2) and performances (.prf2): Folders > Add Folder...\n\n"
                   "G2fresh comes without patches. It reads yours where they are, with their subfolders, and never "
                   "changes them.";
        else if (model_.index.size() == 0 && !scanning_)
            hint = "No .pch2 or .prf2 file in "
                 + (folders_.size() == 1 ? juce::File(folders_[0]).getParentDirectory().getFileName() + "/" + juce::File(folders_[0]).getFileName()
                                         : juce::String("the chosen folders"))
                 + " yet. Put your patches there (subfolders too), or add a folder: Folders > Add Folder...\n\n"
                   "G2fresh comes without patches. You can download these, unzip them into the folder, then Rescan:";
        else if (model_.rowCount() == 0 && !scanning_)
            hint = "No file matches the search and the filters.";
        if (hint.isEmpty())
            return;
        g.setColour(kDimText);
        g.setFont(theme::font());
        auto area = table_.getBounds().reduced(40).withTrimmedTop(table_.getHeaderHeight());
        if (factoryLink_.isVisible())
            area = area.withBottom(factoryLink_.getY() - 10); // above the links
        g.drawFittedText(hint, area, juce::Justification::centredBottom, 6);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(10);
        auto top = r.removeFromTop(26);
        modules_.setBounds(top.removeFromRight(190));
        top.removeFromRight(6);
        category_.setBounds(top.removeFromRight(140));
        top.removeFromRight(6);
        type_.setBounds(top.removeFromRight(200));
        top.removeFromRight(8);
        search_.setBounds(top);
        r.removeFromTop(6);
        auto filters = r.removeFromTop(24);
        for (auto* b : {&starred_, &recent_, &audioIn_, &fits_, &unreadable_}) {
            if (!b->isVisible())
                continue;
            b->setBounds(filters.removeFromLeft(juce::GlyphArrangement::getStringWidthInt(theme::font(), b->getButtonText()) + 44));
            filters.removeFromLeft(6);
        }
        r.removeFromTop(6);

        auto bottom = r.removeFromBottom(28);
        foldersButton_.setBounds(bottom.removeFromLeft(96));
        bottom.removeFromLeft(6);
        rescan_.setBounds(bottom.removeFromLeft(76));
        open_.setBounds(bottom.removeFromRight(84));
        bottom.removeFromRight(6);
        slotButton_.setBounds(bottom.removeFromRight(118));
        bottom.removeFromRight(6);
        audition_.setBounds(bottom.removeFromRight(124));
        bottom.removeFromRight(6);
        onSelect_.setBounds(bottom.removeFromRight(152));
        r.removeFromBottom(6);
        status_.setBounds(r.removeFromBottom(22));
        r.removeFromBottom(4);

        detail_.setBounds(r.removeFromRight(juce::jlimit(280, 380, getWidth() * 3 / 10)));
        r.removeFromRight(8);
        table_.setBounds(r);
        {
            // The links under the empty list's hint.
            auto links = r.withTrimmedTop(r.getHeight() / 2 + 30).reduced(40, 0);
            factoryLink_.setBounds(links.removeFromTop(24));
            communityLink_.setBounds(links.removeFromTop(24));
            links.removeFromTop(8);
            showFolder_.setBounds(links.removeFromTop(26).withSizeKeepingCentre(220, 26));
        }
        updateEmptyLinks();
        pictureDue_ = juce::Time::getMillisecondCounter() + 150; // the picture's room may have changed
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto cmd = juce::ModifierKeys::commandModifier;
        if (key == juce::KeyPress::escapeKey)
            return escape(), true;
        if (key == juce::KeyPress::returnKey)
            return openSelected(), true;
        if (key == juce::KeyPress('f', cmd, 0))
            return search_.grabKeyboardFocus(), true;
        if (key == juce::KeyPress('d', cmd, 0))
            return toggleStar(model_.selectedRow()), true;
        if (key == juce::KeyPress('r', cmd, 0))
            return rescan(), true;
        if ((key == juce::KeyPress('w', cmd, 0) || key == juce::KeyPress('b', cmd, 0)) && onClose)
            return onClose(), true;
        return listKey(key);
    }

private:
    // ---- TableListBoxModel
    int getNumRows() override { return model_.rowCount(); }

    void paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) override
    {
        if (selected)
            g.fillAll(table_.hasKeyboardFocus(true) || search_.hasKeyboardFocus(true) ? kSelection : kSelection.withMultipliedSaturation(0.55f));
        else if (row % 2 != 0)
            g.fillAll(juce::Colour(0xff222428));
    }

    void paintCell(juce::Graphics& g, int row, int columnId, int width, int height, bool selected) override
    {
        if (row >= model_.rowCount())
            return;
        const Entry& e = model_.entry(row);
        const auto column = static_cast<Column>(columnId);
        const auto area = juce::Rectangle<int>(0, 0, width, height);
        if (column == Column::Favourite) {
            juce::Path star;
            star.addStar(area.toFloat().getCentre(), 5, 3.2f, 7.0f);
            if (model_.marks.favourite(e.path)) {
                g.setColour(kStar);
                g.fillPath(star);
            } else {
                g.setColour(selected ? juce::Colours::white.withAlpha(0.5f) : juce::Colour(0xff4a4f58));
                g.strokePath(star, juce::PathStrokeType(1.0f));
            }
            return;
        }
        auto r = area.reduced(6, 0);
        juce::Colour colour = column == Column::Name ? juce::Colours::white : kDimText;
        if (!e.readable())
            colour = kProblem;
        else if (column == Column::Version && e.converted())
            colour = kAmber; // converted when it is opened
        else if (column == Column::Load && e.voiceLoad() > 1000)
            colour = kProblem;
        if (selected)
            colour = juce::Colours::white;
        if (column == Column::Name && e.path == auditioned_) {
            // The file that plays.
            juce::Path play;
            const auto c = r.removeFromLeft(14).toFloat().getCentre();
            play.addTriangle(c.x - 4.0f, c.y - 5.0f, c.x - 4.0f, c.y + 5.0f, c.x + 5.0f, c.y);
            g.setColour(selected ? juce::Colours::white : kPlaying);
            g.fillPath(play);
            if (!selected)
                colour = kPlaying;
        }
        const bool number = column == Column::Voices || column == Column::Modules || column == Column::Load || column == Column::Version;
        g.setColour(colour);
        g.setFont(theme::font());
        auto shown = text(PatchLibraryModel::cell(e, column));
        if (column == Column::Folder) {
            // A folder too long for its column keeps its end: the folder the file is in says the most.
            const auto ellipsis = juce::String::fromUTF8("\xe2\x80\xa6/");
            const auto whole = shown;
            while (shown.containsChar('/') && juce::GlyphArrangement::getStringWidthInt(theme::font(), ellipsis + shown) > r.getWidth())
                shown = shown.fromFirstOccurrenceOf("/", false, false);
            if (shown != whole)
                shown = ellipsis + shown;
        }
        g.drawText(shown, r, number ? juce::Justification::centredRight : juce::Justification::centredLeft, true);
    }

    void sortOrderChanged(int columnId, bool forwards) override
    {
        model_.sortBy(static_cast<Column>(columnId), forwards);
        rowsChanged(true);
    }

    void cellClicked(int row, int columnId, const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            showRowMenu(row);
        else if (static_cast<Column>(columnId) == Column::Favourite)
            toggleStar(row);
    }

    void cellDoubleClicked(int, int columnId, const juce::MouseEvent&) override
    {
        if (static_cast<Column>(columnId) != Column::Favourite)
            openSelected();
    }

    void selectedRowsChanged(int row) override
    {
        if (updating_)
            return;
        model_.select(row);
        selectionChanged();
    }

    void returnKeyPressed(int) override { openSelected(); }

    juce::String getCellTooltip(int row, int columnId) override
    {
        if (row >= model_.rowCount())
            return {};
        const Entry& e = model_.entry(row);
        if (!e.readable())
            return text(e.name() + "\nCannot be read: " + e.error);
        switch (static_cast<Column>(columnId)) {
        case Column::Favourite: return "Star this file (Cmd/Ctrl-D)";
        case Column::Version: return e.converted() ? "An older file format: converted when it is opened" : juce::String();
        case Column::Load: return "One voice's share of a DSP, estimated by G2fresh (the larger of cycles and memory)";
        case Column::Folder: return text(e.path);
        case Column::Name:
        case Column::Type:
        case Column::Category:
        case Column::Voices:
        case Column::Modules:
        case Column::Date: break;
        }
        return {};
    }

    // ---- Keys
    // Up, Down, Page Up and Page Down move the selection from anywhere in the window.
    bool listKey(const juce::KeyPress& key)
    {
        const int page = std::max(1, table_.getHeight() / table_.getRowHeight() - 2);
        int delta = 0;
        if (key.isKeyCode(juce::KeyPress::upKey))
            delta = -1;
        else if (key.isKeyCode(juce::KeyPress::downKey))
            delta = 1;
        else if (key.isKeyCode(juce::KeyPress::pageUpKey))
            delta = -page;
        else if (key.isKeyCode(juce::KeyPress::pageDownKey))
            delta = page;
        else
            return false;
        model_.step(delta);
        rowsChanged(true);
        // The keyboard is in the list from here (Space auditions there); typing takes it back to the search field.
        if (!table_.hasKeyboardFocus(true))
            table_.grabKeyboardFocus();
        return true;
    }

    // In the list: Space auditions, and typing goes on in the search field.
    bool tableKey(const juce::KeyPress& key)
    {
        if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown())
            return false;
        if (key == juce::KeyPress::spaceKey) {
            toggleAudition();
            return true;
        }
        if (key == juce::KeyPress::backspaceKey) {
            search_.setText(search_.getText().dropLastCharacters(1));
            return true;
        }
        const auto c = key.getTextCharacter();
        if (c > ' ' && c != 0x7f && !key.isKeyCode(juce::KeyPress::deleteKey)) {
            search_.setText(search_.getText() + juce::String::charToString(c));
            search_.grabKeyboardFocus();
            search_.setHighlightedRegion({});
            search_.moveCaretToEnd();
            return true;
        }
        return false;
    }

    // Escape: stops an audition, else empties the search, else closes the window.
    void escape()
    {
        if (!auditioned_.empty())
            stopAudition();
        else if (search_.getText().isNotEmpty())
            search_.setText({});
        else if (onClose)
            onClose();
    }

    // ---- Rows and selection
    void queryChanged()
    {
        model_.refresh();
        rowsChanged(true);
    }

    // The model's rows changed: the list follows. `show` scrolls to the selection (not while a scan adds rows).
    void rowsChanged(bool show)
    {
        updating_ = true;
        table_.updateContent();
        // (the width left beside the scroll bar, which comes and goes with the rows)
        table_.getHeader().resizeAllColumnsToFit(table_.getVisibleContentWidth());
        const int row = model_.selectedRow();
        if (row >= 0)
            table_.selectRow(row, !show, true);
        else
            table_.deselectAllRows();
        updating_ = false;
        table_.repaint();
        updateEmptyLinks();
        repaint(); // the hint over an empty list
        selectionChanged();
    }

    void selectionChanged()
    {
        const Entry* e = model_.selected();
        const bool starred = e != nullptr && model_.marks.favourite(e->path);
        if ((e == nullptr) != !shown_ || (e != nullptr && (!(*e == *shown_) || starred != shownStarred_))) {
            const bool sameFile = e != nullptr && shown_ && e->path == shown_->path && e->size == shown_->size && e->mtime == shown_->mtime;
            shown_ = e != nullptr ? std::optional<Entry>(*e) : std::nullopt;
            shownStarred_ = starred;
            detail_.show(e, starred);
            if (!sameFile) {
                detail_.setPicture({});
                pictureDue_ = juce::Time::getMillisecondCounter() + 120; // not for every row an arrow key passes
                if (onSelect_.getToggleState())
                    auditionDue_ = juce::Time::getMillisecondCounter() + 300;
            }
        }
        updateButtons();
    }

    void updateStatus()
    {
        const auto* synth = host_.synth;
        if (!auditioned_.empty() && synth != nullptr) {
            Entry playing;
            playing.path = auditioned_;
            status_.setText("Auditioning \"" + text(playing.name()) + "\" on " + synth->statusText()
                                + ". The open patch is untouched: Space or Escape stops the audition, Return opens the file.",
                            juce::dontSendNotification);
            status_.setColour(juce::Label::textColourId, kPlaying);
            return;
        }
        juce::String s = text(model_.status(scanning_));
        if (message_.isNotEmpty())
            s << "  |  " << message_;
        else if (synth == nullptr || !synth->ready())
            s << "  |  To hear a file with Space, connect a G2 (Synth menu)";
        status_.setText(s, juce::dontSendNotification);
        status_.setColour(juce::Label::textColourId, kDimText);
    }

    void updateButtons()
    {
        const Entry* e = model_.selected();
        const bool ready = host_.synth != nullptr && host_.synth->ready();
        const bool playing = e != nullptr && e->path == auditioned_;
        open_.setEnabled(e != nullptr);
        audition_.setEnabled(ready && ((e != nullptr && e->readable()) || !auditioned_.empty()));
        audition_.setButtonText(playing || (e == nullptr && !auditioned_.empty()) ? "Stop Audition" : "Audition");
        audition_.setTooltip(ready ? "Play the selected file on " + host_.synth->statusText()
                                         + " without changing the open patch (Space); what was there comes back when "
                                           "the audition stops or this window closes"
                                   : juce::String("Connect a G2 first (Synth menu): the Emulated G2, or a G2 on USB"));
        onSelect_.setEnabled(ready);
        slotButton_.setEnabled(e != nullptr && slotsOffered(*e));
        slotButton_.setTooltip(host_.doc.isPerformance() ? "Put the selected patch in a slot of the open performance"
                                                         : "Open the selected patch and send it to a slot of the connected G2 "
                                                           "(the original's File > Open To)");
        updateStatus();
    }

    // ---- The scan
    void rescan()
    {
        std::vector<std::filesystem::path> folders;
        for (const auto& f : folders_)
            folders.push_back(toPath(juce::File(f)));
        // The files of a folder that is no longer chosen go at once; the others stay until the scan says otherwise.
        std::vector<std::string> gone;
        for (const auto& e : model_.index.entries()) {
            bool chosen = false;
            for (const auto& f : folders_)
                chosen = chosen || juce::File(text(e.path)).isAChildOf(juce::File(f));
            if (!chosen)
                gone.push_back(e.path);
        }
        for (const auto& path : gone)
            model_.index.remove(path);
        if (!gone.empty()) {
            model_.refresh();
            rowsChanged(false);
        }
        scanner_.start(std::move(folders));
        scanning_ = true;
        message_.clear();
        startTimerHz(20);
        updateStatus();
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (scanning_) {
            auto update = scanner_.take();
            for (auto& e : update.found)
                model_.index.upsert(std::move(e));
            for (const auto& path : update.removed)
                model_.index.remove(path);
            rowsStale_ = rowsStale_ || !update.found.empty() || !update.removed.empty();
            if (update.finished || (!scanner_.running() && !rowsStale_)) {
                scanning_ = false;
                updateEmptyLinks();

                if (update.finished && update.stats.read > 0)
                    message_ = "Read " + juce::String(static_cast<int>(update.stats.read)) + " new or changed "
                               + (update.stats.read == 1 ? "file" : "files") + " in "
                               + juce::String(update.milliseconds / 1000.0, 1) + " s";
            }
            // The list fills as the scan goes, a few times a second.
            if (rowsStale_ && (!scanning_ || now - lastRefresh_ > 250)) {
                rowsStale_ = false;
                lastRefresh_ = now;
                const std::size_t unreadable = model_.index.unreadableCount();
                if (unreadable_.isVisible() != (unreadable != 0 || unreadable_.getToggleState())) {
                    unreadable_.setVisible(unreadable != 0 || unreadable_.getToggleState());
                    resized();
                }
                unreadable_.setButtonText("Unreadable (" + juce::String(static_cast<int>(unreadable)) + ")");
                model_.refresh();
                rowsChanged(false);
            }
            if (!scanning_)
                updateStatus();
        }
        if (pictureDue_ != 0 && now >= pictureDue_)
            showPicture();
        if (auditionDue_ != 0 && now >= auditionDue_) {
            auditionDue_ = 0;
            if (onSelect_.getToggleState())
                auditionSelected();
        }
        if (!scanning_ && pictureDue_ == 0 && auditionDue_ == 0 && !isShowing())
            stopTimer(); // nothing to wait for behind a closed window
    }

    // ---- Files
    // Loads a file as File > Open would (conversion and repairs included) into a document of its own. Returns why
    // not, or "".
    static juce::String load(const Entry& e, PatchDocument& into)
    {
        const auto file = toFile(e.path);
        juce::MemoryBlock data;
        if (!file.loadFileAsData(data))
            return "cannot be read";
        try {
            const auto* p = static_cast<const std::uint8_t*>(data.getData());
            into.loadBytes(std::vector<std::uint8_t>(p, p + data.getSize()));
            into.setFile(file);
        } catch (const g2::ChecksumError&) {
            return "its checksum is wrong (Open asks what to do)";
        } catch (const std::exception& ex) {
            return ex.what();
        }
        return {};
    }

    void showPicture()
    {
        pictureDue_ = 0;
        const Entry* e = model_.selected();
        const auto room = detail_.pictureArea().reduced(4);
        if (e == nullptr || !e->readable() || room.isEmpty()) {
            detail_.setPicture({});
            return;
        }
        PatchDocument doc;
        if (load(*e, doc).isNotEmpty()) {
            detail_.setPicture({});
            return;
        }
        const float pixels = static_cast<float>(juce::jlimit(1.0, 2.0, juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                                                                           ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->scale
                                                                           : 1.0));
        detail_.setPicture(filePicture(doc, room.getWidth(), room.getHeight(), pixels));
    }

    void openSelected()
    {
        const Entry* e = model_.selected();
        if (e == nullptr || !host_.confirmDiscard || !host_.openFile)
            return;
        const auto path = e->path;
        host_.confirmDiscard([safe = juce::Component::SafePointer<PatchLibraryPanel>(this), path] {
            if (safe != nullptr)
                safe->open(path, -1);
        });
    }

    // Opens a file in the editor (the document is replaced); `slot` >= 0 also sends it to that slot of the synth, as
    // the original's Open To.
    void open(const std::string& path, int slot)
    {
        const auto file = toFile(path);
        // The file that plays, opened where the editor is live: it stays in its slot and the sound goes on. Else
        // the synth gets back what it had before the document changes.
        const bool adopted = path == auditioned_ && host_.synth != nullptr && !host_.doc.isPerformance() && host_.synth->adoptAudition(0);
        stopAudition();
        host_.openFile(file);
        if (host_.doc.file() != file) {
            if (adopted)
                host_.doc.sendChangeMessage(); // not opened after all: the live editing brings the document back
            return; // (unreadable, or a question is being asked)
        }
        model_.opened(path);
        store_.set(kRecentKey, text(model_.marks.recentText()));
        auto* synth = host_.synth;
        if (synth != nullptr && synth->ready()) {
            if (slot >= 0 && !host_.doc.isPerformance())
                synth->sendPatch(slot);
            // The original's Open downloads to the synth (CPatchFilePort::LoadComplete 0x6cbce). G2fresh's does not on a
            // G2 (see README), but the Emulated G2 is there to play the document: when the editor is not live on it,
            // or will not be (a performance opened over a live patch, or the reverse, ends the live editing), the
            // document is sent.
            else if (synth->kind() == SynthSync::Kind::Emulated
                     && (host_.doc.isPerformance() ? !synth->performanceBound() : synth->boundSlot() < 0)) {
                if (host_.doc.isPerformance())
                    synth->sendPerformance();
                else
                    synth->sendPatchAlone();
            }
        }
        rowsChanged(false);
    }

    // Whether "Open in Slot" applies: a patch, into the open performance, or to a slot of the connected synth.
    bool slotsOffered(const Entry& e) const
    {
        if (!e.readable() || e.performance)
            return false;
        return host_.doc.isPerformance() || (host_.synth != nullptr && host_.synth->ready());
    }

    void openInSlot(int slot)
    {
        const Entry* e = model_.selected();
        if (e == nullptr || !slotsOffered(*e))
            return;
        const auto path = e->path;
        if (!host_.doc.isPerformance()) {
            if (host_.confirmDiscard)
                host_.confirmDiscard([safe = juce::Component::SafePointer<PatchLibraryPanel>(this), path, slot] {
                    if (safe != nullptr)
                        safe->open(path, slot);
                });
            return;
        }
        // Into the performance: the slot's patch is replaced, as one undo step.
        PatchDocument loaded;
        if (const auto error = load(*e, loaded); error.isNotEmpty() || loaded.isPerformance()) {
            say(text(e->name()) + ": " + (error.isNotEmpty() ? error : juce::String("not a patch")));
            return;
        }
        host_.doc.setSlot(slot);
        if (path == auditioned_ && host_.synth != nullptr)
            host_.synth->adoptAudition(slot); // it plays there already: it stays
        stopAudition();
        const g2::Patch patch = loaded.patch();
        if (host_.doc.perform("Open in Slot " + slotLetter(slot), [&patch](g2::Patch& p) { p = patch; })) {
            host_.doc.setName(loaded.name());
            model_.opened(path);
            store_.set(kRecentKey, text(model_.marks.recentText()));
            say("\"" + loaded.name() + "\" is in slot " + slotLetter(slot) + " of the performance (Edit > Undo takes it out)");
        }
    }

    // ---- Audition
    void toggleAudition()
    {
        const Entry* e = model_.selected();
        if (!auditioned_.empty() && (e == nullptr || e->path == auditioned_))
            stopAudition();
        else
            auditionSelected();
    }

    void auditionSelected()
    {
        auditionDue_ = 0;
        const Entry* e = model_.selected();
        auto* synth = host_.synth;
        if (e == nullptr || synth == nullptr || !synth->ready() || e->path == auditioned_)
            return;
        if (!e->readable()) {
            say(text(e->name()) + " cannot be read");
            return;
        }
        PatchDocument loaded;
        if (const auto error = load(*e, loaded); error.isNotEmpty()) {
            say(text(e->name()) + ": " + error);
            return;
        }
        message_.clear();
        if (loaded.isPerformance())
            synth->audition(*loaded.performance(), loaded.performanceName().toStdString());
        else
            synth->audition(loaded.patch(), loaded.name().toStdString());
        auditioned_ = e->path;
        table_.repaint();
        updateButtons();
    }

    void stopAudition()
    {
        auditionDue_ = 0;
        if (host_.synth != nullptr)
            host_.synth->endAudition();
        if (auditioned_.empty())
            return;
        auditioned_.clear();
        table_.repaint();
        updateButtons();
    }

    void changeListenerCallback(juce::ChangeBroadcaster* source) override
    {
        // The synth went, or got something else: no audition left to stop.
        if (source == host_.synth && !auditioned_.empty() && !host_.synth->auditioning()) {
            auditioned_.clear();
            table_.repaint();
        }
        if (isShowing())
            updateButtons();
    }

    // ---- Stars
    void toggleStar(int row)
    {
        if (row < 0 || row >= model_.rowCount())
            return;
        model_.toggleFavourite(row);
        store_.set(kFavouritesKey, text(model_.marks.favouritesText()));
        rowsChanged(false);
    }

    // ---- Menus
    void say(const juce::String& message)
    {
        message_ = message;
        updateStatus();
    }

    void updateModulesButton()
    {
        juce::StringArray names;
        for (const auto type : model_.query.modules)
            if (const auto* def = g2::db::find(type))
                names.add(def->shortName);
        modules_.setButtonText(names.isEmpty() ? juce::String("With Any Modules") : "With " + names.joinIntoString(" + "));
    }

    void showModuleMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Any Modules", !model_.query.modules.empty());
        menu.addSeparator();
        const auto categories = g2::db::categories();
        for (std::size_t c = 0; c < categories.size(); ++c) {
            std::vector<const g2::db::ModuleDef*> defs;
            for (const auto& def : g2::db::modules())
                if (def.selectable && def.kind == g2::db::ModuleKind::Module && def.category == static_cast<int>(c))
                    defs.push_back(&def);
            std::sort(defs.begin(), defs.end(), [](const auto* a, const auto* b) { return a->browserOrder < b->browserOrder; });
            juce::PopupMenu sub;
            bool any = false;
            for (const auto* def : defs) {
                const bool chosen = std::find(model_.query.modules.begin(), model_.query.modules.end(), def->typeId)
                                    != model_.query.modules.end();
                any = any || chosen;
                sub.addItem(100 + def->typeId, def->longName != nullptr ? juce::String(def->shortName) + "  -  " + def->longName
                                                                         : juce::String(def->shortName),
                            true, chosen);
            }
            if (!defs.empty())
                menu.addSubMenu(categories[c].name, sub, true, juce::Image(), any);
        }
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(modules_),
                           [safe = juce::Component::SafePointer<PatchLibraryPanel>(this)](int id) {
                               if (safe == nullptr || id == 0)
                                   return;
                               auto& chosen = safe->model_.query.modules;
                               if (id == 1) {
                                   chosen.clear();
                               } else {
                                   const auto type = static_cast<std::uint8_t>(id - 100);
                                   if (const auto it = std::find(chosen.begin(), chosen.end(), type); it != chosen.end())
                                       chosen.erase(it);
                                   else
                                       chosen.push_back(type);
                               }
                               safe->updateModulesButton();
                               safe->queryChanged();
                           });
    }

    void showFoldersMenu()
    {
        juce::PopupMenu menu;
        for (int i = 0; i < folders_.size(); ++i) {
            const juce::File folder(folders_[i]);
            juce::PopupMenu sub;
            sub.addItem(200 + i, "Remove from the Library (the folder stays on the disk)");
            sub.addItem(300 + i, "Show the Folder", folder.isDirectory());
            menu.addSubMenu(folder.getFullPathName() + (folder.isDirectory() ? "" : "  (not found)"), sub);
        }
        if (!folders_.isEmpty())
            menu.addSeparator();
        menu.addItem(1, "Add Folder...");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(foldersButton_),
                           [safe = juce::Component::SafePointer<PatchLibraryPanel>(this)](int id) {
                               if (safe == nullptr || id == 0)
                                   return;
                               if (id == 1)
                                   safe->addFolder();
                               else if (id >= 300)
                                   juce::File(safe->folders_[id - 300]).revealToUser();
                               else if (id >= 200) {
                                   safe->folders_.remove(id - 200);
                                   safe->foldersChanged();
                               }
                           });
    }

    void addFolder()
    {
        const auto start = folders_.isEmpty() ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                              : juce::File(folders_[folders_.size() - 1]).getParentDirectory();
        chooser_ = std::make_unique<juce::FileChooser>("Add a folder of G2 patches to the library", start);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<PatchLibraryPanel>(this)](const juce::FileChooser& fc) {
                                  const auto folder = fc.getResult();
                                  if (safe == nullptr || !folder.isDirectory())
                                      return;
                                  safe->folders_.addIfNotAlreadyThere(folder.getFullPathName());
                                  safe->foldersChanged();
                              });
    }

    void foldersChanged()
    {
        store_.set(kFoldersKey, folders_.joinIntoString("\n"));
        rescan();
        repaint();
    }

    void showSlotMenu()
    {
        juce::PopupMenu menu;
        for (int s = 0; s < 4; ++s)
            menu.addItem(1 + s, "Slot " + slotLetter(s));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(slotButton_),
                           [safe = juce::Component::SafePointer<PatchLibraryPanel>(this)](int id) {
                               if (safe != nullptr && id > 0)
                                   safe->openInSlot(id - 1);
                           });
    }

    // A right-click on a file: as the original's (a slot per item for a patch, Load for a performance), without its
    // Save In Synth, and with G2fresh's own items.
    void showRowMenu(int row)
    {
        if (row < 0 || row >= model_.rowCount())
            return;
        const Entry& e = model_.entry(row);
        const bool ready = host_.synth != nullptr && host_.synth->ready();
        juce::PopupMenu menu;
        menu.addItem(1, "Open");
        if (!e.performance)
            for (int s = 0; s < 4; ++s)
                menu.addItem(10 + s, "Open in Slot " + slotLetter(s), slotsOffered(e));
        menu.addSeparator();
        menu.addItem(2, e.path == auditioned_ ? "Stop Audition" : "Audition", ready && e.readable());
        menu.addItem(3, model_.marks.favourite(e.path) ? "Remove the Star" : "Star");
        menu.addSeparator();
        menu.addItem(4, "Show the File");
        menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<PatchLibraryPanel>(this), path = e.path](int id) {
            if (safe == nullptr || id == 0)
                return;
            // The row the menu was for, wherever it is now.
            const int at = safe->model_.rowOf(path);
            if (at < 0)
                return;
            safe->model_.select(at);
            safe->rowsChanged(false);
            if (id == 1)
                safe->openSelected();
            else if (id == 2)
                safe->toggleAudition();
            else if (id == 3)
                safe->toggleStar(at);
            else if (id == 4)
                toFile(path).revealToUser();
            else if (id >= 10)
                safe->openInSlot(id - 10);
        });
    }

    LibraryHost host_;
    LibraryStore store_;
    PatchLibraryModel model_;
    Scanner scanner_;
    juce::StringArray folders_;
    bool scanning_ = false, rowsStale_ = false, updating_ = false;
    juce::uint32 lastRefresh_ = 0, pictureDue_ = 0, auditionDue_ = 0;
    std::optional<Entry> shown_; // what the detail pane shows
    bool shownStarred_ = false;
    std::string auditioned_;     // the file that plays on the synth, or ""
    juce::String message_;       // the last thing to tell, in the status line

    juce::SharedResourcePointer<theme::LookAndFeel> lookAndFeel_;
    juce::TooltipWindow tooltips_{this, 600};
    SearchField search_;
    juce::ComboBox type_, category_;
    juce::TextButton modules_;
    juce::ToggleButton starred_{"Starred"}, recent_{"Recently Opened"}, audioIn_{"Uses the Audio Inputs"},
        fits_{"Voices Fit 4 DSPs"}, unreadable_{"Unreadable"};
    Table table_;
    Detail detail_;
    juce::Label status_;
    juce::TextButton foldersButton_{"Folders"}, rescan_{"Rescan"}, audition_{"Audition"}, slotButton_{"Open in Slot"},
        open_{"Open"};
    juce::ToggleButton onSelect_{"Audition on Select"};
    std::unique_ptr<juce::FileChooser> chooser_;
    // An empty library says where patches come from (asked for after hand testing). G2fresh ships none: Clavia's
    // banks are Clavia's (Nord no longer hosts them; the Internet Archive keeps the copy), the users' are theirs.
    juce::HyperlinkButton factoryLink_{"Clavia's G2 factory bank v1.24 (a zip, from the Internet Archive's copy of nordkeyboards.com)",
                                       juce::URL("https://web.archive.org/web/2020id_/http://www.nordkeyboards.com/sites/default/files/files/"
                                                 "sound-libraries/product-libraries/nord-modular-g2/"
                                                 "Nord%20Modular%20G2%20Factory%20Bank%20v1.24%20revA.zip")};
    juce::HyperlinkButton communityLink_{"G2 patches by users (a GitHub mirror of the electro-music.com forum's archive)",
                                         juce::URL("https://github.com/DatanoiseTV/NordModularPatches")};
#if JUCE_MAC
    juce::TextButton showFolder_{"Show the Folder in the Finder"};
#else
    juce::TextButton showFolder_{"Show the Folder"};
#endif

    void updateEmptyLinks()
    {
        const bool empty = model_.index.size() == 0 && !scanning_;
        for (juce::Component* c : std::initializer_list<juce::Component*>{&factoryLink_, &communityLink_, &showFolder_})
            c->setVisible(empty);
        showFolder_.setVisible(empty && !folders_.isEmpty());
    }
};

// ---- The window ------------------------------------------------------------------------------------------------------------

PatchLibrary::PatchLibrary(LibraryHost host, LibraryStore store)
    : DocumentWindow("Patch Library", kBackground, DocumentWindow::closeButton),
      panel_(std::make_unique<PatchLibraryPanel>(std::move(host), std::move(store)))
{
    panel_->onClose = [this] { closeButtonPressed(); };
    setUsingNativeTitleBar(true);
    setContentNonOwned(panel_.get(), true);
    setResizable(true, false);
    setResizeLimits(980, 480, 4000, 3000);
    centreWithSize(getWidth(), getHeight());
}

PatchLibrary::~PatchLibrary()
{
    clearContentComponent();
}

void PatchLibrary::open()
{
    setVisible(true);
    toFront(true);
    panel_->opened();
}

void PatchLibrary::closeButtonPressed()
{
    panel_->closed();
    setVisible(false);
}

std::unique_ptr<juce::Component> makePatchLibraryPanel(LibraryHost host, LibraryStore store)
{
    return std::make_unique<PatchLibraryPanel>(std::move(host), std::move(store));
}

bool libraryScanning(juce::Component& panel)
{
    auto* p = dynamic_cast<PatchLibraryPanel*>(&panel);
    return p != nullptr && p->scanning();
}

void librarySearch(juce::Component& panel, const juce::String& words)
{
    if (auto* p = dynamic_cast<PatchLibraryPanel*>(&panel))
        p->searchFor(words);
}

void librarySelect(juce::Component& panel, const juce::String& text)
{
    if (auto* p = dynamic_cast<PatchLibraryPanel*>(&panel))
        p->selectNamed(text);
}

} // namespace g2ui
