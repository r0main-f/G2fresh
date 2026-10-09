// g2bridge: owns the G2's USB connection and serves the editors (see
// include/g2/bridge/server.hpp). Started on demand by an editor; exits a few
// seconds after the last one leaves.
//
//   g2bridge [--emulator] [--stay] [--socket PATH] [--idle-exit MS] [--verbose]

#include "g2/bridge/server.hpp"

#ifdef G2_HAVE_USB
#include "g2/usb/libusb_transport.hpp"
#endif

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::atomic<bool> stopRequested{false};

extern "C" void onSignal(int) { stopRequested = true; }

void usage()
{
    std::fprintf(stderr, "usage: g2bridge [--emulator] [--stay] [--socket PATH] [--idle-exit MS] [--verbose]\n");
}

} // namespace

int main(int argc, char** argv)
{
    using namespace g2;
    bridge::BridgeServer::Options options;
    bool emulator = false, verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--emulator")
            emulator = true;
        else if (a == "--stay")
            options.stay = true;
        else if (a == "--verbose")
            verbose = true;
        else if (a == "--socket" && i + 1 < argc)
            options.socketPath = argv[++i];
        else if (a == "--idle-exit" && i + 1 < argc)
            options.idleExitMs = std::atoi(argv[++i]);
        else {
            usage();
            return 2;
        }
    }

    std::unique_ptr<proto::LocalLink> link;
    if (emulator) {
        link = proto::LocalLink::virtualG2();
    } else {
#ifdef G2_HAVE_USB
        link = std::make_unique<proto::LocalLink>(std::make_unique<usb::LibusbTransport>());
#else
        std::fprintf(stderr, "g2bridge: built without USB; use --emulator\n");
        return 2;
#endif
    }

    bridge::BridgeServer server(std::move(link), options);
    std::string error;
    if (!server.start(&error)) {
        // Another bridge already serves this user: nothing to do.
        if (verbose || error != "in use")
            std::fprintf(stderr, "g2bridge: %s (%s)\n", error.c_str(), options.socketPath.c_str());
        return error == "in use" ? 0 : 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    if (verbose)
        std::fprintf(stderr, "g2bridge: listening on %s%s\n", options.socketPath.c_str(), emulator ? " (virtual G2)" : "");

    auto lastStatus = server.link().status();
    while (!stopRequested && !server.shouldExit()) {
        server.pump(10);
        if (verbose && server.link().status() != lastStatus) {
            lastStatus = server.link().status();
            std::fprintf(stderr, "g2bridge: %s, %zu editor(s)\n", server.link().statusLine().c_str(), server.clientCount());
        }
    }
    if (verbose)
        std::fprintf(stderr, "g2bridge: exiting\n");
    return 0;
}
