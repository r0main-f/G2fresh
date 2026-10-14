// The patch library: an index of the patch (.pch2) and performance (.prf2)
// files in folders the user chose, for G2fresh's library browser. A G2fresh
// addition (the original's browser lists one folder by file name only).
//
// describe() turns a file's bytes into an Entry (what the list, the filters and
// the search need) without ever throwing: a damaged file becomes an entry that
// says why. scan() walks the folders, reusing a Cache keyed by path, size and
// date so that only new or changed files are read again. Files are only ever
// read. Index, Query and search() are the list as the window shows it. No
// threads here: the caller runs scan() where it wants (ui/PatchLibrary.cpp runs
// it on a background thread).
#pragma once

#include "g2/patch.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace g2::library {

// Bumped whenever describe() extracts something else, so that old caches are
// read again from the files.
inline constexpr std::uint32_t kIndexVersion = 1;
// Larger files are not patches (the largest known performance is under 100 KB).
inline constexpr std::uint64_t kMaxFileSize = 4u << 20;
inline constexpr u8 kNoCategory = 0xFF; // a performance has none

struct ModuleCount {
    u8 type = 0;
    std::uint16_t count = 0;
    bool operator==(const ModuleCount&) const = default;
};

struct Entry {
    // The file: where it is, and what the cache checks before trusting itself.
    std::string path;   // UTF-8, as the scan found it
    std::string folder; // its folder below the scanned one, '/'-separated, starting with that folder's name
    std::uint64_t size = 0;
    std::int64_t mtime = 0; // seconds since 1970

    // Non-empty: the file could not be read as a patch or performance (why);
    // nothing below is meaningful then.
    std::string error;

    bool performance = false;
    bool checksumBad = false;      // readable, but its checksum is wrong (Open asks first)
    std::uint16_t version = 0;     // the file's format version; below 23, it is converted when opened
    u8 category = kNoCategory;     // a patch's category (g2::edit::categoryName)
    u8 voices = 0;                 // requested voices: a patch's, or all slots' of a performance
    u8 monoMode = 0;               // a patch's: 0 poly, 1 mono, 2 legato
    std::vector<ModuleCount> modules; // VA + FX (of every slot), by type
    std::uint16_t vaModules = 0, fxModules = 0, cables = 0;
    bool audioIn = false;          // an In module listens to the G2's audio inputs
    // G2fresh's load estimate (g2::patchload::compute), in tenths of a percent
    // of one DSP: one voice, and the FX area. A performance: its heaviest slot.
    std::uint16_t vaCycles = 0, vaMemory = 0, fxCycles = 0, fxMemory = 0;
    // Voices the G2 would give by the manual's rule (estimateVoices), on 4 DSPs
    // and on 8 (the expansion board, or a G2X).
    u8 voicesOn4 = 0, voicesOn8 = 0;
    std::string notes;                     // the patch notes (every slot's, for a performance)
    std::array<std::string, 4> slotNames;  // a performance's patch names

    bool readable() const { return error.empty(); }
    bool converted() const { return readable() && version != file::kCurrentVersion; }
    bool usesFx() const { return fxModules != 0; }
    bool fitsUnexpanded() const { return readable() && voicesOn4 >= voices; }
    int moduleTotal() const { return vaModules + fxModules; }
    // The worse of the cycles and the memory of one voice, in tenths of a percent.
    int voiceLoad() const { return vaCycles > vaMemory ? vaCycles : vaMemory; }
    int fxLoad() const { return fxCycles > fxMemory ? fxCycles : fxMemory; }
    // A patch is named by its file, as on the G2.
    std::string name() const;
    bool operator==(const Entry&) const = default;
};

// What a file holds. Never throws: unreadable bytes give an entry with
// `error` set. path, folder, size and mtime are left for the caller.
Entry describe(std::span<const u8> bytes);
// The same for a file on disk (path, size and date filled in; `folder` is the
// caller's). A file that cannot be opened is an entry with `error` too.
Entry describeFile(const std::filesystem::path& file);

// How many voices fit in `dsps` DSPs, by the manual's rule (v1.2 p121): a voice
// runs whole in one DSP, as many per DSP as its load (the larger of cycles and
// memory) fits in 100 %; the FX area takes its share of one DSP. `voiceLoad`
// and `fxLoad` in tenths of a percent. An estimate: the load figures are
// G2fresh's own sum, and how the OS really places voices is not known.
struct SlotDemand {
    int voices = 0, voiceLoad = 0, fxLoad = 0;
};
int estimateVoices(std::span<const SlotDemand> slots, int dsps);

// ---- The cache ---------------------------------------------------------------

