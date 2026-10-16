// The Patch Library window (PatchLibrary.h) as values, without a window: the index of the user's folders
// (g2/library.hpp), the search and the filters, the sorted rows, the selection, and the texts the list and the detail
// pane show. Plain C++, so the tests check it; PatchLibrary.cpp shows it.
//
// What follows the original's Patch Browser, Disk tab (CHardDriveView, manual p111): a click only selects
// (CListView::ItemClicked 0x52e8), a double-click or Return opens the file as File > Open does
// (CHardDriveView::ItemSecondClickedAction 0x968a -> CPatchFilePort::LoadComplete 0x6cbce), Up and Down move the
// selection (CListView::HandleNavigationKey 0x50c6), names sort without case (CHardDriveView::SortLess 0x981a). The
// original lists one folder level by file name and nothing else: the index, the columns, the search, the filters, the
// stars and the audition are G2fresh's.
#pragma once

#include "g2/edit.hpp"
#include "g2/library.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace g2ui {

// The default folder: G2fresh/Patches in the user's Music folder, on every system (where music apps keep their users'
// files; the user's choice, 2026-10-16). `musicDir` is the system's Music folder; its separator is kept.
inline std::string defaultPatchFolder(const std::string& musicDir)
{
    const char sep = musicDir.find('\\') != std::string::npos && musicDir.find('/') == std::string::npos ? '\\' : '/';
    std::string dir = musicDir;
    if (!dir.empty() && dir.back() != sep)
        dir += sep;
    return dir + "G2fresh" + sep + "Patches";
}

// The folders the library starts with: the ones chosen before; on a first use (none chosen, and the default never
// added), the default folder (defaultPatchFolder, which the caller creates). Once added, it is not added again: a
// list the user emptied stays empty.
struct StartingFolders {
    std::vector<std::string> folders;
    bool addedDefault = false;
};
inline StartingFolders startingFolders(const std::vector<std::string>& stored, bool defaultAddedBefore,
                                       const std::string& defaultFolder)
{
    if (!stored.empty() || defaultAddedBefore)
        return {stored, false};
    return {{defaultFolder}, true};
}

class PatchLibraryModel {
public:
    using Entry = g2::library::Entry;
    using SortKey = g2::library::SortKey;

    enum class Column { Favourite = 1, Name, Type, Category, Voices, Modules, Load, Version, Date, Folder };

    g2::library::Index index;
    g2::library::Marks marks;
    g2::library::Query query;

    // ---- Rows --------------------------------------------------------------------------------------------------
    // The rows again after the index, the query, the marks or the sort changed. The selected file stays selected if
    // it is still listed; else the row at its place is (the list does not jump to the top).
    void refresh()
    {
        const int oldRow = selectedRow();
        rows_ = index.search(query, marks, sortKey_, descending_);
        if (rowOf(selected_) < 0)
            selected_ = rows_.empty() ? std::string() : entry(std::clamp(oldRow, 0, rowCount() - 1)).path;
    }
    int rowCount() const { return static_cast<int>(rows_.size()); }
    const Entry& entry(int row) const { return index.entries()[rows_[static_cast<std::size_t>(row)]]; }
    int rowOf(const std::string& path) const
    {
        if (path.empty())
            return -1;
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (index.entries()[rows_[i]].path == path)
                return static_cast<int>(i);
        return -1;
    }

    // ---- Selection: one file, as the original's list ----------------------------------------------------------------
    int selectedRow() const { return rowOf(selected_); }
    const Entry* selected() const
    {
        const int row = selectedRow();
        return row < 0 ? nullptr : &entry(row);
    }
    void select(int row) { selected_ = row >= 0 && row < rowCount() ? entry(row).path : std::string(); }
    void selectPath(const std::string& path) { selected_ = path; }
    // Up and Down: one row, stopping at the ends (CListView::StepItem); from no selection, the first row.
    void step(int delta)
    {
        if (rows_.empty())
            return;
        const int row = selectedRow();
        select(row < 0 ? 0 : std::clamp(row + delta, 0, rowCount() - 1));
    }

    // ---- Sorting ---------------------------------------------------------------------------------------------------
    SortKey sortKey() const { return sortKey_; }
    bool descending() const { return descending_; }
    // A column's header clicked: `forwards` is the column's usual direction (names A to Z; the newest, the largest,
    // the most first), which a first click gives and the next one turns around.
    void sortBy(Column c, bool forwards)
    {
        sortKey_ = keyFor(c);
        descending_ = startsDescending(sortKey_) == forwards;
        refresh();
    }
    static SortKey keyFor(Column c)
    {
        switch (c) {
        case Column::Favourite: return SortKey::Favourite;
        case Column::Name: return SortKey::Name;
        case Column::Type: return SortKey::Type;
        case Column::Category: return SortKey::Category;
        case Column::Voices: return SortKey::Voices;
        case Column::Modules: return SortKey::Modules;
        case Column::Load: return SortKey::Load;
        case Column::Version: return SortKey::Version;
        case Column::Date: return SortKey::Date;
        case Column::Folder: return SortKey::Folder;
        }
        return SortKey::Name;
    }
    static bool startsDescending(SortKey key)
    {
        return key == SortKey::Date || key == SortKey::Modules || key == SortKey::Load || key == SortKey::Voices;
    }

    // ---- Marks -----------------------------------------------------------------------------------------------------
    // Stars or unstars a row's file; the rows follow (it may leave a "favourites only" list).
    void toggleFavourite(int row)
    {
        if (row < 0 || row >= rowCount())
            return;
        const auto path = entry(row).path;
        marks.setFavourite(path, !marks.favourite(path));
        refresh();
    }
    void opened(const std::string& path)
    {
        marks.opened(path);
        if (query.recent || sortKey_ == SortKey::Recent)
            refresh();
    }

