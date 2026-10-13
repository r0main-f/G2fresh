#include "g2/library.hpp"

#include "g2/convert.hpp"
#include "g2/edit.hpp"
#include "g2/patch_load.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>

#include <sys/stat.h>

namespace g2::library {
namespace {

namespace fs = std::filesystem;

constexpr u8 kTwoIn = 170, kFourIn = 171; // the modules that take the G2's audio inputs

// Paths are kept as UTF-8 text, whatever the system's own encoding.
std::string toUtf8(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// A file's size and date (seconds since 1970); false if it cannot be asked.
bool stamp(const fs::path& p, std::uint64_t& size, std::int64_t& mtime)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_wstat64(p.c_str(), &st) != 0)
        return false;
#else
    struct stat st;
    if (::stat(p.c_str(), &st) != 0)
        return false;
#endif
    size = static_cast<std::uint64_t>(st.st_size);
    mtime = static_cast<std::int64_t>(st.st_mtime);
    return true;
}

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), lower);
    return out;
}

// Names and notes are stored one byte per character (shown as Latin-1,
// PatchDocument::fromG2Bytes); the index keeps them as UTF-8.
std::string latin1ToUtf8(std::string_view bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) {
            out += ch;
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

int tenths(float percent)
{
    return static_cast<int>(std::lround(std::clamp(percent, 0.0f, 6000.0f) * 10.0f));
}

std::uint16_t clip16(int v)
{
    return static_cast<std::uint16_t>(std::clamp(v, 0, 0xFFFF));
}

bool listensToInputs(const Module& m)
{
    if (m.type != kTwoIn && m.type != kFourIn)
        return false;
    // Source: 2-In "In 1/2, In 3/4, Bus 1/2, Bus 3/4", 4-In "In, Bus".
    const u8 lastInput = m.type == kTwoIn ? 1 : 0;
    if (m.params.empty())
        return true;
    for (std::size_t v = 0; v < m.params.size() && v < static_cast<std::size_t>(kUserVariations); ++v)
        if (m.params[v].empty() || m.params[v][0] <= lastInput)
            return true;
    return false;
}

struct PatchFacts {
    int voices = 1;
    int voiceLoad = 0, fxLoad = 0;
};

// Adds one patch (a performance's slot) to the entry.
PatchFacts addPatch(Entry& e, const Patch& p, std::map<u8, int>& counts)
{
    for (const auto loc : {Location::Va, Location::Fx}) {
        const auto& area = p.area(loc);
        for (const auto& m : area.modules) {
            ++counts[m.type];
            e.audioIn = e.audioIn || listensToInputs(m);
        }
        e.cables = clip16(e.cables + static_cast<int>(area.cables.size()));
    }
    e.vaModules = clip16(e.vaModules + static_cast<int>(p.va.modules.size()));
    e.fxModules = clip16(e.fxModules + static_cast<int>(p.fx.modules.size()));
    if (!p.notes.empty()) {
        if (!e.notes.empty())
            e.notes += "\n\n";
        e.notes += latin1ToUtf8(p.notes);
    }
    const auto load = patchload::compute(p);
    PatchFacts facts;
    facts.voices = p.header.monoMode != 0 ? 1 : std::max<int>(1, p.header.voiceCount);
    // An empty area costs nothing (compute() leaves its figures at 0).
    const int vaCycles = p.va.modules.empty() ? 0 : tenths(load.va.cycles);
    const int vaMemory = p.va.modules.empty() ? 0 : tenths(load.va.criticalPercent);
    const int fxCycles = p.fx.modules.empty() ? 0 : tenths(load.fx.cycles);
    const int fxMemory = p.fx.modules.empty() ? 0 : tenths(load.fx.criticalPercent);
    facts.voiceLoad = std::max(vaCycles, vaMemory);
    facts.fxLoad = std::max(fxCycles, fxMemory);
    // The entry shows its heaviest patch.
    if (facts.voiceLoad >= e.voiceLoad()) {
        e.vaCycles = clip16(vaCycles);
        e.vaMemory = clip16(vaMemory);
    }
    if (facts.fxLoad >= e.fxLoad()) {
        e.fxCycles = clip16(fxCycles);
        e.fxMemory = clip16(fxMemory);
    }
    return facts;
}

// ---- The cache file -------------------------------------------------------------

constexpr char kMagic[8] = {'G', '2', 'F', 'L', 'I', 'B', 0, 1};

struct Writer {
    std::string out;
    void u8v(std::uint8_t v) { out += static_cast<char>(v); }
    void u16(std::uint16_t v)
    {
        u8v(static_cast<std::uint8_t>(v));
        u8v(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v)
    {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
    }
    void u64(std::uint64_t v)
    {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32));
    }
    void str(const std::string& s)
    {
        u32(static_cast<std::uint32_t>(s.size()));
        out += s;
    }
};

