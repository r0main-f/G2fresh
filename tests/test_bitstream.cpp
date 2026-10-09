#include <catch2/catch_test_macros.hpp>

#include "g2/bitstream.hpp"
#include "g2/crc.hpp"

#include <array>
#include <string_view>

TEST_CASE("bit writer and reader round-trip mixed widths")
{
    g2::BitWriter w;
    w.write(0b101, 3);
    w.write(0x1FF, 9);
    w.write(0, 1);
    w.write(0xDEADBEEF, 32);
    w.align();
    REQUIRE(w.data().size() == 6);

    g2::BitReader r(w.data());
    CHECK(r.read(3) == 0b101);
    CHECK(r.read(9) == 0x1FF);
    CHECK(r.read(1) == 0);
    CHECK(r.read(32) == 0xDEADBEEF);
}

TEST_CASE("bits are packed most significant first")
{
    g2::BitWriter w;
    w.write(1, 1);
    w.write(0, 6);
    w.write(1, 1);
    REQUIRE(w.data() == std::vector<std::uint8_t>{0x81});
}

TEST_CASE("reading past the end throws")
{
    const std::array<std::uint8_t, 1> data{0xFF};
    g2::BitReader r(data);
    r.read(6);
    CHECK_THROWS_AS(r.read(3), g2::FormatError);
}

TEST_CASE("crc16 matches the CRC-16/XMODEM check value")
{
    constexpr std::string_view check = "123456789";
    const auto* p = reinterpret_cast<const std::uint8_t*>(check.data());
    CHECK(g2::crc16({p, check.size()}) == 0x31C3);
}

TEST_CASE("kept pad bits that no longer fit are written as zeros")
{
    g2::BitWriter keep;
    keep.write(1, 3);
    keep.align(0x1F); // 5 pad bits: kept
    CHECK(keep.data() == std::vector<std::uint8_t>{0x3F});

    g2::BitWriter moved;
    moved.write(1, 3);
    moved.align(0x7F); // the pad of a longer layout: zeros
    CHECK(moved.data() == std::vector<std::uint8_t>{0x20});
}
