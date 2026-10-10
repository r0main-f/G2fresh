// The G2's firmware, read from the user's own copy of Clavia's updater (re/notes/g2-hardware-and-emulation.md
// §2): the OS image (`NMG2` 128 "OS") and the boot loader (`BOOT` 128 "Loader") in the resource file of the Mac
// "Nord Modular G2 OS Update.app" (`Contents/Resources/Nord Modular G2 Updater.rsrc`). A C++ port of
// tools/firmware/g2os.py: the resource map, the OS image header, its LZO1X sections and their checksums.
//
// Nothing from Clavia is in this code. The bytes come from the user's files at run time.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace g2emu {

struct OsSection {
    std::string name;        // "SRAM", "CODE"
    std::uint32_t address = 0;
    std::vector<std::uint8_t> data;  // unpacked
};

struct Firmware {
    int version = 0;                     // 162 = OS 1.62
    std::vector<OsSection> sections;     // as the boot loader loads them
    std::vector<std::uint8_t> bootLoader;  // ColdFire code linked at address 0 (may be empty)

    // The section the boot loader jumps to ("CODE"), or nullptr.
    const OsSection* code() const;
    const OsSection* section(const std::string& name) const;

    // From the updater's resource file (a resource map in the data fork).
    static Firmware fromUpdaterResources(std::span<const std::uint8_t> rsrc);
    // From the OS image (NMG2 128) and, optionally, the boot loader (BOOT 128).
    static Firmware fromOsImage(std::span<const std::uint8_t> os, std::span<const std::uint8_t> boot = {});
    // From a path: the updater's .rsrc file, the updater app (…/Nord Modular G2 OS Update.app), or a directory
    // that g2os.py wrote (NMG2_OS.bin and BOOT_Loader.bin), or G2fresh's original/firmware directory
    // (mac-updater-rsrc/NMG2/128_OS.bin, mac-updater-rsrc/BOOT/128_Loader.bin). Throws std::runtime_error.
    static Firmware load(const std::filesystem::path& path);
};

// One resource of a classic Mac OS resource map (Inside Macintosh: More Macintosh Toolbox, Resource Manager).
struct Resource {
    std::string type;  // four characters
    int id = 0;
    std::string name;
    std::vector<std::uint8_t> data;
};
std::vector<Resource> parseResources(std::span<const std::uint8_t> rsrc);

// LZO1X decompression, the semantics of lzo1x_decompress() (and of the G2 boot loader). Throws on a malformed
// stream.
std::vector<std::uint8_t> lzo1xDecompress(std::span<const std::uint8_t> src);

// The checksum of the OS image: the one's complement of the byte sum.
std::uint32_t onesComplementSum(std::span<const std::uint8_t> data);

} // namespace g2emu
