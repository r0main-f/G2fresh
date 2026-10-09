#include "g2/bridge/socket.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace g2::bridge {
namespace {

#ifdef _WIN32
using Native = SOCKET;
using AddressLength = int;
constexpr Native kInvalidNative = INVALID_SOCKET;
bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
void closeNative(Native s) { closesocket(s); }
void removeFile(const std::string& path) { DeleteFileA(path.c_str()); }
#else
using Native = int;
using AddressLength = socklen_t;
constexpr Native kInvalidNative = -1;
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
void closeNative(Native s) { ::close(s); }
void removeFile(const std::string& path) { ::unlink(path.c_str()); }
#endif

Native native(SocketHandle h) { return static_cast<Native>(h); }
SocketHandle handleOf(Native s) { return s == kInvalidNative ? kNoSocket : static_cast<SocketHandle>(s); }

bool setNonBlocking(Native s)
{
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) != 0)
        return false;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); // macOS: no SIGPIPE on a closed peer
#endif
    return true;
#endif
}

bool makeAddress(const std::string& path, sockaddr_un& address)
{
    std::memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof address.sun_path)
        return false;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

Native openStream()
{
    if (!initSockets())
        return kInvalidNative;
    return ::socket(AF_UNIX, SOCK_STREAM, 0);
}

} // namespace

bool initSockets()
{
#ifdef _WIN32
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

std::string defaultSocketPath()
{
#ifdef _WIN32
    char temp[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathA(MAX_PATH, temp);
    std::string dir = n > 0 && n <= MAX_PATH ? std::string(temp, n) : std::string("C:\\Windows\\Temp\\");
    if (!dir.empty() && dir.back() != '\\')
        dir += '\\';
    return dir + "g2bridge.sock";
#else
    std::string dir;
#ifdef __linux__
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        dir = runtime;
#endif
    if (dir.empty())
        if (const char* tmp = std::getenv("TMPDIR"); tmp && *tmp)
            dir = tmp;
    if (dir.empty())
        dir = "/tmp";
    if (dir.back() != '/')
        dir += '/';
    return dir + "g2bridge-" + std::to_string(static_cast<unsigned long>(getuid())) + ".sock";
#endif
}

Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other) {
        close();
        handle_ = other.handle_;
        path_ = std::move(other.path_);
        other.handle_ = kNoSocket;
        other.path_.clear();
    }
    return *this;
}

void Socket::close()
{
    if (handle_ != kNoSocket)
        closeNative(native(handle_));
    handle_ = kNoSocket;
    if (!path_.empty())
        removeFile(path_);
    path_.clear();
}

Socket Socket::listen(const std::string& path, std::string* error)
{
    auto fail = [&](const char* why) {
        if (error)
            *error = why;
        return Socket();
    };
    sockaddr_un address;
    if (!makeAddress(path, address))
        return fail("socket path too long");
    for (int attempt = 0; attempt < 2; ++attempt) {
        const Native s = openStream();
        if (s == kInvalidNative)
            return fail("cannot create a socket");
        if (::bind(s, reinterpret_cast<const sockaddr*>(&address), static_cast<AddressLength>(sizeof address)) == 0) {
            if (::listen(s, 16) != 0 || !setNonBlocking(s)) {
                closeNative(s);
                return fail("cannot listen");
            }
            Socket out(handleOf(s));
            out.path_ = path;
            return out;
        }
        closeNative(s);
        // The file exists (EADDRINUSE; Windows may report other errors for a
        // leftover file): someone listening, or a stale file from a crash.
        if (connect(path).valid())
            return fail("in use");
        removeFile(path);
    }
    return fail("cannot bind the socket");
}

Socket Socket::connect(const std::string& path)
{
    sockaddr_un address;
    if (!makeAddress(path, address))
        return {};
    const Native s = openStream();
    if (s == kInvalidNative)
        return {};
    if (::connect(s, reinterpret_cast<const sockaddr*>(&address), static_cast<AddressLength>(sizeof address)) != 0 || !setNonBlocking(s)) {
        closeNative(s);
        return {};
    }
    return Socket(handleOf(s));
}

Socket Socket::accept()
{
    if (!valid())
        return {};
    const Native s = ::accept(native(handle_), nullptr, nullptr);
    if (s == kInvalidNative)
        return {};
    if (!setNonBlocking(s)) {
        closeNative(s);
        return {};
    }
    return Socket(handleOf(s));
}

long Socket::read(std::span<std::uint8_t> buffer)
{
    if (!valid())
        return -1;
#ifdef _WIN32
    const int n = ::recv(native(handle_), reinterpret_cast<char*>(buffer.data()),
                         static_cast<int>(std::min<std::size_t>(buffer.size(), 1 << 30)), 0);
#else
    const auto n = ::recv(native(handle_), buffer.data(), buffer.size(), 0);
#endif
    if (n > 0)
        return static_cast<long>(n);
    if (n < 0 && wouldBlock())
        return 0;
    return -1; // 0: the peer closed
}

long Socket::write(std::span<const std::uint8_t> data)
{
    if (!valid())
        return -1;
    if (data.empty())
        return 0;
#ifdef _WIN32
    const int n = ::send(native(handle_), reinterpret_cast<const char*>(data.data()),
                         static_cast<int>(std::min<std::size_t>(data.size(), 1 << 30)), 0);
#elif defined(MSG_NOSIGNAL)
    const auto n = ::send(native(handle_), data.data(), data.size(), MSG_NOSIGNAL);
#else
    const auto n = ::send(native(handle_), data.data(), data.size(), 0);
#endif
    if (n >= 0)
        return static_cast<long>(n);
    return wouldBlock() ? 0 : -1;
}

void waitReadable(std::span<const SocketHandle> sockets, int timeoutMs)
{
#ifdef _WIN32
    std::vector<WSAPOLLFD> fds;
    for (const auto s : sockets)
        if (s != kNoSocket)
            fds.push_back({native(s), POLLRDNORM, 0});
    if (fds.empty()) {
        if (timeoutMs > 0)
            Sleep(static_cast<DWORD>(timeoutMs));
        return;
    }
    WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeoutMs);
#else
    std::vector<pollfd> fds;
    for (const auto s : sockets)
        if (s != kNoSocket)
            fds.push_back({native(s), POLLIN, 0});
    ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
#endif
}

} // namespace g2::bridge
