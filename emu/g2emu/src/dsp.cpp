#include "dsp.hpp"

#include "dsp56kEmu/jitconfig.h"
#include "dsp56kEmu/jittrampoline.h"

#include <algorithm>
#include <cstring>
#include <limits>

using namespace dsp56k;

namespace g2emu {

Dsp::Dsp(int index, Options options) : index_(index), options_(options)
{
    auto& clock = periphX_.getEsaiClock();
    clock.setCyclesPerSample(CyclesPerSlot);
    for(int i = 0; i < 2; ++i)
    {
        esai(i).setReadRxCallback([this, i](std::uint64_t& frame, Audio::RxFrame& f) { readRx(i, f); ++frame; });
        esai(i).setWriteTxCallback([this, i](std::uint64_t& frame, const Audio::TxFrame& f) { writeTx(i, f); ++frame; });
    }
    clock.setTickCallback([this] { lastTick_ = dsp_.getInstructionCounter(); });
    periphX_.setExecCallback([this] { return onPeripherals(); });

    // The OS loads whole routines into P memory whenever it compiles a patch: tracking written P addresses as
    // volatile would make the frame code slower with every upload (see JitConfig::trackVolatilePMemory). The
    // background loop's control-rate code is preempted by the frame interrupt at any instruction on the chip, so a
    // DO loop leaves its block when an interrupt waits.
    JitConfig config = dsp_.getJit().getConfig();
    config.trackVolatilePMemory = false;
    config.doLoopExitOnPendingInterrupt = true;
    // Stage 1 keeps subroutines in the interrupt vector area (P:$B8, $D6, $F4: called with jsr and bsset, §3.9.3):
    // without this the JIT compiles every block there as a two-word fast interrupt that never advances the PC.
    config.dynamicFastInterrupts = true;
    dsp_.getJit().setConfig(config);

    boot_ = std::make_unique<DspBoot>(dsp_);
}

Dsp::~Dsp() = default;

void Dsp::setDividers(std::uint32_t tx0, std::uint32_t rx0, std::uint32_t tx1, std::uint32_t rx1)
{
    auto& clock = periphX_.getEsaiClock();
    clock.setEsaiDivider(&esai(0), tx0, rx0);
    clock.setEsaiDivider(&esai(1), tx1, rx1);
}

// ---------------------------------------------------------------------------------------------------------------
// host port

std::uint8_t Dsp::isr()
{
    std::uint8_t v = 0;
    if(rxLatched_ || hdi_.hasTX()) v |= 0x01 | 0x80;              // RXDF, HREQ
    if(hdi_.rxData().size() < 8000) v |= 0x02;                    // TXDE: the host may write ahead, the queue keeps order
    if(hdi_.rxData().empty()) v |= 0x04;                          // TRDY
    v |= std::uint8_t(hdi_.readControlRegister() & 0x18);        // HF2, HF3
    return v;
}

std::uint8_t Dsp::hostRead(int reg)
{
    switch(reg)
    {
    case 0: return icr_;
    case 1:
        // HC stays set until the DSP has taken the command
        if((cvr_ & 0x80) && !hcPending_ && !dsp_.hasPendingExternalInterrupts()) cvr_ &= 0x7f;
        return cvr_;
    case 2: return isr();
    case 3: return ivr_;
    case 5: case 6: case 7:
        if(!rxLatched_ && hdi_.hasTX())
        {
            const TWord w = hdi_.readTX();
            rx_[0] = std::uint8_t(w >> 16); rx_[1] = std::uint8_t(w >> 8); rx_[2] = std::uint8_t(w);
            rxLatched_ = true;
        }
        {
            const std::uint8_t v = (icr_ & 0x20) ? rx_[2 - (reg - 5)] : rx_[reg - 5];
            if(reg == 7) rxLatched_ = false;
            return v;
        }
    default: return 0;
    }
}

void Dsp::hostWrite(int reg, std::uint8_t v)
{
    switch(reg)
    {
    case 0:
        icr_ = v & 0x7f;  // INIT completes at once
        hdi_.setPendingHostFlags01(v & 0x18);
        break;
    case 1:
        cvr_ = v;
        if(v & 0x80)
        {
            // the host command vector is P:2*HV; the DSP takes it when HCIE allows (stage 1 enables HCIE before the
            // host sends any command)
            hcVector_ = TWord(v & 0x7f) * 2;
            if(hdi_.hostCommandsFull())
                hcPending_ = true;
            else
                hdi_.injectHostCommand(hcVector_);
        }
        break;
    case 3: ivr_ = v; break;
    case 5: case 6: case 7:
        tx_[reg - 5] = v;
        if(reg == 7)
        {
            const TWord w = (icr_ & 0x20) ? (TWord(tx_[2]) << 16 | TWord(tx_[1]) << 8 | tx_[0])
                                          : (TWord(tx_[0]) << 16 | TWord(tx_[1]) << 8 | tx_[2]);
            hdi_.writeRX(&w, 1);
        }
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// running

void Dsp::feedBootRom()
{
    // the boot ROM in host-boot mode: count, address, then the words into P; then it jumps there
    while(booting_ && hdi_.hasRXData())
    {
        const TWord w = hdi_.readRX(Movep_Spp);
        if(boot_->hdiWriteTX(w))
        {
            booting_ = false;
            // the serial clock starts with the program: no backlog of slots from the time spent in the boot ROM
            periphX_.getEsaiClock().restartClock();
        }
    }
}

void Dsp::runTo(std::uint64_t target)
{
    if(hcPending_ && !hdi_.hostCommandsFull())
    {
        hdi_.injectHostCommand(hcVector_);
        hcPending_ = false;
    }
    while(dsp_.getInstructionCounter() < target)
    {
        if(booting_)
        {
            feedBootRom();
            if(booting_)
            {
                dsp_.fastForward(TWord(target - dsp_.getInstructionCounter()), 0);
                return;
            }
        }
        if(options_.jit)
            dsp_.getJit().getTrampoline().exec(&dsp_, dsp56k::JitTrampoline::UnrollSize);  // 8 blocks (a count below 8 means 2^32 rounds)
        else
            dsp_.execInterpreter();  // a DO FOREVER does not come back from here: debugging only
        if(dsp_.getPC().toWord() >= 0xff0000)  // a jump into the boot ROM: it loads P again
        {
            boot_->reset();
            booting_ = true;
        }
    }
}

std::uint32_t Dsp::onPeripherals()
{
    // Idle skipping: while the DSP spins in its background loop with nothing to do - the frame counter at most
    // `limit`, no interrupt pending, HF0 clear - nothing it does can change until the next serial slot, so its clock
    // moves on to that slot instead of running the loop. The loop only reads; skipping it changes no state.
    if(!options_.idleSkip) return std::numeric_limits<std::uint32_t>::max();
    const TWord pc = dsp_.getPC().toWord();
    if(pc < idle_.first || pc > idle_.last) return 64;
    if(std::int32_t(mem_.get(MemArea_X, idle_.counter) << 8) >> 8 > std::int32_t(idle_.limit)) return 64;
    if(dsp_.hasPendingInterrupts() || hdi_.hasPendingHostFlags01() || (hdi_.readStatusRegister() & (1 << HDI08::HSR_HF0)))
        return 64;
    const std::uint64_t now = dsp_.getInstructionCounter();
    const std::uint64_t next = lastTick_ + CyclesPerSlot;
    if(next > now + 1)
    {
        const auto n = TWord(next - now - 1);
        dsp_.fastForward(n, n);
        skipped_ += n;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// serial audio

void Dsp::readRx(int i, Audio::RxFrame& f)
{
    ++rxFrames_[i];
    f.resize(Audio::MaxSlotsPerFrame);
    for(std::uint32_t s = 0; s < Audio::MaxSlotsPerFrame; ++s) f[s].fill(0);
    Link* l = in_[i];
    if(!l || !l->active.load(std::memory_order_acquire)) return;  // nothing sends yet, or the ADCs: silence
    if(l->size() == 0)
    {
        ++l->underruns;
        return;
    }
    const auto t = l->tail.load(std::memory_order_relaxed);
    const auto& fr = l->ring[t % Link::Capacity];
    for(std::uint32_t s = 0; s < fr.slots; ++s) { f[s][0] = fr.w[s][0]; f[s][1] = fr.w[s][1]; }
    l->tail.store(t + 1, std::memory_order_release);
    ++l->received;
}

void Dsp::writeTx(int i, const Audio::TxFrame& f)
{
    ++txFrames_[i];
    Link* l = out_[i];
    if(!l)
    {
        if(i == 0 && sink_)
            for(std::uint32_t s = 0; s < 2; ++s)
                for(std::uint32_t t = 0; t < 2; ++t)
                    sink_->push_back(s < f.size() ? std::int32_t(f[s][t] << 8) >> 8 : 0);
        return;
    }
    Dsp& d = *l->down;
    if(!d.esai(l->downEsai).getEnabledReceivers())
    {
        // the other side does not listen (yet): the line carries nothing anyone keeps
        if(l->active.exchange(false)) l->tail.store(l->head.load());
        ++l->dropped;
        return;
    }
    auto h = l->head.load(std::memory_order_relaxed);
    if(!l->active.load(std::memory_order_acquire))
    {
        // the first frame since the receiver listens: start with `prefill` empty frames
        l->tail.store(h);
        for(std::uint32_t k = 0; k < l->prefill; ++k)
        {
            auto& fr = l->ring[h % Link::Capacity];
            fr.slots = Link::Slots;
            std::memset(fr.w, 0, sizeof(fr.w));
            ++h;
        }
        l->head.store(h, std::memory_order_release);
        l->active.store(true, std::memory_order_release);
    }
    if(l->size() >= Link::Capacity - 1)
    {
        ++l->overruns;
        return;
    }
    auto& fr = l->ring[h % Link::Capacity];
    fr.slots = std::min<std::uint32_t>(f.size(), Link::Slots);
    for(std::uint32_t s = 0; s < fr.slots; ++s) { fr.w[s][0] = f[s][l->txBase]; fr.w[s][1] = f[s][l->txBase + 1]; }
    l->head.store(h + 1, std::memory_order_release);
    ++l->sent;
}

} // namespace g2emu