struct Reader {
    std::string_view in;
    std::size_t pos = 0;
    void need(std::size_t n) const
    {
        if (n > in.size() - pos)
            throw std::runtime_error("truncated");
    }
    std::uint8_t u8v()
    {
        need(1);
        return static_cast<std::uint8_t>(in[pos++]);
    }
    std::uint16_t u16()
    {
        const auto lo = u8v();
        return static_cast<std::uint16_t>(lo | (u8v() << 8));
    }
    std::uint32_t u32()
    {
        const std::uint32_t lo = u16();
        return lo | (static_cast<std::uint32_t>(u16()) << 16);
    }
    std::uint64_t u64()
    {
        const std::uint64_t lo = u32();
        return lo | (static_cast<std::uint64_t>(u32()) << 32);
    }
    std::string str()
    {
        const auto n = u32();
        need(n);
        std::string s(in.substr(pos, n));
        pos += n;
        return s;
    }
};

void writeEntry(Writer& w, const Entry& e)
{
    w.str(e.path);
    w.str(e.folder);
    w.u64(e.size);
    w.u64(static_cast<std::uint64_t>(e.mtime));
    w.str(e.error);
    w.u8v(static_cast<std::uint8_t>((e.performance ? 1 : 0) | (e.checksumBad ? 2 : 0) | (e.audioIn ? 4 : 0)));
    w.u16(e.version);
    w.u8v(e.category);
    w.u8v(e.voices);
    w.u8v(e.monoMode);
    w.u16(static_cast<std::uint16_t>(e.modules.size()));
    for (const auto& m : e.modules) {
        w.u8v(m.type);
        w.u16(m.count);
    }
    for (const auto v : {e.vaModules, e.fxModules, e.cables, e.vaCycles, e.vaMemory, e.fxCycles, e.fxMemory})
        w.u16(v);
    w.u8v(e.voicesOn4);
    w.u8v(e.voicesOn8);
    w.str(e.notes);
    for (const auto& s : e.slotNames)
        w.str(s);
}

Entry readEntry(Reader& r)
{
    Entry e;
    e.path = r.str();
    e.folder = r.str();
    e.size = r.u64();
    e.mtime = static_cast<std::int64_t>(r.u64());
    e.error = r.str();
    const auto flags = r.u8v();
    e.performance = (flags & 1) != 0;
    e.checksumBad = (flags & 2) != 0;
    e.audioIn = (flags & 4) != 0;
    e.version = r.u16();
    e.category = r.u8v();
    e.voices = r.u8v();
    e.monoMode = r.u8v();
    e.modules.resize(r.u16());
    for (auto& m : e.modules) {
        m.type = r.u8v();
        m.count = r.u16();
    }
    for (auto* v : {&e.vaModules, &e.fxModules, &e.cables, &e.vaCycles, &e.vaMemory, &e.fxCycles, &e.fxMemory})
        *v = r.u16();
    e.voicesOn4 = r.u8v();
    e.voicesOn8 = r.u8v();
    e.notes = r.str();
    for (auto& s : e.slotNames)
        s = r.str();
    return e;
}

// ---- Scanning ---------------------------------------------------------------------

bool isPatchFile(const fs::path& p)
{
    const auto ext = lowered(toUtf8(p.extension()));
    if (ext != ".pch2" && ext != ".prf2")
        return false;
    // "._name.pch2": what macOS leaves beside a file on a disk without resource forks.
    const auto name = toUtf8(p.filename());
    return name.rfind("._", 0) != 0;
}

