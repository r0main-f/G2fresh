// Our protocol client's link to the emulated G2: a proto::Transport over the emulated ISP1181 USB chip, so that
// proto::Client (and a LocalLink around it) talks to the user's own OS running in the emulator as it talks to a G2
// over libusb. Thread-safe against the machine: the machine may run on another thread.
#pragma once

#include "g2/proto/link.hpp"
#include "g2/proto/transport.hpp"
#include "g2emu/machine.hpp"

#include <memory>

namespace g2emu {

class MachineTransport final : public g2::proto::Transport {
public:
    explicit MachineTransport(Machine& machine) : machine_(machine) {}

    bool send(std::span<const std::uint8_t> frame) override
    {
        if(!announced_) return false;
        machine_.usbSend(frame);
        return true;
    }

    void poll() override
    {
        if(!sink_) return;
        if(!announced_ && machine_.usbArrived())
        {
            announced_ = true;
            sink_->deviceArrived();
        }
        Machine::UsbIn in;
        while(machine_.usbTake(in))
        {
            if(in.bulk) sink_->bulkIn(in.data);
            else sink_->interruptPacket(in.data);
        }
    }

private:
    Machine& machine_;
    bool announced_ = false;
};

// A SynthLink to the emulated G2, as LocalLink::virtualG2() is one to the protocol emulator: the whole editor stack
// (SynthSync, the bank browser, live edits) can drive it. The link times out by the wall clock, so the machine has to
// run in real time on another thread (Runner).
inline std::unique_ptr<g2::proto::LocalLink> emulatedG2Link(Machine& machine)
{
    machine.plugUsb();
    return std::make_unique<g2::proto::LocalLink>(std::make_unique<MachineTransport>(machine));
}

} // namespace g2emu
