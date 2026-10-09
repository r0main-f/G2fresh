#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace g2test {

inline std::vector<std::uint8_t> readBytes(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

inline std::string readText(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Patch files to test: the committed corpus, Verhue's sample patch, and any
// directory listed in the G2_EXTRA_CORPUS environment variable (for private
// patches that are not committed).
inline std::vector<std::filesystem::path> corpusFiles()
{
    namespace fs = std::filesystem;
    std::vector<fs::path> dirs{G2_CORPUS_DIR "/pch2csd"};
    if (const char* extra = std::getenv("G2_EXTRA_CORPUS"))
        dirs.emplace_back(extra);
    std::vector<fs::path> files;
    for (const auto& d : dirs) {
        if (!fs::is_directory(d))
            continue;
        for (const auto& e : fs::directory_iterator(d)) {
            const auto ext = e.path().extension().string();
            if (ext == ".pch2" || ext == ".prf2")
                files.push_back(e.path());
        }
    }
    const fs::path verhue = G2_SOURCE_DIR "/third_party/nord_g2_editor/Gen3/Patch/hi_hat_machine.pch2";
    if (fs::exists(verhue))
        files.push_back(verhue);
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace g2test