bool isSeparator(char c)
{
    return c == '/' || c == '\\';
}

std::string withoutTrailingSeparator(std::string s)
{
    while (s.size() > 1 && isSeparator(s.back()))
        s.pop_back();
    return s;
}

bool isBelow(const std::string& path, const std::string& root)
{
    return path.size() > root.size() + 1 && path.compare(0, root.size(), root) == 0
        && (isSeparator(path[root.size()]) || isSeparator(root.back()));
}

// "Library/Bass" for <...>/Library/Bass/x.pch2 scanned from <...>/Library.
std::string folderBelow(const std::string& path, const std::string& root)
{
    std::size_t nameStart = root.size();
    while (nameStart > 0 && !isSeparator(root[nameStart - 1]))
        --nameStart;
    std::size_t end = path.size();
    while (end > root.size() && !isSeparator(path[end - 1]))
        --end;
    std::string folder = path.substr(nameStart, end > nameStart ? end - 1 - nameStart : 0);
    std::replace(folder.begin(), folder.end(), '\\', '/');
    return folder;
}

} // namespace

// ---- Entries --------------------------------------------------------------------------

std::string Entry::name() const
{
    std::size_t start = path.size();
    while (start > 0 && !isSeparator(path[start - 1]))
        --start;
    const auto dot = path.rfind('.');
    return path.substr(start, dot != std::string::npos && dot > start ? dot - start : std::string::npos);
}

int estimateVoices(std::span<const SlotDemand> slots, int dsps)
{
    constexpr int kFull = 1000; // one DSP, in tenths of a percent
    std::vector<int> free(static_cast<std::size_t>(std::max(0, dsps)), kFull);
    auto place = [&free](int load) {
        for (auto& f : free)
            if (load <= f) {
                f -= load;
                return true;
            }
        return false;
    };
    // The FX areas first, the largest first: each runs once, whatever the voices.
    std::vector<int> fx;
    for (const auto& s : slots)
        if (s.fxLoad > 0)
            fx.push_back(s.fxLoad);
    std::sort(fx.rbegin(), fx.rend());
    for (const int load : fx)
        if (!place(load))
            return 0; // an FX area that fits no DSP: the patch does not load
    // Then the voices, one per slot in turn (the heaviest slot first), until
    // every slot has what it asked for or nothing more fits.
    std::vector<std::size_t> order(slots.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return slots[a].voiceLoad > slots[b].voiceLoad; });
    std::vector<int> given(slots.size(), 0);
    int total = 0;
    for (bool placed = true; placed;) {
        placed = false;
        for (const auto i : order) {
            const auto& s = slots[i];
            if (given[i] >= s.voices)
                continue;
            if (s.voiceLoad > 0 && !place(s.voiceLoad))
                continue;
            ++given[i];
            ++total;
            placed = true;
        }
    }
    return total;
}

Entry describe(std::span<const u8> bytes)
{
    Entry e;
    try {
        const file::File f = file::read(bytes);
        e.checksumBad = !f.crcValid;
        e.version = convert::sourceVersion(f);
        e.performance = f.type == file::FileType::Performance;
        std::map<u8, int> counts;
        std::vector<SlotDemand> demand;
        if (e.performance) {
            const Performance perf = Performance::fromFile(f);
            for (std::size_t s = 0; s < perf.slots.size(); ++s) {
                const auto facts = addPatch(e, perf.slots[s], counts);
                e.slotNames[s] = latin1ToUtf8(perf.header.slots[s].patchName);
                // A slot that is switched off plays nothing.
                if (perf.header.slots[s].enabled != 0)
                    demand.push_back({facts.voices, facts.voiceLoad, facts.fxLoad});
            }
        } else {
            const Patch patch = Patch::fromFile(f);
            e.category = patch.header.category;
            e.monoMode = patch.header.monoMode;
            const auto facts = addPatch(e, patch, counts);
            demand.push_back({facts.voices, facts.voiceLoad, facts.fxLoad});
        }
        int asked = 0;
        for (const auto& d : demand)
            asked += d.voices;
        e.voices = static_cast<u8>(std::min(asked, 255));
        e.voicesOn4 = static_cast<u8>(std::min(estimateVoices(demand, 4), 255));
        e.voicesOn8 = static_cast<u8>(std::min(estimateVoices(demand, 8), 255));
        for (const auto& [type, count] : counts)
            e.modules.push_back({type, clip16(count)});
    } catch (const std::exception& ex) {
        e = Entry{};
        e.error = ex.what();
        if (e.error.empty())
            e.error = "unreadable";
    } catch (...) {
        e = Entry{};
        e.error = "unreadable";
    }
    return e;
}

