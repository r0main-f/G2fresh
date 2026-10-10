#include "hw.hpp"

#include <algorithm>

namespace g2emu {

namespace {
// SIM register offsets (MCF5307/5407 UM)
constexpr std::uint32_t IPR = 0x40, IMR = 0x44, AVCR = 0x4B, ICR0 = 0x4C;
constexpr std::uint32_t TimerBase[2] = {0x140, 0x180};
constexpr std::uint32_t UartBase[2] = {0x1C0, 0x200};
// IMR bits of the internal sources (ICRn: IMR bit 8 + n): 9 TIMER0, 10 TIMER1, 11 I2C, 12 UART0, 13 UART1 [C, the
// OS's set-up code clears exactly these bits for its timer 1 and UART0]
constexpr int TimerImrBit[2] = {9, 10};
constexpr int UartImrBit[2] = {12, 13};
constexpr std::uint32_t TimerIcr[2] = {ICR0 + 1, ICR0 + 2};
constexpr std::uint32_t UartIcr[2] = {ICR0 + 4, ICR0 + 5};
} // namespace

Sim::Sim(std::function<std::uint64_t()> busNow) : busNow_(std::move(busNow))
{
    r_[IMR] = 0xff; r_[IMR + 1] = 0xff; r_[IMR + 2] = 0xff; r_[IMR + 3] = 0xfe;  // all masked at reset
}

// ---------------------------------------------------------------------------------------------------------------
// timers. TMR: PS[15:8], CE[7:6], OM[5], ORI[4], FRR[3], CLK[2:1] (00 stop, 01 bus clock, 10 bus clock / 16,
// 11 TIN), RST[0]. TCN counts at CLK / (PS + 1); with FRR it restarts at 0 after reaching TRR (period TRR + 1
// counts; TRR = 0 is taken as no reference, a free-running 16-bit counter, as in g2hostemu.py).

std::uint32_t Sim::timerPeriod(const Timer& t) const
{
    const unsigned clk = (t.tmr >> 1) & 3;
    if(!(t.tmr & 1) || clk == 0 || clk == 3) return 0;  // reset, stopped, or an external clock nobody drives
    return ((t.tmr >> 8) + 1u) * (clk == 2 ? 16u : 1u);
}

std::uint32_t Sim::timerCount(const Timer& t, std::uint64_t now) const
{
    const auto p = timerPeriod(t);
    if(!p) return std::uint32_t(t.base);  // stopped: base holds the count
    const std::uint64_t n = (now - t.base) / p;
    const std::uint64_t m = (t.tmr & 8) && t.trr ? std::uint64_t(t.trr) + 1 : 0x10000;
    return std::uint32_t(n % m);
}

void Sim::timerUpdate(Timer& t, std::uint64_t now)
{
    const auto p = timerPeriod(t);
    const int i = int(&t - timer_.data());
    if(!p)
    {
        timerChecked_[i] = now;
        return;
    }
    // reference events in (checked, now]: absolute counts k*m + trr
    const std::uint64_t m = (t.tmr & 8) && t.trr ? std::uint64_t(t.trr) + 1 : 0x10000;
    const auto from = timerChecked_[i] > t.base ? (timerChecked_[i] - t.base) / p : 0;
    const auto to = now > t.base ? (now - t.base) / p : 0;
    timerChecked_[i] = now;
    auto events = [&](std::uint64_t n) { return n >= t.trr ? (n - t.trr) / m + 1 : 0; };
    if(t.trr && events(to) > events(from)) t.ter |= 2;  // REF
}

void Sim::advance(std::uint64_t now)
{
    for(auto& t : timer_) timerUpdate(t, now);
    // MIDI bytes complete one by one at the line rate; a full FIFO overruns
    while(!midiPending_.empty() && now >= midiNext_)
    {
        auto& u = uart_[0];
        if(u.rxEnabled)
        {
            if(u.fifo.size() < 3)
                u.fifo.push_back(midiPending_.front());
            else
            {
                u.overrun = true;
                ++uartOverruns_;
            }
        }
        midiPending_.pop_front();
        midiNext_ += busClocksPerMidiByte();
    }
}

std::uint64_t Sim::nextEvent() const
{
    std::uint64_t next = ~0ull;
    if(!midiPending_.empty()) next = std::min(next, midiNext_);
    for(int i = 0; i < 2; ++i)
    {
        const auto& t = timer_[i];
        const auto p = timerPeriod(t);
        if(!p || !t.trr || !(t.tmr & 0x10)) continue;
        // the next reference count after the last check: absolute counts trr + k * m
        const std::uint64_t m = (t.tmr & 8) ? std::uint64_t(t.trr) + 1 : 0x10000;
        const std::uint64_t n = timerChecked_[i] > t.base ? (timerChecked_[i] - t.base) / p : 0;
        const std::uint64_t k = n < t.trr ? 0 : (n - t.trr) / m + 1;
        next = std::min(next, t.base + (t.trr + k * m) * p);
    }
    return next;
}

std::uint32_t Sim::busClocksPerMidiByte() const
{
    // 10 bits per byte at fbus / (32 * UBG)
    const std::uint32_t ubg = (std::uint32_t(uart_[0].ubg1) << 8 | uart_[0].ubg2);
    return 10u * 32u * (ubg ? ubg : 54u);
}

void Sim::midiIn(const std::uint8_t* data, std::size_t n)
{
    const auto now = busNow_();
    if(midiPending_.empty()) midiNext_ = std::max(midiNext_, now + busClocksPerMidiByte());
    midiPending_.insert(midiPending_.end(), data, data + n);
}

std::vector<std::uint8_t> Sim::takeMidiOut()
{
    std::vector<std::uint8_t> v;
    v.swap(uart_[0].tx);
    return v;
}

// ---------------------------------------------------------------------------------------------------------------
// UARTs. USR: RB FE PE OE TxEMP TxRDY FFULL RxRDY; UISR: COS - - - - DB RxRDY/FFULL TxRDY.

std::uint8_t Sim::usr(const Uart& u) const
{
    std::uint8_t v = 0x0C;  // the transmitter is always ready and empty: bytes leave at once
    if(!u.fifo.empty()) v |= 0x01;
    if(u.fifo.size() >= 3) v |= 0x02;
    if(u.overrun) v |= 0x10;
    return v;
}

std::uint8_t Sim::uisr(const Uart& u) const
{
    std::uint8_t v = 0x01;  // TxRDY
    const bool ffullIrq = u.umr[0] & 0x40;  // UMR1[RxIRQ]: interrupt on FFULL instead of RxRDY
    if(ffullIrq ? u.fifo.size() >= 3 : !u.fifo.empty()) v |= 0x02;
    return v;
}

std::uint32_t Sim::readUart(Uart& u, std::uint32_t off)
{
    switch(off)
    {
    case 0x00: { const auto v = u.umr[u.umrPtr]; u.umrPtr = 1; return v; }
    case 0x04: return usr(u);
    case 0x0C:
        if(u.fifo.empty()) return 0;
        {
            const auto v = u.fifo.front();
            u.fifo.pop_front();
            return v;
        }
    case 0x10: return 0;          // UIPCR
    case 0x14: return uisr(u);
    case 0x18: return u.ubg1;
    case 0x1C: return u.ubg2;
    case 0x30: return u.uivr;
    case 0x34: return 0x01;       // UIP: CTS
    default: return 0;
    }
}

void Sim::writeUart(Uart& u, std::uint32_t off, std::uint8_t v)
{
    switch(off)
    {
    case 0x00: u.umr[u.umrPtr] = v; u.umrPtr = 1; break;
    case 0x08:  // UCR
        switch((v >> 4) & 7)
        {
        case 1: u.umrPtr = 0; break;
        case 2: u.fifo.clear(); u.overrun = false; break;
        case 4: u.overrun = false; break;
        default: break;
        }
        if((v & 3) == 1) u.rxEnabled = true;
        if((v & 3) == 2) u.rxEnabled = false;
        if(((v >> 2) & 3) == 1) u.txEnabled = true;
        if(((v >> 2) & 3) == 2) u.txEnabled = false;
        break;
    case 0x0C: if(u.tx.size() < 65536) u.tx.push_back(v); break;
    case 0x14: u.uimr = v; break;
    case 0x18: u.ubg1 = v; break;
    case 0x1C: u.ubg2 = v; break;
    case 0x30: u.uivr = v; break;
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// I2C (IADR 0x280, IFDR 0x284, I2CR 0x288, I2SR 0x28C, I2DR 0x290)

std::uint8_t Sim::readI2c(std::uint32_t off)
{
    switch(off)
    {
    case 0x28C: return i2cMsr_;
    case 0x288: return i2cMcr_;
    case 0x290: i2cMsr_ |= 0x82; return 0x80;
    default: return r_[off];
    }
}

void Sim::writeI2c(std::uint32_t off, std::uint8_t v)
{
    switch(off)
    {
    case 0x288:
        {
            const bool start = (v & 0x20) && !(i2cMcr_ & 0x20), stop = (i2cMcr_ & 0x20) && !(v & 0x20);
            const bool rsta = v & 0x04;
            i2cMcr_ = v & ~0x04;
            if(start || rsta) i2cMsr_ |= 0x20;  // IBB
            if(stop) i2cMsr_ &= ~0x20;
        }
        break;
    case 0x28C: i2cMsr_ = std::uint8_t((i2cMsr_ & ~0x12) | (v & 0x12)); break;
    case 0x290: i2cMsr_ = std::uint8_t((i2cMsr_ | 0x82) & ~0x01); break;  // ICF, IIF, acknowledged
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------------------------

std::uint32_t Sim::read(std::uint32_t off, int size)
{
    const auto now = busNow_();
    std::uint32_t v = 0;
    for(int k = 0; k < size; ++k)
    {
        const std::uint32_t o = off + std::uint32_t(k);
        std::uint8_t b = o < r_.size() ? r_[o] : 0;
        for(int i = 0; i < 2; ++i)
        {
            const auto base = TimerBase[i];
            auto& t = timer_[i];
            if(o >= base && o < base + 0x12)
            {
                std::uint16_t w = 0;
                switch((o - base) & ~1u)
                {
                case 0x0: w = t.tmr; break;
                case 0x4: w = t.trr; break;
                case 0x8: w = t.tcr; break;
                case 0xC: w = std::uint16_t(timerCount(t, now)); break;
                case 0x10: timerUpdate(t, now); w = t.ter; break;
                default: break;
                }
                b = (o - base) == 0x11 ? std::uint8_t(t.ter) : (o & 1) ? std::uint8_t(w) : std::uint8_t(w >> 8);
                if(o - base == 0x10) b = 0;
            }
        }
        for(int i = 0; i < 2; ++i)
            if(o >= UartBase[i] && o < UartBase[i] + 0x40)
                b = (o - UartBase[i]) % 4 == 0 ? std::uint8_t(readUart(uart_[i], o - UartBase[i])) : 0;
        if(o >= 0x280 && o < 0x2A0) b = readI2c(o);
        if(o >= IPR && o < IPR + 4)
        {
            std::uint32_t ipr = 0;
            for(int i = 0; i < 2; ++i)
                if((timer_[i].ter & 2) && (timer_[i].tmr & 0x10)) ipr |= 1u << TimerImrBit[i];
            for(int i = 0; i < 2; ++i)
                if(uisr(uart_[i]) & uart_[i].uimr) ipr |= 1u << UartImrBit[i];
            ipr |= externalIrqs_;
            b = std::uint8_t(ipr >> (8 * (3 - (o - IPR))));
        }
        v = v << 8 | b;
    }
    return v;
}

void Sim::write(std::uint32_t off, int size, std::uint32_t value)
{
    const auto now = busNow_();
    for(int k = 0; k < size; ++k)
    {
        const std::uint32_t o = off + std::uint32_t(k);
        const auto b = std::uint8_t(value >> (8 * (size - 1 - k)));
        if(o < r_.size()) r_[o] = b;
        for(int i = 0; i < 2; ++i)
            if(o >= UartBase[i] && o < UartBase[i] + 0x40 && (o - UartBase[i]) % 4 == 0)
                writeUart(uart_[i], o - UartBase[i], b);
        if(o >= 0x280 && o < 0x2A0) writeI2c(o, b);
    }
    // timers: whole registers
    for(int i = 0; i < 2; ++i)
    {
        const auto base = TimerBase[i];
        auto& t = timer_[i];
        if(off + std::uint32_t(size) <= base || off >= base + 0x12) continue;
        auto touched = [&](std::uint32_t reg, std::uint32_t len) { return off < base + reg + len && off + std::uint32_t(size) > base + reg; };
        auto get16 = [&](std::uint32_t reg) { return std::uint16_t(r_[base + reg] << 8 | r_[base + reg + 1]); };
        if(touched(0x11, 1)) t.ter &= std::uint8_t(~r_[base + 0x11]);  // write 1 to clear
        if(touched(0xC, 2))
        {
            // writing TCN clears it
            if(timerPeriod(t)) t.base = now; else t.base = 0;
        }
        if(touched(0x4, 2)) t.trr = get16(0x4);
        if(touched(0x8, 2)) t.tcr = get16(0x8);
        if(touched(0x0, 2))
        {
            const std::uint16_t tmr = get16(0x0);
            const auto count = timerCount(t, now);
            const bool wasRunning = timerPeriod(t) != 0;
            if(!(tmr & 1))
            {
                // RST = 0: the timer is reset
                t = Timer{};
                t.tmr = tmr;
                t.base = 0;
            }
            else
            {
                t.tmr = tmr;
                const auto p = timerPeriod(t);
                // keep the count across a change of mode
                if(p) t.base = now - std::uint64_t(wasRunning ? count : (t.base & 0xffff)) * p;
                else t.base = count;
            }
            timerChecked_[i] = now;
        }
    }
}

void Sim::setExternalIrq(int level, bool on)
{
    if(on) externalIrqs_ |= std::uint8_t(1u << level);
    else externalIrqs_ &= std::uint8_t(~(1u << level));
}

int Sim::pendingLevel() const
{
    const std::uint32_t imr = std::uint32_t(r_[IMR]) << 24 | std::uint32_t(r_[IMR + 1]) << 16 | std::uint32_t(r_[IMR + 2]) << 8 | r_[IMR + 3];
    int best = 0;
    // external IRQ lines, autovectored when their AVCR bit is set (the ISP1181 on IRQ3)
    for(int level = 1; level < 8; ++level)
        if((externalIrqs_ & (1u << level)) && !(imr & (1u << level)) && (r_[AVCR] & (1u << level)))
            best = std::max(best, level);
    auto internal = [&](bool pending, int bit, std::uint32_t icrOff) {
        if(!pending || (imr & (1u << bit))) return;
        const int level = (r_[icrOff] >> 2) & 7;
        best = std::max(best, level);
    };
    for(int i = 0; i < 2; ++i)
    {
        // a timer is autovectored only (it has no vector register): ignored unless its ICR says AVEC
        internal((timer_[i].ter & 2) && (timer_[i].tmr & 0x10) && (r_[TimerIcr[i]] & 0x80), TimerImrBit[i], TimerIcr[i]);
        internal((uisr(uart_[i]) & uart_[i].uimr) != 0, UartImrBit[i], UartIcr[i]);
    }
    return best;
}

std::uint8_t Sim::acknowledge(int level) const
{
    const std::uint32_t imr = std::uint32_t(r_[IMR]) << 24 | std::uint32_t(r_[IMR + 1]) << 16 | std::uint32_t(r_[IMR + 2]) << 8 | r_[IMR + 3];
    // the source at this level with the highest priority (ICR IP bits)
    int bestPrio = -1;
    std::uint8_t vec = std::uint8_t(24);  // spurious
    auto consider = [&](bool pending, int bit, std::uint32_t icrOff, std::uint8_t vector) {
        if(!pending || (imr & (1u << bit))) return;
        const auto icr = r_[icrOff];
        if(((icr >> 2) & 7) != level) return;
        const int prio = icr & 3;
        if(prio > bestPrio) { bestPrio = prio; vec = (icr & 0x80) ? std::uint8_t(24 + level) : vector; }
    };
    for(int i = 0; i < 2; ++i)
    {
        consider((timer_[i].ter & 2) && (timer_[i].tmr & 0x10), TimerImrBit[i], TimerIcr[i], std::uint8_t(24 + level));
        consider((uisr(uart_[i]) & uart_[i].uimr) != 0, UartImrBit[i], UartIcr[i], uart_[i].uivr);
    }
    if(bestPrio < 0 && (externalIrqs_ & (1u << level)) && (r_[AVCR] & (1u << level))) vec = std::uint8_t(24 + level);
    return vec;
}

// ---------------------------------------------------------------------------------------------------------------
// flash

namespace {
// CFI query answers: "QRY", primary command set 2 (AMD), 2^23 bytes, x16, one erase region of 128 x 64 KB
std::uint16_t cfi(std::uint32_t word)
{
    switch(word)
    {
    case 0x10: return 'Q';
    case 0x11: return 'R';
    case 0x12: return 'Y';
    case 0x13: return 0x02;
    case 0x27: return 23;
    case 0x28: return 0x01;
    case 0x2C: return 1;
    case 0x2D: return 127;
    case 0x30: return 1;
    default: return 0;
    }
}
} // namespace

Flash::Flash() : mem_(Size, 0xff) {}

std::uint32_t Flash::read(std::uint32_t off, int size)
{
    off &= Size - 1;
    auto word = [&](std::uint16_t w) -> std::uint32_t {
        if(size == 2) return w;
        if(size == 4) return std::uint32_t(w) << 16 | w;
        return (off & 1) ? (w & 0xff) : (w >> 8);
    };
    switch(mode_)
    {
    case Mode::Cfi:
        {
            const auto w = cfi((off >> 1) & 0xff);
            return size == 1 ? ((off & 1) ? w : 0) : word(w);
        }
    case Mode::Id: return word(((off >> 1) & 0xff) == 0 ? 0x0001 : ((off >> 1) & 0xff) == 1 ? 0x22F9 : 0);
    case Mode::Status: mode_ = Mode::Read; return size == 1 ? 0x80 : 0x0080;
    case Mode::Read:
    default:
        {
            std::uint32_t v = 0;
            for(int k = 0; k < size; ++k) v = v << 8 | mem_[(off + std::uint32_t(k)) & (Size - 1)];
            return v;
        }
    }
}

void Flash::erase(std::uint32_t off, bool chip)
{
    if(chip)
        std::fill(mem_.begin(), mem_.end(), 0xff);
    else
    {
        const auto base = off & ~0xffffu & (Size - 1);
        std::fill(mem_.begin() + base, mem_.begin() + base + 0x10000, 0xff);
    }
    dirty_ = true;
}

void Flash::write(std::uint32_t off, int size, std::uint32_t value)
{
    off &= Size - 1;
    const std::uint32_t w = off >> 1;
    if(state_ == State::Program)
    {
        for(int k = 0; k < size; ++k)
            mem_[(off + std::uint32_t(k)) & (Size - 1)] &= std::uint8_t(value >> (8 * (size - 1 - k)));
        state_ = State::Idle;
        ++programs_;
        dirty_ = true;
        return;
    }
    const auto v = value & 0xff;
    if(bypass_)  // unlock bypass (AA 55 20): A0 data, 80 30/10 erase, 90 00 exit [C] 0x30003DA0
    {
        if(state_ == State::BypassExit) { bypass_ = v != 0x00; state_ = State::Idle; }
        else if(state_ == State::BypassErase && (v == 0x30 || v == 0x10)) { erase(off, v == 0x10); state_ = State::Idle; }
        else if(v == 0xA0) state_ = State::Program;
        else if(v == 0x80) state_ = State::BypassErase;
        else if(v == 0x90) state_ = State::BypassExit;
        else state_ = State::Idle;
        return;
    }
    if(v == 0xF0 || v == 0xFF) { state_ = State::Idle; mode_ = Mode::Read; }
    else if(state_ == State::Unlock2 && v == 0x20) { state_ = State::Idle; bypass_ = true; }
    else if(v == 0x98 && (w & 0xff) == 0x55) { state_ = State::Idle; mode_ = Mode::Cfi; }
    else if(state_ == State::Idle && (w & 0x7ff) == 0x555 && v == 0xAA) state_ = State::Unlock1;
    else if(state_ == State::Unlock1 && (w & 0x7ff) == 0x2AA && v == 0x55) state_ = State::Unlock2;
    else if(state_ == State::Unlock2 && v == 0x90) { state_ = State::Idle; mode_ = Mode::Id; }
    else if(state_ == State::Unlock2 && v == 0xA0) state_ = State::Program;
    else if(state_ == State::Unlock2 && v == 0x80) state_ = State::Erase1;
    else if(state_ == State::Erase1 && v == 0xAA) state_ = State::Erase2;
    else if(state_ == State::Erase2 && v == 0x55) state_ = State::Erase3;
    else if(state_ == State::Erase3 && (v == 0x30 || v == 0x10)) { erase(off, v == 0x10); state_ = State::Idle; }
    else state_ = State::Idle;
}

// ---------------------------------------------------------------------------------------------------------------
// ISP1181. Commands: B0 unlock, B2/B3 scratch, B4 frame, B5 chip ID, B6/B7 address, B8/B9 mode, BA/BB hardware
// config, C0 interrupt register (4 bytes, LSB first), C2/C3 interrupt enable, F0-F3 DMA, F4 acknowledge setup, F6
// reset; per endpoint index i (0 = EP0 OUT, 1 = EP0 IN, 2.. = EP1..): 0x00+i write buffer (LE16 length, data),
// 0x10+i read buffer, 0x20+i/0x30+i write/read config, 0x50+i read status (clears its interrupt bit), 0x60+i
// validate, 0x70+i clear, 0xD0+i check status. Interrupt register: bit 0 bus reset, bit 8+i endpoint i.

std::uint32_t Isp1181::read(std::uint32_t off, int size)
{
    std::uint32_t v = 0;
    for(int k = 0; k < size; ++k)
    {
        std::uint8_t b = 0;
        if(!(off & 0x10) && !rbuf_.empty()) { b = rbuf_.front(); rbuf_.pop_front(); }
        v = v << 8 | b;
    }
    return v;
}

void Isp1181::write(std::uint32_t off, int size, std::uint32_t value)
{
    for(int k = 0; k < size; ++k)
    {
        const auto b = std::uint8_t(value >> (8 * (size - 1 - k)));
        if(off & 0x10) command(b); else data(b);
    }
}

void Isp1181::out(int ep, const std::uint8_t* data, std::size_t n, bool setup)
{
    outq_[std::size_t(ep)].push_back({std::vector<std::uint8_t>(data, data + n), setup});
    load(ep);
}

void Isp1181::load(int i)
{
    auto& q = outq_[std::size_t(i)];
    if(!outFull_[std::size_t(i)] && !q.empty())
    {
        outbuf_[std::size_t(i)] = std::move(q.front());
        q.pop_front();
        outFull_[std::size_t(i)] = true;
        intreg_ |= 1u << (8 + i);
    }
}

void Isp1181::attach()
{
    attached_ = true;
    for(int i = 0; i < 16; ++i)
    {
        for(auto& d : held_[std::size_t(i)])
        {
            inPackets_.emplace_back(i, std::move(d));
            intreg_ |= 1u << (8 + i);
        }
        held_[std::size_t(i)].clear();
    }
}

bool Isp1181::pollBulkIn(std::vector<std::uint8_t>& packet)
{
    if(bulkIn_.empty()) return false;
    intreg_ |= 1u << (8 + 3);
    packet = std::move(bulkIn_.front());
    bulkIn_.pop_front();
    return true;
}

std::vector<std::uint8_t> Isp1181::takeInBuffer(int i)
{
    auto b = std::move(inbuf_[std::size_t(i)]);
    inbuf_[std::size_t(i)].clear();
    if(b.size() >= 2)
    {
        const std::size_t n = std::size_t(b[0]) | std::size_t(b[1]) << 8;
        return {b.begin() + 2, b.begin() + 2 + std::ptrdiff_t(std::min(n, b.size() - 2))};
    }
    return {};
}

void Isp1181::command(std::uint8_t c)
{
    cmd_ = c;
    haveCmd_ = true;
    wbuf_.clear();
    rbuf_.clear();
    const int i = c & 0x0f;
    const auto ui = std::size_t(i);
    auto put32 = [&](std::uint32_t v) { for(int k = 0; k < 4; ++k) rbuf_.push_back(std::uint8_t(v >> (8 * k))); };
    if(c == 0xC0)
    {
        put32(intreg_);
        intreg_ &= ~0xffu;  // bus events clear on read; endpoint bits on a status read
    }
    else if(c == 0xB5) { rbuf_.push_back(0x81); rbuf_.push_back(0x81); }
    else if(c == 0xB4 || c == 0xB3 || c == 0xBB) { rbuf_.push_back(0); rbuf_.push_back(0); }
    else if(c == 0xB7 || c == 0xB9) rbuf_.push_back(0);
    else if(c == 0xC3) put32(inten_);
    else if(c >= 0x10 && c <= 0x1F)
    {
        const auto& p = outbuf_[ui];
        const std::size_t n = outFull_[ui] ? p.data.size() : 0;
        rbuf_.push_back(std::uint8_t(n));
        rbuf_.push_back(std::uint8_t(n >> 8));
        if(outFull_[ui]) rbuf_.insert(rbuf_.end(), p.data.begin(), p.data.end());
    }
    else if(c >= 0x30 && c <= 0x3F) rbuf_.push_back(epcfg_[ui]);
    else if((c >= 0x50 && c <= 0x5F) || (c >= 0xD0 && c <= 0xDF))
    {
        std::uint8_t st = 0;
        if(outFull_[ui]) st |= 0x20 | (outbuf_[ui].setup ? 0x04 : 0);  // EPFULL0, SETUPT
        rbuf_.push_back(st);
        if(c < 0xD0) intreg_ &= ~(1u << (8 + i));
    }
    else if(c >= 0x60 && c <= 0x6F)
    {
        // validate an IN buffer: "sent" once a host listens
        auto d = takeInBuffer(i);
        if(i == 3)
            bulkIn_.push_back(std::move(d));  // bulk-IN: the host polls it only while it expects data
        else if(attached_)
        {
            inPackets_.emplace_back(i, std::move(d));
            intreg_ |= 1u << (8 + i);
        }
        else
            held_[ui].push_back(std::move(d));  // no host: the buffer stays full, no completion
    }
    else if(c >= 0x70 && c <= 0x7F)
    {
        // clear an OUT buffer: the next packet
        outFull_[ui] = false;
        intreg_ &= ~(1u << (8 + i));
        load(i);
    }
    else if(c == 0xF6) intreg_ = 0;
    else if(c == 0xF4 && outFull_[0] && outbuf_[0].setup)
    {
        // acknowledge setup: the SETUP packet is consumed
        outFull_[0] = false;
        load(0);
    }
}

void Isp1181::data(std::uint8_t b)
{
    if(!haveCmd_) return;
    const std::uint8_t c = cmd_;
    wbuf_.push_back(b);
    if(c <= 0x0F)
        inbuf_[c] = wbuf_;
    else if(c >= 0x20 && c <= 0x2F)
        epcfg_[c & 0x0f] = b;
    else if(c == 0xC2 && wbuf_.size() == 4)
        inten_ = std::uint32_t(wbuf_[0]) | std::uint32_t(wbuf_[1]) << 8 | std::uint32_t(wbuf_[2]) << 16 | std::uint32_t(wbuf_[3]) << 24;
}

} // namespace g2emu
