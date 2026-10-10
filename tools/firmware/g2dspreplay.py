#!/usr/bin/env python3
"""Replay the host-port streams that g2hostemu.py recorded (dspN.events) into
four emulated DSP56367s (emu/dspbridge) without the ColdFire OS, with their
serial audio links wired (re/notes/g2-hardware-and-emulation.md 3.7), then let
them run and record the output DSP's DAC frames.

  g2dspreplay.py EVENTS_DIR [--seconds S] [--out DIR] [--chain 3,2,1,0] [--poke N:P:ADDR=VAL] ...

EVENTS_DIR holds dsp0.events .. dsp3.events (dspN = the DSP on chip select
A(3+N)). They contain the user's own firmware's DSP code: keep them and this
tool's output out of the repository.

Each stream is replayed in order, per DSP, waiting where the OS waited: for
the DSP to take a host command (HC clear), for a word to read back (RXDF), for
HF2 after HF0 is raised and for HF2 to drop after HF0 is cleared. The OS's
timing between DSPs and its later live edits are not reproduced: this is a
test bench for the audio links, the full machine is g2hostemu.py --patch.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import g2hostemu as emu  # noqa: E402


def load_events(path):
    out = []
    with open(path) as f:
        for line in f:
            k, v = line.split()
            out.append((k, int(v, 16)))
    return out


def replay(dsp, events, pos):
    """A generator: sends the stream to one DSP, yields while it has to wait."""
    hf0 = False
    i = 0
    n = len(events)
    while i < n:
        k, v = events[i]
        if k == 'w':
            while dsp.rx_queued() > 256:
                yield
            dsp.host_write(5, (v >> 16) & 0xff)
            dsp.host_write(6, (v >> 8) & 0xff)
            dsp.host_write(7, v & 0xff)
        elif k == 'cvr':
            dsp.host_write(1, v)
            if v & 0x80:
                t0 = time.time()
                while dsp.host_read(1) & 0x80:
                    if time.time() - t0 > 5:
                        print(f'  host command {v & 0x7f:02x} not taken (event {i})', flush=True)
                        break
                    yield
        elif k == 'icr':
            dsp.host_write(0, v)
            new_hf0 = bool(v & 0x08)
            if new_hf0 != hf0:
                hf0 = new_hf0
                t0 = time.time()
                while bool(dsp.host_read(2) & 0x08) != hf0:  # HF2 follows HF0 (stage 1 handshake)
                    if time.time() - t0 > 5:
                        print(f'  no HF2 answer to HF0={int(hf0)} (event {i})', flush=True)
                        break
                    yield
        elif k == 'r5':
            t0 = time.time()
            while not dsp.host_read(2) & 0x01:
                if time.time() - t0 > 5:
                    print(f'  nothing to read back (event {i})', flush=True)
                    break
                yield
            dsp.host_read(5)
        elif k == 'r6':
            dsp.host_read(6)
        elif k == 'r7':
            dsp.host_read(7)
        i += 1
        pos[0] = i
        if i % 64 == 0:
            yield


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('events')
    ap.add_argument('--bridge', default=os.path.join(emu.ROOT, 'build', 'emu', 'libg2dspbridge.dylib'))
    ap.add_argument('--out', default=None, help='where to write the DAC WAV and stats (default: EVENTS_DIR)')
    ap.add_argument('--seconds', type=float, default=0.5, help='emulated seconds of DAC output to record')
    ap.add_argument('--settle', type=float, default=0.05, help='emulated seconds to run before recording')
    ap.add_argument('--poke', action='append', default=[],
                    help='after the replay, write a DSP word: N:SPACE:ADDR=VALUE (hex; SPACE P, X or Y), e.g. a '
                         'parameter the OS would have changed later')
    emu.add_audio_args(ap)
    args = ap.parse_args(argv)
    out = args.out or args.events
    os.makedirs(out, exist_ok=True)

    emu.DspBridge.load(args.bridge)
    dsps = [emu.DspBridge() for _ in range(4)]
    links = emu.wire_audio(dsps, args)
    streams = [load_events(os.path.join(args.events, f'dsp{n}.events')) for n in range(4)]
    t0 = time.time()
    pos = [[0] for _ in range(4)]
    gens = [replay(d, s, p) for d, s, p in zip(dsps, streams, pos)]
    live = list(range(4))
    last = time.time()
    while live:
        if time.time() - last > 5:
            last = time.time()
            print(f'[{last - t0:6.2f}s] ' + '; '.join(
                f'dsp{n}: ev {pos[n][0]} pc {d.pc():06x} rxq {d.rx_queued()} hcr {d.hcr():02x} isr {d.host_read(2):02x} '
                f'frames tx {d.tx_frames(0)}/{d.tx_frames(1)} rx {d.rx_frames(0)}/{d.rx_frames(1)}'
                for n, d in enumerate(dsps)), flush=True)
        for n in list(live):
            try:
                next(gens[n])
            except StopIteration:
                live.remove(n)
                print(f'[{time.time() - t0:6.2f}s] dsp{n}: stream replayed ({len(streams[n])} events)', flush=True)
    for p in args.poke:
        where, _, val = p.partition('=')
        n, space, addr = where.split(':')
        dsps[int(n)].mem_write('PXY'.index(space.upper()), int(addr, 16), int(val, 16))
    rec = emu.AudioRecorder(dsps, links, args)
    # Stage 1's background loop adds X:$40 to the 48-bit L:$42 each time it runs the control-rate
    # code [C]: the count of control ticks, which must be one per 4 frames (notes 3.8)
    clock = lambda d: ((d.mem_read(1, 0x42) << 24) | d.mem_read(2, 0x42), d.rx_frames(1))
    c0 = [clock(d) for d in dsps]
    rec.run(args.settle, args.seconds)
    for n, (d, (l0, f0)) in enumerate(zip(dsps, c0)):
        l1, f1 = clock(d)
        inc = d.mem_read(1, 0x40)
        if inc and f1 > f0:
            print(f'  A{3 + n}: {((l1 - l0) % (1 << 48)) / inc / (f1 - f0):.4f} control ticks per frame (0.25 expected)',
                  flush=True)
    rec.save(out)
    emu.DspBridge.lib.g2dsp_shutdown_all()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
