#include <catch2/catch_test_macros.hpp>

#include "g2/proto/client.hpp"
#include "g2/proto/emulator.hpp"
#include "g2/proto/logging_transport.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

using namespace g2::proto;

namespace {
std::string readAll(const std::filesystem::path& p)
{
    std::ifstream in(p);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
} // namespace

TEST_CASE("the USB log records a whole sync and rotates")
{
    const auto dir = std::filesystem::temp_directory_path() / "g2fresh-log-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const auto path = dir / "usb.log";

    {
        Emulator emulator;
        auto inner = std::make_unique<EmulatorTransport>(emulator);
        auto* emulated = inner.get();
        LoggingTransport log(std::move(inner), path.string()); // 5 MB: no rotation here
        ManualClock clock;
        Client client(log, clock);
        emulated->plugIn();
        for (int i = 0; i < 2000 && !client.synced(); ++i)
            client.tick();
        REQUIRE(client.synced()); // logging does not disturb the conversation

        log.note("a note");
        const auto text = readAll(path);
        CHECK(text.find("device arrived") != std::string::npos);
        CHECK(text.find("version request") != std::string::npos);
        CHECK(text.find("OUT ") != std::string::npos);
        CHECK(text.find("a note") != std::string::npos);
        CHECK((text.find("INT ") != std::string::npos || text.find("BULK") != std::string::npos));
    } // closes the log (Windows cannot delete an open file)

    // Past the limit the file is renamed to .1 and a new one started.
    const auto small = dir / "small.log";
    {
        Emulator e;
        LoggingTransport tiny(std::make_unique<EmulatorTransport>(e), small.string(), 512);
        for (int i = 0; i < 40; ++i)
            tiny.note("line " + std::to_string(i));
    }
    CHECK(std::filesystem::exists(small.string() + ".1"));
    CHECK(std::filesystem::file_size(small) < 512 + 128);
    CHECK(readAll(small).find("log closed") != std::string::npos);
    std::filesystem::remove_all(dir, ec);
}
