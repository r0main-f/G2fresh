#include "g2/clavia/skin.hpp"

#include "g2/module_db.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <stdexcept>

namespace g2::clavia {
namespace {

std::uint32_t le16(std::span<const std::uint8_t> b, std::size_t o)
{
    if (o + 2 > b.size())
        throw std::runtime_error("bmp: read past the end");
    return std::uint32_t(b[o]) | std::uint32_t(b[o + 1]) << 8;
}

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t o)
{
    return le16(b, o) | le16(b, o + 2) << 16;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

// A "Key:value" line: a key of letters and spaces, then an integer or a double-quoted string.
bool keyValue(std::string_view line, std::string& key, std::string& value)
{
    line = trim(line);
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0 || !std::isalpha(static_cast<unsigned char>(line[0])))
        return false;
    const auto k = line.substr(0, colon);
    if (!std::all_of(k.begin(), k.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == ' '; }))
        return false;
    auto v = line.substr(colon + 1);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        v = v.substr(1, v.size() - 2);
    } else {
        const auto digits = v.substr(!v.empty() && v.front() == '-' ? 1 : 0);
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
                                           [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return false;
    }
    key.assign(k);
    value.assign(v);
    return true;
}

} // namespace

Bitmap decodeBmp(std::span<const std::uint8_t> data)
{
    if (data.size() < 2 || data[0] != 'B' || data[1] != 'M')
        throw std::runtime_error("bmp: not a BMP");
    const auto pixelOff = le32(data, 10), headerSize = le32(data, 14);
    if (headerSize != 40 && headerSize != 64 && headerSize != 108 && headerSize != 124)
        throw std::runtime_error("bmp: unsupported header size " + std::to_string(headerSize));
    const auto width = std::int32_t(le32(data, 18)), rawHeight = std::int32_t(le32(data, 22));
    const auto bpp = le16(data, 28), compression = le32(data, 30);
    if (bpp != 24 || compression != 0)
        throw std::runtime_error("bmp: unsupported: " + std::to_string(bpp) + " bpp, compression "
                                 + std::to_string(compression));
    const bool topDown = rawHeight < 0;
    const int height = std::abs(rawHeight);
    if (width <= 0 || height == 0 || width > 1 << 14 || height > 1 << 14)
        throw std::runtime_error("bmp: bad size");
    const std::size_t stride = (std::size_t(width) * 3 + 3) & ~std::size_t(3);
    if (pixelOff + stride * std::size_t(height - 1) + std::size_t(width) * 3 > data.size())
        throw std::runtime_error("bmp: pixels past the end");
    Bitmap b;
    b.width = width;
    b.height = height;
    b.rgb.resize(std::size_t(width) * std::size_t(height) * 3);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = data.data() + pixelOff + stride * std::size_t(topDown ? y : height - 1 - y);
        std::uint8_t* dst = b.rgb.data() + std::size_t(y) * std::size_t(width) * 3;
        for (int x = 0; x < width; ++x, src += 3, dst += 3) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        }
    }
    return b;
}

const std::string* PanlNode::field(std::string_view key) const
{
    for (const auto& [k, v] : fields)
        if (k == key)
            return &v;
    return nullptr;
}

PanlNode parsePanl(std::string_view text)
{
    PanlNode root;
    std::vector<PanlNode*> stack{&root};
    std::string key, value;
    while (!text.empty()) {
        const auto end = text.find_first_of("\r\n");
        const auto raw = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        const auto line = trim(raw);
        if (line.empty())
            continue;
        if (line.substr(0, 2) == "<#") {
            stack.back()->children.push_back({std::string(trim(line.substr(2))), {}, {}});
            stack.push_back(&stack.back()->children.back());
        } else if (line == "#>") {
            if (stack.size() < 2)
                throw std::runtime_error("PANL: unbalanced blocks");
            stack.pop_back();
        } else if (keyValue(line, key, value)) {
            stack.back()->fields.emplace_back(key, value);
        } else {
            throw std::runtime_error("PANL: unparsed line: " + std::string(line));
        }
    }
    if (stack.size() != 1 || root.children.size() != 1)
        throw std::runtime_error("PANL: unbalanced blocks");
    return std::move(root.children.front());
}

ClassicSkin classicSkin(std::span<const std::uint8_t> editorRsrc)
{
    std::set<int> faces;
    for (const auto& m : g2::db::modules())
        if (m.faceResId)
            faces.insert(m.faceResId);
    const auto in = [](const auto& list, int id) { return std::find(list.begin(), list.end(), id) != list.end(); };

    ClassicSkin skin;
    for (auto& r : parseResources(editorRsrc)) {
        if (r.type == "CBMP" && (faces.count(r.id) || in(kCbmpSprites, r.id))) {
            skin.cbmp.emplace(r.id, decodeBmp(r.data));
        } else if (r.type == "SMTB" && in(kSmtbSprites, r.id)) {
            skin.smtb.emplace(r.id, decodeBmp(r.data));
        } else if (r.type == "JPEG" && in(kJpegSprites, r.id)) {
            skin.jpeg.emplace(r.id, std::move(r.data));
        } else if (r.type == "PANL") {
            const auto panel = parsePanl(std::string_view(reinterpret_cast<const char*>(r.data.data()), r.data.size()));
            auto& images = skin.inlineImages[r.id];
            for (const auto& e : panel.children) {
                const auto* pixels = e.type == "Bitmap" ? e.field("Data") : e.field("Image");
                images.push_back(pixels ? *pixels : std::string());
            }
        }
    }
    return skin;
}

} // namespace g2::clavia
