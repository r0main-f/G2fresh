#!/usr/bin/env python3
"""Use the emulated G2 as a black box: patch in, DAC audio out.

  nice -n 10 g2blackbox.py JOBS.json --out DIR

Boots the user's OS once in g2hostemu.py's emulator (four emulated DSPs, our
protocol client on the emulated USB), then for each job of JOBS.json
  {"name": "sine69", "patch": "a.pch2", "settle": 1.5, "seconds": 1.0,
   "notes": [[0.2, 64, 1], [0.7, 64, 0]]}
uploads the patch to slot A, waits until the client is idle plus `settle`
emulated seconds (the output level ramps after an upload), plays the notes
(time from the start of the recording, note number, on/off) and records the
DACs to DIR/<name>/dac.wav (4 channels, 96 kHz float) and dac.json.

Only the DAC audio is written: no DSP memories or host-port streams (this is
the black-box reference of the native engine, re/notes/native-engine.md).
Needs the venv and libraries of g2hostemu.py.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import g2hostemu as H  # noqa: E402


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('jobs')
    ap.add_argument('--out', required=True)
    ap.add_argument('--fw', default=os.path.join(H.ROOT, 'original', 'firmware'))
    ap.add_argument('--usb-start', type=int, default=12000)
    ap.add_argument('--ms-per-slice', type=float, default=0.125)
    ap.add_argument('--protobridge', default=os.path.join(H.ROOT, 'build', 'emu', 'libg2protobridge.dylib'))
    ap.add_argument('--bridge', default=os.path.join(H.ROOT, 'build', 'emu', 'libg2dspbridge.dylib'))
    H.add_audio_args(ap)
    args = ap.parse_args(argv)
    with open(args.jobs) as f:
        jobs = json.load(f)
    base = os.path.dirname(os.path.abspath(args.jobs))

    H.DspBridge.load(args.bridge)
    emu = H.Emu(args.fw, dsps=4)
    emu.log.once = lambda *a, **k: None
    emu.audio_links = H.wire_audio([p.dsp for p in emu.host.ports[:4]], args)
    emu.start()
    host = H.UsbHost(emu, args.protobridge)
    import ctypes
    host.lib.g2p_play_note.restype = None
    host.lib.g2p_play_note.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    host.lib.g2p_send_kbd_performance.restype = ctypes.c_int
    host.lib.g2p_send_kbd_performance.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    dsps = [p.dsp for p in emu.host.ports if p.dsp is not None]
    st = {'phase': 'boot', 'n': 0, 'job': -1, 't0': time.time()}

    def next_job():
        st['job'] += 1
        if st['job'] >= len(jobs):
            return False
        job = jobs[st['job']]
        path = job['patch'] if os.path.isabs(job['patch']) else os.path.join(base, job['patch'])
        print(f'[{time.time() - st["t0"]:.0f} s] job {job["name"]}: sending {path}', flush=True)
        if host.lib.g2p_send_patch(host.h, 0, path.encode(), job['name'][:16].encode()):
            raise SystemExit('patch not sent')
        st['phase'] = 'upload'
        return True

    def until(em):
        st['n'] += 1
        if st['phase'] == 'boot' and em.slices >= args.usb_start:
            host.start()
            st['phase'] = 'sync'
        host.step(args.ms_per_slice)
        ph = st['phase']
        if ph == 'sync' and host.lib.g2p_synced(host.h):
            # The synth's default performance has the keyboard off on every slot: send one with
            # slot A focused and keyboard-enabled, so that the notes (PlayNote) reach slot A.
            print(f'[{time.time() - st["t0"]:.0f} s] synced; sending a performance with the keyboard on slot A',
                  flush=True)
            first = jobs[0]['patch'] if os.path.isabs(jobs[0]['patch']) else os.path.join(base, jobs[0]['patch'])
            if host.lib.g2p_send_kbd_performance(host.h, first.encode()):
                raise SystemExit('performance not sent')
            st['phase'] = 'perf'
        elif ph == 'perf' and host.lib.g2p_idle(host.h):
            return not next_job()
        if ph == 'upload' and host.lib.g2p_idle(host.h):
            job = jobs[st['job']]
            st['rec'] = H.AudioRecorder(dsps, emu.audio_links, args, host_cpu=time.thread_time)
            st['settle_end'] = st['rec'].frames() + int(job.get('settle', 1.5) * H.FRAME_RATE)
            st['phase'] = 'settle'
        elif ph == 'settle' and st['rec'].frames() >= st['settle_end']:
            job = jobs[st['job']]
            st['rec'].start(int(job.get('seconds', 1.0) * H.FRAME_RATE))
            st['rec_start'] = st['rec'].frames()
            st['notes'] = sorted(job.get('notes', []))
            st['note_log'] = []
            st['traffic_mark'] = len(host.traffic)
            st['phase'] = 'record'
        elif ph == 'record':
            rec = st['rec']
            t = (rec.frames() - st['rec_start']) / H.FRAME_RATE
            while st['notes'] and st['notes'][0][0] <= t:
                tn, note, on = st['notes'].pop(0)
                host.lib.g2p_play_note(host.h, int(note), int(on))
                st['note_log'].append([t, note, on])
            if st['n'] % 50 == 0 and rec.poll():
                rec.stop()
                job = jobs[st['job']]
                d = os.path.join(args.out, job['name'])
                os.makedirs(d, exist_ok=True)
                rec.report['notes_sent'] = st['note_log']
                rec.report['usb_during_record'] = host.traffic[st['traffic_mark']:]
                rec.save(d)
                return not next_job()
        return False

    emu.run(steps=10 ** 13, slice_instr=20000, dsp_instr=20000, until=until)
    H.DspBridge.lib.g2dsp_shutdown_all()
    print(f'done in {time.time() - st["t0"]:.0f} s', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