Entry describeFile(const fs::path& file)
{
    Entry e;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    const bool stamped = stamp(file, size, mtime);
    if (!stamped) {
        e.error = "cannot be read";
    } else if (size > kMaxFileSize) {
        e.error = "too large to be a patch";
    } else {
        std::vector<u8> bytes;
        try {
            std::ifstream in(file, std::ios::binary);
            if (in) {
                bytes.resize(static_cast<std::size_t>(size));
                in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
                e = describe(bytes);
            } else {
                e.error = "cannot be read";
            }
        } catch (const std::exception&) {
            e = Entry{};
            e.error = "cannot be read";
        }
    }
    e.path = toUtf8(file);
    e.size = size;
    e.mtime = mtime;
    return e;
}

// ---- The cache --------------------------------------------------------------------------

bool Cache::load(const fs::path& file)
{
    entries_.clear();
    dirty_ = false;
    try {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return false;
        const std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        Reader r{data};
        r.need(sizeof kMagic);
        if (data.compare(0, sizeof kMagic, kMagic, sizeof kMagic) != 0)
            return false;
        r.pos = sizeof kMagic;
        if (r.u32() != kIndexVersion)
            return false;
        const auto count = r.u32();
        for (std::uint32_t i = 0; i < count; ++i) {
            Entry e = readEntry(r);
            auto path = e.path;
            entries_.insert_or_assign(std::move(path), std::move(e));
        }
        return true;
    } catch (const std::exception&) {
        entries_.clear();
        return false;
    }
}

