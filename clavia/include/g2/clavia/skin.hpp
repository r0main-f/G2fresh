// The original editor's graphics that G2fresh's Classic look draws with, taken from the user's own copy of the
// editor's resource file (download.hpp) at run time: the module faces and control sprites (CBMP), small UI sprites
// (JPEG), the note sequencer's morph strips (SMTB) and the images inline in the module panels (PANL). C++ ports of
// tools/rsrc/bmp2png.py and panl.py. The panel layouts themselves are G2fresh's data (assets/clavia/panels.json,
// pixels stripped by tools/assets/build_assets.py).
#pragma once

#include "g2/clavia/download.hpp"

#include <array>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace g2::clavia {

// An uncompressed 24-bit BMP (the editor's CBMP and SMTB resources; Windows 40-byte or OS/2 v2 64-byte info
// header, bottom-up BGR rows padded to 4 bytes), as top-down R, G, B rows. Throws std::runtime_error.
struct Bitmap {
    int width = 0, height = 0;
    Bytes rgb;
};
Bitmap decodeBmp(std::span<const std::uint8_t> bmp);

// A PANL resource: a tree of "<#Type" ... "#>" blocks of "Key:value" lines, values integers or double-quoted
// strings (kept as written, without the quotes). Throws std::runtime_error.
struct PanlNode {
    std::string type;
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<PanlNode> children;
    const std::string* field(std::string_view key) const;
};
PanlNode parsePanl(std::string_view text);

// The sprites the Classic look draws besides the module faces (the faces: the module database's faceResId).
inline constexpr std::array<int, 9> kCbmpSprites{199, 200, 201, 202, 203, 204, 205, 531, 532};
inline constexpr std::array<int, 27> kJpegSprites{115, 116, 117, 118, 212, 213, 214, 215, 216, 217, 240, 241, 242, 243,
                                                  244, 245, 246, 247, 248, 250, 251, 268, 269, 272, 273, 874, 877};
// CSeqSliderGUI::Draw's strips for a morphed step: 0xD01 (focused group), 0xD02 (the range), 0xD03 (another group).
inline constexpr std::array<int, 3> kSmtbSprites{0xD01, 0xD02, 0xD03};

struct ClassicSkin {
    std::map<int, Bitmap> cbmp, smtb;
    std::map<int, Bytes> jpeg; // JPEG files as stored
    // Per panel (PANL resource id), per element of the module in order (the children of panels.json's module):
    // the pixels of its inline image, "rrggbb:rrggbb:...", a Bitmap's Data or a button's Image; empty if none.
    std::map<int, std::vector<std::string>> inlineImages;
};
// Everything the Classic look uses, from the editor's resource file. Resources the file lacks are left out.
ClassicSkin classicSkin(std::span<const std::uint8_t> editorRsrc);

} // namespace g2::clavia
