// Reading the user's own copy of Clavia's free download for the Mac, "Nord Modular G2 OS v1.62 Update.dmg": the
// Apple UDIF disk image, the classic Mac OS resource files on it (each app's resource fork, stored as a data file
// next to it: "Nord Modular G2 OS Update.app/Contents/Resources/Nord Modular G2 Updater.rsrc" and
// "Nord Modular G2 Editor.app/Contents/Resources/Nord Modular G2 Editor.rsrc"). The emulator takes the G2 OS from
// the updater's file (emu/g2emu), the Classic look takes the original editor's graphics from the editor's (skin.hpp).
//
// Nothing from Clavia is in this code. The bytes come from the user's files at run time.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace g2::clavia {

using Bytes = std::vector<std::uint8_t>;

// One resource of a classic Mac OS resource map (Inside Macintosh: More Macintosh Toolbox, Resource Manager).
struct Resource {
    std::string type; // four characters
    int id = 0;
    std::string name;
    Bytes data;
};
// Throws std::runtime_error on a malformed map.
std::vector<Resource> parseResources(std::span<const std::uint8_t> rsrc);

// Apple UDIF disk images (.dmg): whether `file` is one, and the disk it holds (stored, zero-filled and
// zlib-compressed chunks; the others throw).
bool isUdifImage(std::span<const std::uint8_t> file);
Bytes udifDisk(std::span<const std::uint8_t> file);

// The resource files on a disk: every resource map found at the start of a 512-byte block whose header, map and
// the copy of the header at the start of the map agree. HFS stores a file's blocks in order when it is in one
// extent, as both resource files are on Clavia's image (a fragmented file would need the volume's catalog; its
// map would not check out and it would not be found).
std::vector<std::span<const std::uint8_t>> resourceFilesOnDisk(std::span<const std::uint8_t> disk);

Bytes readFile(const std::filesystem::path& path);

// The original editor's resource file ("Nord Modular G2 Editor.rsrc", the one that holds the module panels, PANL,
// and the bitmaps, CBMP) from what the user points at: the .dmg as downloaded, the editor app (or the mounted
// volume that holds it), or the .rsrc itself. Throws std::runtime_error with a message for the user.
Bytes editorResources(const std::filesystem::path& path);
// Whether a resource file is the editor's (it holds PANL and CBMP resources).
bool isEditorResources(const std::vector<Resource>& resources);

} // namespace g2::clavia