bool Cache::save(const fs::path& file) const
{
    try {
        Writer w;
        w.out.append(kMagic, sizeof kMagic);
        w.u32(kIndexVersion);
        w.u32(static_cast<std::uint32_t>(entries_.size()));
        for (const auto& [path, e] : entries_)
            writeEntry(w, e);
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        auto temp = file;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(w.out.data(), static_cast<std::streamsize>(w.out.size()));
            if (!out)
                return false;
        }
        fs::rename(temp, file, ec);
        if (ec) {
            fs::remove(temp, ec);
            return false;
        }
        dirty_ = false;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

const Entry* Cache::find(const std::string& path, std::uint64_t size, std::int64_t mtime) const
{
    const auto it = entries_.find(path);
    return it != entries_.end() && it->second.size == size && it->second.mtime == mtime ? &it->second : nullptr;
}

void Cache::put(Entry entry)
{
    auto path = entry.path;
    entries_.insert_or_assign(std::move(path), std::move(entry));
    dirty_ = true;
}

// ---- Scanning -----------------------------------------------------------------------------

ScanStats scan(std::span<const fs::path> folders, Cache& cache, ScanSink& sink)
{
    ScanStats stats;
    try {
        std::vector<std::string> roots;
        for (const auto& f : folders)
            roots.push_back(withoutTrailingSeparator(toUtf8(f)));
        auto rootOf = [&roots](const std::string& path) -> const std::string* {
            for (const auto& r : roots)
                if (isBelow(path, r))
                    return &r;
            return nullptr;
        };

        // What is known already shows at once; a folder no longer chosen is forgotten.
        {
            std::vector<Entry> known;
            std::vector<std::string> dropped;
            for (const auto& [path, e] : cache.entries()) {
                if (rootOf(path) != nullptr)
                    known.push_back(e);
                else
                    dropped.push_back(path);
            }
            for (const auto& path : dropped)
                cache.erase(path);
            if (!known.empty())
                sink.found(std::move(known));
        }

        std::unordered_set<std::string> seen;
        std::vector<std::string> walked; // the folders that could be listed
        std::vector<Entry> batch;
        auto lastFlush = std::chrono::steady_clock::now();
        auto flush = [&](bool force) {
            const auto now = std::chrono::steady_clock::now();
            if (batch.empty() || (!force && batch.size() < 256 && now - lastFlush < std::chrono::milliseconds(120)))
                return;
            sink.found(std::move(batch));
            batch.clear();
            lastFlush = now;
        };

        for (std::size_t i = 0; i < roots.size() && !stats.cancelled; ++i) {
            const auto& root = roots[i];
            std::error_code ec;
            if (!fs::is_directory(folders[i], ec))
                continue; // not there now (a disk not plugged in): what the cache knows stays
            fs::recursive_directory_iterator it(folders[i], fs::directory_options::skip_permission_denied, ec);
            if (ec)
                continue;
            walked.push_back(root);
            for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec)
                    break;
                if (sink.cancelled()) {
                    stats.cancelled = true;
                    break;
                }
                std::error_code entryError;
                if (!it->is_regular_file(entryError) || entryError || !isPatchFile(it->path()))
                    continue;
                const auto path = toUtf8(it->path());
                if (!seen.insert(path).second)
                    continue; // a folder chosen twice, or inside another one
                ++stats.files;
                const auto folder = folderBelow(path, root);
                std::uint64_t size = 0;
                std::int64_t mtime = 0;
                const bool stamped = stamp(it->path(), size, mtime);
                if (const Entry* hit = stamped ? cache.find(path, size, mtime) : nullptr) {
                    ++stats.reused;
                    stats.unreadable += hit->readable() ? 0 : 1;
                    if (hit->folder != folder) { // seen from another chosen folder now
                        Entry moved = *hit;
                        moved.folder = folder;
                        batch.push_back(moved);
                        cache.put(std::move(moved));
                    }
                } else {
                    Entry e = describeFile(it->path());
                    e.folder = folder;
                    ++stats.read;
                    stats.unreadable += e.readable() ? 0 : 1;
                    batch.push_back(e);
                    cache.put(std::move(e));
                }
                flush(false);
            }
        }
        flush(true);

        // Files gone from a folder that was listed completely.
        if (!stats.cancelled) {
            std::vector<std::string> gone;
            for (const auto& [path, e] : cache.entries())
                if (!seen.count(path)
                    && std::any_of(walked.begin(), walked.end(), [&](const std::string& r) { return isBelow(path, r); }))
                    gone.push_back(path);
            for (const auto& path : gone)
                cache.erase(path);
            stats.removed = gone.size();
            if (!gone.empty())
                sink.removed(std::move(gone));
        }
    } catch (const std::exception&) {
        // A folder that fails half-way: what was found stays.
    }
    return stats;
}

// ---- Marks ------------------------------------------------------------------------------------

namespace {

std::vector<std::string> lines(std::string_view text)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        auto line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (!line.empty())
            out.emplace_back(line);
        start = end + 1;
    }
    return out;
}

std::string joined(const std::vector<std::string>& v)
{
    std::string out;
    for (const auto& s : v) {
        if (!out.empty())
            out += '\n';
        out += s;
    }
    return out;
}

} // namespace

void Marks::setFavourite(const std::string& path, bool on)
{
    if (on)
        favourites.insert(path);
    else
        favourites.erase(path);
}

int Marks::recentRank(const std::string& path) const
{
    const auto it = std::find(recent.begin(), recent.end(), path);
    return it == recent.end() ? -1 : static_cast<int>(it - recent.begin());
}

void Marks::opened(const std::string& path)
{
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > kMaxRecent)
        recent.resize(kMaxRecent);
}

std::string Marks::favouritesText() const
{
    std::vector<std::string> sorted(favourites.begin(), favourites.end());
    std::sort(sorted.begin(), sorted.end());
    return joined(sorted);
}

std::string Marks::recentText() const
{
    return joined(recent);
}

void Marks::setFavouritesText(std::string_view text)
{
    favourites.clear();
    for (auto& l : lines(text))
        favourites.insert(std::move(l));
}

