// g2tool: command-line checks with the editor's own loader.
//
//   g2tool check <file|dir>...   load every .pch2/.prf2 (recursively for dirs)
//                                exactly like the editor, report failures and
//                                whether saving reproduces the file
//   g2tool diff <file>           show which sections change when re-saved
#include "g2/file.hpp"
#include "g2/patch.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> readBytes(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool isG2File(const fs::path& p)
{
    const auto ext = lower(p.extension().string());
    return ext == ".pch2" || ext == ".prf2";
}

// Returns "" if the file loads, otherwise the reason.
std::string check(const fs::path& path, bool& roundTrips)
{
    roundTrips = false;
    const auto bytes = readBytes(path);
    if (bytes.empty())
        return "empty file";
    try {
        // Like the editor: the file's content decides, not its extension.
        const auto loaded = g2::load(bytes);
        if (const auto* perf = std::get_if<g2::Performance>(&loaded.content))
            roundTrips = g2::savePerformance(*perf) == bytes;
        else
            roundTrips = g2::savePatch(std::get<g2::Patch>(loaded.content)) == bytes;
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}

} // namespace

// Prints the sections where saving the loaded model differs from the file.
int diff(const fs::path& path)
{
    const auto bytes = readBytes(path);
    const auto original = g2::file::read(bytes);
    const auto again = lower(path.extension().string()) == ".prf2"
        ? g2::Performance::fromFile(original).toFile() : g2::Patch::fromFile(original).toFile();
    const auto n = std::max(original.sections.size(), again.sections.size());
    int diffs = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto a = i < original.sections.size() ? g2::file::encodeSection(original.sections[i]) : std::vector<std::uint8_t>{};
        const auto b = i < again.sections.size() ? g2::file::encodeSection(again.sections[i]) : std::vector<std::uint8_t>{};
        if (a == b)
            continue;
        std::size_t at = 0;
        while (at < a.size() && at < b.size() && a[at] == b[at])
            ++at;
        std::printf("section %zu (id %02X): file %zu bytes, saved %zu bytes, first difference at byte %zu\n", i,
                    i < original.sections.size() ? original.sections[i].id : again.sections[i].id, a.size(), b.size(), at);
        ++diffs;
    }
    if (original.textHeader != again.textHeader)
        std::printf("text header differs\n"), ++diffs;
    return diffs ? 1 : 0;
}

int main(int argc, char* argv[])
{
    if (argc == 3 && std::string(argv[1]) == "diff")
        return diff(argv[2]);
    if (argc < 3 || std::string(argv[1]) != "check") {
        std::cerr << "usage: g2tool check <file|dir>...\n";
        return 2;
    }
    std::vector<fs::path> files;
    for (int i = 2; i < argc; ++i) {
        const fs::path p(argv[i]);
        if (fs::is_directory(p)) {
            for (const auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied))
                if (e.is_regular_file() && isG2File(e.path()))
                    files.push_back(e.path());
        } else {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    std::map<std::string, std::vector<fs::path>> failures;
    std::size_t ok = 0, exact = 0;
    for (const auto& f : files) {
        bool roundTrips = false;
        const auto reason = check(f, roundTrips);
        if (reason.empty()) {
            ++ok;
            if (roundTrips)
                ++exact;
            else
                failures["loads, but saving changes bytes"].push_back(f);
        } else {
            failures[reason].push_back(f);
        }
    }
    std::cout << files.size() << " files: " << ok << " load, " << exact << " save byte-identical\n";
    for (const auto& [reason, list] : failures) {
        std::cout << "\n[" << list.size() << "] " << reason << "\n";
        for (std::size_t i = 0; i < list.size() && i < 5; ++i)
            std::cout << "    " << list[i].string() << "\n";
    }
    return ok == files.size() ? 0 : 1;
}
