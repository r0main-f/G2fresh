// How many of the computer's threads the emulated G2s may run at real-time priority (re/notes/audio-path.md §3.7).
// macOS keeps time-constraint threads on the performance cores; the machine's threads work in step, each waiting
// for the others, so when there are more of them than cores, one that loses its core holds the others up while they
// spin. Measured on an M1 (4 performance cores): two emulated G2s at once, six real-time threads, lost all of their
// audio at 0.46 x real time; at an application's priority (where the efficiency cores help) none, at 2.0 x. So the
// machines count their real-time threads together, across processes (a host may run each plugin in a process of its
// own; two stand-alone apps are two processes), and keep real-time priority only while all of them fit.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace g2emu {

// `threads`: the real-time threads of all the emulated G2s running, this one's included. One performance core stays
// for the host's own audio thread. 0 cores: not known.
inline bool realtimeFits(int threads, int performanceCores) { return threads >= 1 && threads <= performanceCores - 1; }

// The computer's performance cores (macOS: the first performance level; a Mac with one kind of core: all of them).
// 0 where it is not known.
inline int performanceCores()
{
#ifdef __APPLE__
    int n = 0;
    std::size_t size = sizeof(n);
    if(sysctlbyname("hw.perflevel0.physicalcpu", &n, &size, nullptr, 0) == 0 && n > 0) return n;
    size = sizeof(n);
    if(sysctlbyname("hw.physicalcpu", &n, &size, nullptr, 0) == 0 && n > 0) return n;
#endif
    return 0;
}

// One emulated G2's share: while it lives, a file in `dir` named for its process and its count of real-time threads,
// held under a lock (flock: let go by the system when the process ends, however it ends). threadsAll() adds up the
// files whose lock is held; a file nobody holds is from a process that ended without removing it, and goes. Where the
// folder cannot be made or read (and on Windows, where nothing runs at real-time priority) each machine counts itself
// alone, as before. Not real-time safe (files): call it from time to time, off the audio thread.
class RealtimeShare {
public:
    RealtimeShare(std::filesystem::path dir, int threads) : dir_(std::move(dir)), threads_(threads)
    {
#ifndef _WIN32
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        static int seq = 0;
        path_ = dir_ / (std::to_string(::getpid()) + "-" + std::to_string(++seq) + "-" + std::to_string(threads));
        // made under another name and locked before it gets its own, so that a count never finds it unlocked
        auto made = path_;
        made += ".new";
        fd_ = ::open(made.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if(fd_ >= 0 && (::flock(fd_, LOCK_EX | LOCK_NB) != 0 || ::rename(made.c_str(), path_.c_str()) != 0))
        {
            ::close(fd_);
            ::unlink(made.c_str());
            fd_ = -1;
        }
#endif
    }
    RealtimeShare(const RealtimeShare&) = delete;
    RealtimeShare& operator=(const RealtimeShare&) = delete;
    ~RealtimeShare()
    {
#ifndef _WIN32
        if(fd_ >= 0)
        {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
            ::close(fd_);
        }
#endif
    }

    // The default folder: the user's temporary folder (per user on macOS), so every G2fresh of this user sees it.
    static std::filesystem::path defaultDir()
    {
        std::error_code ec;
        const auto tmp = std::filesystem::temp_directory_path(ec);
        return ec ? std::filesystem::path() : tmp / "g2fresh-realtime";
    }

    int threadsAll() const
    {
#ifndef _WIN32
        if(fd_ < 0) return threads_;
        int all = 0;
        std::error_code ec;
        for(std::filesystem::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec))
        {
            const auto& p = it->path();
            const auto name = p.filename().string();
            const auto dash = name.rfind('-');
            if(dash == std::string::npos || name.find('.') != std::string::npos) continue;  // (one being made)
            const int n = std::atoi(name.c_str() + dash + 1);
            if(p == path_)
            {
                all += n;
                continue;
            }
            const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
            if(fd < 0) continue;
            if(::flock(fd, LOCK_SH | LOCK_NB) == 0)
            {
                std::error_code rm;
                std::filesystem::remove(p, rm);  // nobody holds it: its process has ended
            }
            else if(errno == EWOULDBLOCK)
                all += n;
            ::close(fd);
        }
        return ec ? threads_ : all;
#else
        return threads_;
#endif
    }

private:
    std::filesystem::path dir_, path_;
    int threads_ = 0;
#ifndef _WIN32
    int fd_ = -1;
#endif
};

} // namespace g2emu