void Marks::setRecentText(std::string_view text)
{
    recent = lines(text);
    if (recent.size() > kMaxRecent)
        recent.resize(kMaxRecent);
}

// ---- The list ---------------------------------------------------------------------------------

namespace {

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

std::string searchText(const Entry& e)
{
    std::string t = e.name();
    t += '\n';
    t += e.folder;
    if (e.readable()) {
        if (!e.performance) {
            t += '\n';
            t += edit::categoryName(e.category);
        }
        for (const auto& s : e.slotNames) {
            t += '\n';
            t += s;
        }
        t += '\n';
        t += e.notes;
    } else {
        t += '\n';
        t += e.error;
    }
    return lowered(t);
}

// The module types whose short or long name contains `word` (lower case).
std::bitset<256> modulesNamed(const std::string& word)
{
    std::bitset<256> types;
    for (const auto& def : db::modules()) {
        if (def.kind != db::ModuleKind::Module)
            continue;
        if (lowered(def.shortName).find(word) != std::string::npos
            || (def.longName != nullptr && lowered(def.longName).find(word) != std::string::npos))
            types.set(def.typeId);
    }
    return types;
}

bool hasModule(const Entry& e, u8 type)
{
    const auto it = std::lower_bound(e.modules.begin(), e.modules.end(), type,
                                     [](const ModuleCount& m, u8 t) { return m.type < t; });
    return it != e.modules.end() && it->type == type;
}

} // namespace

int compareNatural(std::string_view a, std::string_view b)
{
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            std::size_t ie = i, je = j;
            while (ie < a.size() && isDigit(a[ie]))
                ++ie;
            while (je < b.size() && isDigit(b[je]))
                ++je;
            // Compare the numbers without their leading zeros: the longer is larger.
            auto na = a.substr(i, ie - i), nb = b.substr(j, je - j);
            while (na.size() > 1 && na.front() == '0')
                na.remove_prefix(1);
            while (nb.size() > 1 && nb.front() == '0')
                nb.remove_prefix(1);
            if (na.size() != nb.size())
                return na.size() < nb.size() ? -1 : 1;
            if (const int c = na.compare(nb); c != 0)
                return c < 0 ? -1 : 1;
            i = ie;
            j = je;
            continue;
        }
        const char ca = lower(a[i]), cb = lower(b[j]);
        if (ca != cb)
            return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb) ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size())
        return 1;
    if (j < b.size())
        return -1;
    return 0;
}

void Index::clear()
{
    entries_.clear();
    text_.clear();
    byPath_.clear();
}

void Index::upsert(Entry entry)
{
    auto text = searchText(entry);
    if (const auto it = byPath_.find(entry.path); it != byPath_.end()) {
        text_[it->second] = std::move(text);
        entries_[it->second] = std::move(entry);
        return;
    }
    byPath_.emplace(entry.path, entries_.size());
    entries_.push_back(std::move(entry));
    text_.push_back(std::move(text));
}

void Index::remove(const std::string& path)
{
    const auto it = byPath_.find(path);
    if (it == byPath_.end())
        return;
    const std::size_t i = it->second, last = entries_.size() - 1;
    byPath_.erase(it);
    if (i != last) {
        entries_[i] = std::move(entries_[last]);
        text_[i] = std::move(text_[last]);
        byPath_[entries_[i].path] = i;
    }
    entries_.pop_back();
    text_.pop_back();
}

const Entry* Index::find(const std::string& path) const
{
    const auto it = byPath_.find(path);
    return it == byPath_.end() ? nullptr : &entries_[it->second];
}

std::size_t Index::unreadableCount() const
{
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(), [](const Entry& e) { return !e.readable(); }));
}

