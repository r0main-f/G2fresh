// Local stream sockets between g2bridge and its editors: AF_UNIX on macOS,
// Linux and Windows 10+ (afunix.h), non-blocking, with the few operations the
// bridge needs. A socket is a path in the user's temporary directory, so only
// that user's processes can reach it.
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace g2::bridge {

// The platform's socket handle (SOCKET on Windows, int elsewhere).
using SocketHandle = std::intptr_t;
inline constexpr SocketHandle kNoSocket = -1;

// Starts Winsock (once; elsewhere nothing). Returns false if sockets are unavailable.
bool initSockets();

// Where the bridge listens unless told otherwise: "$TMPDIR/g2bridge-<uid>.sock"
// (XDG_RUNTIME_DIR first on Linux), "%TEMP%\g2bridge.sock" on Windows.
std::string defaultSocketPath();

class Socket {
public:
    Socket() = default;
    explicit Socket(SocketHandle handle) : handle_(handle) {}
    ~Socket() { close(); }
    Socket(Socket&& other) noexcept : handle_(other.handle_), path_(std::move(other.path_)) { other.handle_ = kNoSocket; other.path_.clear(); }
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // A listening socket at `path`. A stale socket file (no one listening) is
    // replaced; if another process listens there, fails with "in use".
    static Socket listen(const std::string& path, std::string* error = nullptr);
    // Connects to a listening socket. Invalid if no one listens there.
    static Socket connect(const std::string& path);
    // A pending connection of a listening socket, or an invalid socket.
    Socket accept();

    // Bytes read (> 0), 0 when nothing is available now, -1 when the other
    // side closed or on error.
    long read(std::span<std::uint8_t> buffer);
    // Bytes written (0 when the socket's buffer is full now), -1 on error.
    long write(std::span<const std::uint8_t> data);

    bool valid() const { return handle_ != kNoSocket; }
    SocketHandle handle() const { return handle_; }
    // Closes it; a listening socket also removes its file.
    void close();

private:
    SocketHandle handle_ = kNoSocket;
    std::string path_; // listening sockets: the file to remove
};

// Waits until one of the sockets has something to read (data, a connection,
// or a hang-up), at most `timeoutMs` (0: just checks).
void waitReadable(std::span<const SocketHandle> sockets, int timeoutMs);

} // namespace g2::bridge
