#!/usr/bin/env python3
"""Decode real timeout output; exercise inactivity, refresh and recovery."""
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from test_support import ROOT, run, caption_port, validate

BIN = ROOT / 'cc_injector_youtube_timeout'


def events(helper, output):
    # FFmpeg emits an ASS event header even for an empty display. Inspect
    # its text field rather than treating that metadata as visible captions.
    return [(int(timestamp) / 1000, screen.split(',', 8)[-1])
            for timestamp, screen in (line.split('\t', 1)
                                      for line in run(helper, output, '--timeline').splitlines())]


def screen_at(timeline, seconds):
    previous = [screen for timestamp, screen in timeline if timestamp <= seconds]
    assert previous, timeline
    return previous[-1]


def main():
    with tempfile.TemporaryDirectory(prefix='captiongpt-timeout-') as tmp:
        folder = Path(tmp)
        source = folder / 'source.mp4'
        run('ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
            'color=blue:size=320x180:rate=30', '-t', '24', '-c:v',
            'libx264', '-preset', 'ultrafast', '-threads', '4', source)
        helper = folder / 'read_captions'
        flags = run('pkg-config', '--cflags', '--libs',
                    'libavformat', 'libavcodec', 'libavutil').split()
        run('g++', '-O2', '-std=c++17', ROOT / 'tests/read_embedded_captions.cpp',
            *flags, '-o', helper)
        for mode in ['rollup', 'popon']:
            print(f'Testing ten-second timeout and recovery: {mode}...', flush=True)
            output = folder / f'{mode}.flv'
            port = caption_port()
            with (folder / f'{mode}.log').open('w+') as log:
                encoder = subprocess.Popen([str(BIN), '-i', str(source), '-o', str(output),
                    '--pace=on', '--preset=ultrafast', f'--cc-port={port}',
                    f'--cc-mode={mode}'], stdout=log, stderr=log)
                try:
                    time.sleep(1.5)
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                        caption = b'OLD TOP\nOLD BOTTOM'
                        sender.sendto(caption, ('127.0.0.1', port))
                        time.sleep(4)
                        sender.sendto(caption, ('127.0.0.1', port))  # refresh duplicate
                        time.sleep(13.5)
                        sender.sendto(b'NEW CAPTION', ('127.0.0.1', port))
                    assert encoder.wait(timeout=15) == 0
                    log.seek(0)
                    assert log.read().count('cleared after 10 seconds') == 1
                finally:
                    if encoder.poll() is None:
                        encoder.kill()
                        encoder.wait()
            validate(output, 24)
            timeline = events(helper, output)
            before = screen_at(timeline, 14)
            assert 'OLD TOP' in before and 'OLD BOTTOM' in before, timeline
            assert not screen_at(timeline, 18), timeline
            clears = [timestamp for timestamp, screen in timeline if not screen and timestamp > 5]
            assert clears and 15 <= clears[0] <= 17, timeline
            after = screen_at(timeline, 23)
            assert 'NEW CAPTION' in after and 'OLD' not in after, timeline
            print(f'  PASS {mode}: duplicate refresh, ten-second clear, both buffers erased, fresh caption recovery', flush=True)
        # The startup caption also expires when no UDP data ever arrives.
        bootstrap = folder / 'bootstrap.flv'
        short_source = folder / 'short.mp4'
        run('ffmpeg', '-v', 'error', '-i', source, '-t', '13', '-c', 'copy', short_source)
        run(BIN, '-i', short_source, '-o', bootstrap, '--cc-port=0', '--pace=on', '--preset=ultrafast')
        timeline = events(helper, bootstrap)
        assert 'CC ONLINE' in screen_at(timeline, 5), timeline
        assert not screen_at(timeline, 12), timeline
        print('PASS: timeout variant, both modes, duplicate refresh, recovery and startup expiry', flush=True)


if __name__ == '__main__':
    main()