std::vector<std::size_t> Index::search(const Query& query, const Marks& marks, SortKey key, bool descending) const
{
    struct Word {
        std::string text;
        std::bitset<256> modules;
    };
    std::vector<Word> words;
    {
        const auto text = lowered(query.text);
        std::size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
                ++i;
            std::size_t end = i;
            while (end < text.size() && text[end] != ' ' && text[end] != '\t')
                ++end;
            if (end > i) {
                Word w{text.substr(i, end - i), {}};
                w.modules = modulesNamed(w.text);
                words.push_back(std::move(w));
            }
            i = end;
        }
    }

    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (e.readable() == query.unreadable)
            continue;
        if (query.type != TypeFilter::All && (query.type == TypeFilter::Performances) != e.performance)
            continue;
        if (query.category >= 0 && (e.performance || e.category != query.category))
            continue;
        if (query.audioIn && !e.audioIn)
            continue;
        if (query.fitsUnexpanded && !e.fitsUnexpanded())
            continue;
        if (query.favourites && !marks.favourite(e.path))
            continue;
        if (query.recent && marks.recentRank(e.path) < 0)
            continue;
        if (!std::all_of(query.modules.begin(), query.modules.end(), [&e](u8 t) { return hasModule(e, t); }))
            continue;
        const bool found = std::all_of(words.begin(), words.end(), [&](const Word& w) {
            if (text_[i].find(w.text) != std::string::npos)
                return true;
            return std::any_of(e.modules.begin(), e.modules.end(),
                               [&w](const ModuleCount& m) { return w.modules.test(m.type); });
        });
        if (found)
            rows.push_back(i);
    }

    // The value a column sorts by; the name, then the path, settle ties.
    std::vector<std::string> names(entries_.size());
    for (const auto i : rows)
        names[i] = entries_[i].name();
    auto primary = [&](std::size_t a, std::size_t b) -> int {
        const Entry& x = entries_[a];
        const Entry& y = entries_[b];
        auto cmp = [](auto p, auto q) { return p < q ? -1 : p > q ? 1 : 0; };
        switch (key) {
        case SortKey::Name: return compareNatural(names[a], names[b]);
        case SortKey::Type: return cmp(x.performance, y.performance);
        case SortKey::Category: {
            // By name, as shown; performances (no category) last.
            if (x.performance != y.performance)
                return cmp(x.performance, y.performance);
            return x.performance ? 0 : compareNatural(edit::categoryName(x.category), edit::categoryName(y.category));
        }
        case SortKey::Voices: return cmp(x.voices, y.voices);
        case SortKey::Modules: return cmp(x.moduleTotal(), y.moduleTotal());
        case SortKey::Load: return cmp(x.voiceLoad(), y.voiceLoad());
        case SortKey::Version: return cmp(x.version, y.version);
        case SortKey::Date: return cmp(x.mtime, y.mtime);
        case SortKey::Folder: return compareNatural(x.folder, y.folder);
        case SortKey::Favourite: return cmp(!marks.favourite(x.path), !marks.favourite(y.path));
        case SortKey::Recent: {
            const int rx = marks.recentRank(x.path), ry = marks.recentRank(y.path);
            return cmp(rx < 0 ? 1 << 20 : rx, ry < 0 ? 1 << 20 : ry);
        }
        }
        return 0;
    };
    std::sort(rows.begin(), rows.end(), [&](std::size_t a, std::size_t b) {
        if (const int c = primary(a, b); c != 0)
            return descending ? c > 0 : c < 0;
        if (const int c = compareNatural(names[a], names[b]); c != 0)
            return c < 0;
        return entries_[a].path < entries_[b].path;
    });
    return rows;
}

std::string voicesText(const Entry& e)
{
    if (!e.performance && e.monoMode == 1)
        return "Mono";
    if (!e.performance && e.monoMode == 2)
        return "Legato";
    return std::to_string(e.voices);
}

std::string moduleSummary(const Entry& e)
{
    std::vector<ModuleCount> order = e.modules;
    auto rank = [](u8 type) {
        const auto* def = db::find(type);
        return def == nullptr ? std::pair<int, int>{1000, type} : std::pair<int, int>{def->category, def->browserOrder};
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](const ModuleCount& a, const ModuleCount& b) { return rank(a.type) < rank(b.type); });
    std::string out;
    for (const auto& m : order) {
        const auto* def = db::find(m.type);
        if (!out.empty())
            out += ", ";
        out += def != nullptr ? std::string(def->shortName) : "type " + std::to_string(m.type);
        if (m.count > 1)
            out += " x" + std::to_string(m.count);
    }
    return out;
}

} // namespace g2::library