    // ---- Texts -------------------------------------------------------------------------------------------------------
    static std::string percent(int tenths)
    {
        return std::to_string((tenths + 5) / 10) + "%";
    }
    static std::string date(std::int64_t mtime)
    {
        const auto t = static_cast<std::time_t>(mtime);
        std::tm tm{};
#ifdef _WIN32
        if (localtime_s(&tm, &t) != 0)
            return {};
#else
        if (localtime_r(&t, &tm) == nullptr)
            return {};
#endif
        char text[16];
        std::snprintf(text, sizeof text, "%04d-%02d-%02d", (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100,
                      tm.tm_mday % 100);
        return text;
    }
    static std::string cell(const Entry& e, Column c)
    {
        if (c == Column::Name)
            return e.name();
        if (c == Column::Folder)
            return e.folder;
        if (c == Column::Date)
            return date(e.mtime);
        if (!e.readable())
            return c == Column::Type ? "Unreadable" : std::string();
        switch (c) {
        case Column::Type: return e.performance ? "Perf" : "Patch";
        case Column::Category: return e.performance ? std::string() : g2::edit::categoryName(e.category);
        case Column::Voices: return g2::library::voicesText(e);
        case Column::Modules: return std::to_string(e.moduleTotal());
        case Column::Load: return e.moduleTotal() == 0 ? std::string() : percent(e.voiceLoad());
        case Column::Version: return std::to_string(e.version);
        case Column::Favourite: // a star, drawn
        case Column::Name:
        case Column::Date:
        case Column::Folder: break;
        }
        return {};
    }

    // The detail pane's facts about a file, one per line.
    static std::vector<std::string> facts(const Entry& e)
    {
        std::vector<std::string> lines;
        if (!e.readable()) {
            lines.push_back("Cannot be read: " + e.error + ".");
            lines.push_back("The file is listed as it is; G2fresh never changes the files of the library.");
            return lines;
        }
        if (e.performance) {
            std::string slots;
            for (std::size_t s = 0; s < e.slotNames.size(); ++s) {
                if (e.slotNames[s].empty())
                    continue;
                slots += slots.empty() ? "" : ", ";
                slots += static_cast<char>('A' + s);
                slots += " " + e.slotNames[s];
            }
            lines.push_back("Performance" + (slots.empty() ? std::string() : ": " + slots));
        } else {
            const auto voices = g2::library::voicesText(e);
            lines.push_back(std::string("Patch, ") + g2::edit::categoryName(e.category) + ", "
                            + (e.monoMode != 0 ? voices : voices + (e.voices == 1 ? " voice" : " voices")));
        }
        lines.push_back(std::to_string(e.moduleTotal()) + " modules (" + std::to_string(e.vaModules) + " in the voice area, "
                        + std::to_string(e.fxModules) + " in the FX area), " + std::to_string(e.cables) + " cables");
        if (e.moduleTotal() != 0) {
            std::string load = "Load, estimated: one voice " + percent(e.vaCycles) + " cycles, " + percent(e.vaMemory) + " memory";
            if (e.usesFx())
                load += "; FX " + percent(e.fxCycles) + " cycles, " + percent(e.fxMemory) + " memory";
            lines.push_back(load + (e.performance ? " (the heaviest slot)" : ""));
            // The manual's rule (p121) on that estimate.
            if (e.voicesOn4 == 0)
                lines.push_back("Too large for a G2 by this estimate");
            else if (e.fitsUnexpanded())
                lines.push_back((e.voices == 1 ? std::string("Its voice fits") : "Its " + voiceCount(e.voices) + " fit")
                                + " a G2 without expansion (4 DSPs), by this estimate");
            else
                lines.push_back("A G2 without expansion (4 DSPs) would play about " + std::to_string(e.voicesOn4) + " of its "
                                + voiceCount(e.voices) + (e.voicesOn8 > e.voicesOn4
                                                              ? ", an expanded one " + std::to_string(e.voicesOn8)
                                                              : std::string()));
        }
        if (e.audioIn)
            lines.push_back("Listens to the G2's audio inputs");
        if (e.converted())
            lines.push_back("Format version " + std::to_string(e.version) + ": converted when it is opened");
        if (e.checksumBad)
            lines.push_back("Its checksum is wrong: Open asks first");
        return lines;
    }

    // "5,578 files" style counts for the status line: "312 of 5578 files", "5578 files", with what is still going on.
    std::string status(bool scanning) const
    {
        const std::size_t unreadable = index.unreadableCount();
        const std::size_t readable = index.size() - unreadable;
        std::string s;
        if (query.unreadable)
            s = std::to_string(rowCount()) + (rowCount() == 1 ? " file" : " files") + " that cannot be read";
        else if (static_cast<std::size_t>(rowCount()) == readable)
            s = std::to_string(readable) + (readable == 1 ? " file" : " files");
        else
            s = std::to_string(rowCount()) + " of " + std::to_string(readable) + " files";
        if (!query.unreadable && unreadable != 0)
            s += ", " + std::to_string(unreadable) + " unreadable";
        if (scanning)
            s += " (reading the folders...)";
        return s;
    }

private:
    static std::string voiceCount(int n) { return std::to_string(n) + (n == 1 ? " voice" : " voices"); }

    std::vector<std::size_t> rows_;
    std::string selected_;
    SortKey sortKey_ = SortKey::Name;
    bool descending_ = false;
};

} // namespace g2ui