class Cache {
public:
    // Reads a cache file; false (and an empty cache) if there is none, it is
    // from another kIndexVersion, or it is damaged.
    bool load(const std::filesystem::path& file);
    // Writes it through a temporary file beside it.
    bool save(const std::filesystem::path& file) const;

    // The entry of a file unchanged since it was described, else nullptr.
    const Entry* find(const std::string& path, std::uint64_t size, std::int64_t mtime) const;
    void put(Entry entry);
    void erase(const std::string& path) { dirty_ = entries_.erase(path) != 0 || dirty_; }
    std::size_t size() const { return entries_.size(); }
    const std::unordered_map<std::string, Entry>& entries() const { return entries_; }
    // Changed since load() or the last save().
    bool dirty() const { return dirty_; }

private:
    std::unordered_map<std::string, Entry> entries_;
    mutable bool dirty_ = false; // save() is const
};

// ---- Scanning -----------------------------------------------------------------

struct ScanStats {
    std::size_t files = 0;      // patch and performance files found
    std::size_t reused = 0;     // taken from the cache
    std::size_t read = 0;       // read from disk
    std::size_t unreadable = 0; // of all files
    std::size_t removed = 0;    // were in the cache, no longer on disk
    bool cancelled = false;
};

struct ScanSink {
    virtual ~ScanSink() = default;
    // Entries to show, replacing any with the same path: first everything the
    // cache has below the folders (so a known library shows at once), then
    // what the walk finds new or changed.
    virtual void found(std::vector<Entry> entries) = 0;
    // Files the cache knew that are gone.
    virtual void removed(std::vector<std::string> paths) = 0;
    // Asked between files; true stops the scan.
    virtual bool cancelled() { return false; }
};

// Walks `folders` and their subfolders for .pch2 and .prf2 files (any case).
// The cache is brought up to date: new and changed files described, files gone
// from a folder that could be walked forgotten (a folder that is missing, e.g.
// a disk not plugged in, keeps its entries). Never throws.
ScanStats scan(std::span<const std::filesystem::path> folders, Cache& cache, ScanSink& sink);

// ---- The list ------------------------------------------------------------------

// The user's marks: starred files and those opened lately. Kept in G2fresh's
// settings, never in the files.
struct Marks {
    static constexpr std::size_t kMaxRecent = 50;
    std::unordered_set<std::string> favourites;
    std::vector<std::string> recent; // the last opened first

    bool favourite(const std::string& path) const { return favourites.count(path) != 0; }
    void setFavourite(const std::string& path, bool on);
    int recentRank(const std::string& path) const; // 0 = the last opened, -1 = not recent
    void opened(const std::string& path);
    // One path per line.
    std::string favouritesText() const;
    std::string recentText() const;
    void setFavouritesText(std::string_view text);
    void setRecentText(std::string_view text);
};

enum class TypeFilter : u8 { All, Patches, Performances };

struct Query {
    // Words, all of which must be found (case ignored) in the file's name, its
    // folder, its notes, a slot's patch name, its category or the name of a
    // module it contains ("vocoder" finds the patches with a Vocoder).
    std::string text;
    TypeFilter type = TypeFilter::All;
    int category = -1;            // -1: any
    std::vector<u8> modules;      // contains every one of these module types
    bool audioIn = false;         // only those listening to the audio inputs
    bool fitsUnexpanded = false;  // only those whose voices fit 4 DSPs (estimate)
    bool favourites = false;
    bool recent = false;
    bool unreadable = false;      // only the files that could not be read
    bool operator==(const Query&) const = default;
};

enum class SortKey : u8 { Name, Type, Category, Voices, Modules, Load, Version, Date, Folder, Favourite, Recent };

class Index {
public:
    void clear();
    // Adds an entry, or replaces the one with its path.
    void upsert(Entry entry);
    void remove(const std::string& path);
    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* find(const std::string& path) const;
    std::size_t size() const { return entries_.size(); }

    // The entries matching `query`, sorted (ties by name, then path): indices
    // into entries(). Unreadable files only show with query.unreadable.
    std::vector<std::size_t> search(const Query& query, const Marks& marks, SortKey key = SortKey::Name,
                                    bool descending = false) const;
    std::size_t unreadableCount() const;

private:
    std::vector<Entry> entries_;
    std::vector<std::string> text_; // per entry: what the words are looked for in, lower case
    std::unordered_map<std::string, std::size_t> byPath_;
};

// "8", "Mono", "Legato"; a performance's total.
std::string voicesText(const Entry& e);
// "OscB x3, FltClassic, EnvADSR x2, ..." in the module browser's order.
std::string moduleSummary(const Entry& e);
// Case-insensitive comparison that orders "Bass 2" before "Bass 10".
int compareNatural(std::string_view a, std::string_view b);

} // namespace g2::library
