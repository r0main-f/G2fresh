#include "g2/proto/logging_transport.hpp"

#include "g2/proto/frame.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <utility>

namespace g2::proto {
namespace {

std::string timestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

std::string hex(std::span<const std::uint8_t> bytes)
{
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    s.reserve(bytes.size() * 3);
    for (const auto b : bytes) {
        if (!s.empty())
            s += ' ';
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

std::string byteText(unsigned v)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02X", v & 0xFF);
    return buf;
}

} // namespace

LoggingTransport::LoggingTransport(std::unique_ptr<Transport> inner, std::string path, std::uint64_t maxBytes)
    : inner_(std::move(inner)), path_(std::move(path)), maxBytes_(maxBytes)
{
    inner_->setSink(this);
    open();
}

LoggingTransport::~LoggingTransport()
{
    note("log closed");
    inner_->setSink(nullptr);
}

void LoggingTransport::open()
{
    std::error_code ec;
    const std::filesystem::path p(path_);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path(), ec);
    out_.open(path_, std::ios::app);
    written_ = out_ ? static_cast<std::uint64_t>(std::filesystem::file_size(p, ec)) : 0;
    if (ec)
        written_ = 0;
}

void LoggingTransport::line(const char* tag, std::span<const std::uint8_t> bytes, const std::string& detail)
{
    if (!out_)
        return;
    std::string text = timestamp() + "  " + tag;
    if (!detail.empty())
        text += "  [" + detail + "]";
    if (!bytes.empty())
        text += "  " + hex(bytes);
    text += '\n';
    out_ << text;
    out_.flush();
    written_ += text.size();
    if (written_ > maxBytes_) {
        out_.close();
        std::error_code ec;
        std::filesystem::rename(path_, path_ + ".1", ec);
        open();
    }
}

void LoggingTransport::note(const std::string& text)
{
    line("NOTE", {}, text);
}

bool LoggingTransport::send(std::span<const std::uint8_t> frame)
{
    std::string detail;
    if (frame.size() == 5 && frame[2] == 0x80) {
        detail = "version request";
    } else if (const auto f = parseHostFrame(frame)) {
        detail = "hdr " + byteText(f->hdr) + " sess " + byteText(f->session);
        if (!f->molecules.empty())
            detail += " id " + byteText(f->molecules[0]);
    }
    const bool ok = inner_->send(frame);
    line(ok ? "OUT " : "OUT (not sent)", frame, detail);
    return ok;
}

void LoggingTransport::poll()
{
    inner_->poll();
}

void LoggingTransport::deviceArrived()
{
    note("device arrived");
    if (sink_)
        sink_->deviceArrived();
}

void LoggingTransport::deviceRemoved()
{
    note("device removed");
    if (sink_)
        sink_->deviceRemoved();
}

void LoggingTransport::interruptPacket(std::span<const std::uint8_t> packet)
{
    line("INT ", packet);
    if (sink_)
        sink_->interruptPacket(packet);
}

void LoggingTransport::bulkIn(std::span<const std::uint8_t> data)
{
    std::string detail;
    if (const auto m = parseDeviceMessage(data)) {
        detail = "hdr " + byteText(m->hdr) + " sess " + byteText(m->session);
        if (!m->body.empty())
            detail += " id " + byteText(m->body[0]);
    }
    line("BULK", data, detail);
    if (sink_)
        sink_->bulkIn(data);
}

std::string defaultUsbLogPath()
{
    auto env = [](const char* name) {
        const char* v = std::getenv(name);
        return v != nullptr ? std::string(v) : std::string();
    };
#if defined(_WIN32)
    const auto base = env("LOCALAPPDATA");
    return (std::filesystem::path(base.empty() ? "." : base) / "G2fresh" / "usb.log").string();
#elif defined(__APPLE__)
    return (std::filesystem::path(env("HOME")) / "Library" / "Logs" / "G2fresh" / "usb.log").string();
#else
    const auto state = env("XDG_STATE_HOME");
    const auto base = state.empty() ? std::filesystem::path(env("HOME")) / ".local" / "state" : std::filesystem::path(state);
    return (base / "G2fresh" / "usb.log").string();
#endif
}

} // namespace g2::proto
