#include "g2/bridge/bridge_link.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace g2::bridge {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// One argument for CreateProcess's command line (the CommandLineToArgvW rules).
std::wstring quoted(const std::wstring& arg)
{
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

} // namespace

bool spawnBridge(const std::string& executable, const std::vector<std::string>& arguments)
{
    const std::wstring exe = widen(executable);
    std::wstring commandLine = quoted(exe);
    for (const auto& a : arguments)
        commandLine += L" " + quoted(widen(a));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

#else

bool spawnBridge(const std::string& executable, const std::vector<std::string>& arguments)
{
    if (access(executable.c_str(), X_OK) != 0)
        return false;
    // Everything the child needs, built before fork(): only async-signal-safe
    // calls happen in between (this may run inside a multithreaded host).
    std::vector<std::string> args{executable};
    args.insert(args.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    // Double fork: the bridge is re-parented to init, leaves our session and
    // never becomes our zombie.
    const pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        setsid();
        const pid_t grandchild = fork();
        if (grandchild != 0)
            _exit(grandchild < 0 ? 1 : 0);
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2)
                close(devnull);
        }
        execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    for (;;) {
        if (waitpid(child, &status, 0) >= 0)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (errno != EINTR)
            return true; // a host ignoring SIGCHLD reaps its children itself (ECHILD)
    }
}

#endif

} // namespace g2::bridge
