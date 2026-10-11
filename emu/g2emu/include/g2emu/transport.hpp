// Our protocol client's link to the emulated G2: a proto::Transport over the emulated ISP1181 USB chip, so that
// proto::Client (and a LocalLink around it) talks to the user's own OS running in the emulator as it talks to a G2
// over libusb. Thread-safe against the machine: the machine may run on another thread.
#pragma once

#include "g2/proto/link.hpp"
#include "g2/proto/transport.hpp"
#include "g2emu/machine.hpp"

#include <cstdint>
#include <memory>
#include <span>

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

// A memory image (Machine::loadMemory) the OS starts a new G2 from: no EEPROM content (none yet, or an image from
// before G2fresh kept the EEPROM). The OS then takes its own built-in system settings (its settings constructor,
// OS 1.62 @0x30023B92): MIDI Local On, slots on channels 1-4, Global channel 16, Prg Chng and Ctrl Send & Receive,
// SysEx ID All, Memory Protect Off, name "ModularG2" (and formats an erased flash). Empty counts as new.
inline bool newMemory(std::span<const std::uint8_t> image)
{
    if(image.size() <= Machine::EepromSize) return true;
    for(const auto b : image.last(Machine::EepromSize))
        if(b != 0xFF) return false;
    return true;
}

// The system settings of a G2 as shipped, over `s` (the OS's own for the rest): what the manual states as factory
// values where the OS's built-in ones differ, Global channel Off [M p.126] and Memory Protect On [M p.39, p.49]. A new
// emulated G2 gets them once through the Synth Settings message, as Clavia's editor sets them; the OS stores them in
// its EEPROM. The OS's other defaults agree with the manual's System menu pictures [M p.30-31] or the manual says
// nothing about them: re/notes/g2-hardware-and-emulation.md §3.10.5.
inline g2::proto::SynthSettings factorySettings(g2::proto::SynthSettings s)
{
    s.globalChannel = 16;  // Off
    s.memoryProtect = true;
    return s;
}

// A SynthLink to the emulated G2, as LocalLink::virtualG2() is one to the protocol emulator: the whole editor stack
// (SynthSync, the bank browser, live edits) can drive it. The link times out by the wall clock, so the machine has to
// run in real time on another thread (Runner).
inline std::unique_ptr<g2::proto::LocalLink> emulatedG2Link(Machine& machine)
{
    machine.plugUsb();
    return std::make_unique<g2::proto::LocalLink>(std::make_unique<MachineTransport>(machine));
}

} // namespace g2emu
