#!/usr/bin/env python3
"""Unattended test run: starts the game through run.py, presses pad buttons on a schedule
(MZ_PAD_FILE), dumps the screen at intervals (MZ_FRAME_DUMP_TRIGGER), then writes a contact sheet
of the frames and the loudness of what each audio port played (MZ_AUDIO_DUMP).

    python tools/autoplay.py --game D:/Games/CUSA02046 --seconds 120 --every 10 \\
        --press 40:cross --press 45:cross --hold 60-70:ly=0

--press T:BUTTONS taps the buttons (space-separated pad file tokens) for 0.3 s at T seconds;
--hold A-B:TOKENS holds them from A to B. Dialogs answer themselves (MZ_AUTO_DIALOGS=1).
Results go to --out (default out/autoplay): sheet.png, frames, run.log and audio levels.
"""
import argparse
import array
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

PORT = Path(__file__).resolve().parent.parent


def parse_press(text):
    at, tokens = text.split(':', 1)
    return float(at), float(at) + 0.3, tokens.replace(',', ' ')


def parse_hold(text):
    span, tokens = text.split(':', 1)
    start, end = span.split('-')
    return float(start), float(end), tokens.replace(',', ' ')


def write_pad(path, tokens):
    path.write_text(tokens + '\n' if tokens else '')


def kill_tree(process):
    if sys.platform == 'win32':
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)], capture_output=True)
    else:
        process.kill()


def sheet(frames, out):
    from PIL import Image
    if not frames:
        return None
    columns = 3
    images = [Image.open(f).convert('RGB').resize((384, 216)) for f in frames]
    rows = (len(images) + columns - 1) // columns
    canvas = Image.new('RGB', (384 * columns, 216 * rows))
    for i, image in enumerate(images):
        canvas.paste(image, ((i % columns) * 384, (i // columns) * 216))
    canvas.save(out)
    return out


def audio_levels(prefix):
    lines = []
    for path in sorted(prefix.parent.glob(prefix.name + '.port*')):
        data = path.read_bytes()
        samples = array.array('f' if path.name.endswith('f32') else 'h')
        samples.frombytes(data[:len(data) - len(data) % samples.itemsize])
        scale = 1.0 if samples.typecode == 'f' else 32768.0
        if not samples:
            lines.append(f'{path.name}: empty')
            continue
        peak = max(abs(s) for s in samples) / scale
        rms = math.sqrt(sum(s * s for s in samples) / len(samples)) / scale
        loud = sum(1 for s in samples if abs(s) / scale > 0.01) / len(samples)
        lines.append(f'{path.name}: {len(data)} bytes, peak {peak:.3f}, rms {rms:.4f}, {loud:.0%} of samples above -40 dB')
    return lines


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--game', default=os.environ.get('MZ_GAME_DIR'))
    p.add_argument('--seconds', type=float, default=90)
    p.add_argument('--every', type=float, default=10, help='seconds between screen dumps')
    p.add_argument('--press', action='append', default=[])
    p.add_argument('--hold', action='append', default=[])
    p.add_argument('--out', type=Path, default=PORT / 'out' / 'autoplay')
    p.add_argument('--env', action='append', default=[], help='extra NAME=VALUE for the game')
    a = p.parse_args()
    if not a.game:
        p.error('--game or MZ_GAME_DIR is needed')
    shutil.rmtree(a.out, ignore_errors=True)
    a.out.mkdir(parents=True)
    pad, trigger = a.out / 'pad.txt', a.out / 'trigger'
    write_pad(pad, '')
    env = {**os.environ, 'MZ_GAME_DIR': a.game, 'MZ_PAD_FILE': str(pad), 'MZ_FRAME_DUMP_TRIGGER': str(trigger),
           'MZ_DUMP_DIR': str(a.out), 'MZ_AUTO_DIALOGS': '1', 'MZ_AUDIO_DUMP': str(a.out / 'audio'),
           'PYTHONIOENCODING': 'utf-8'}
    for item in a.env:
        name, value = item.split('=', 1)
        env[name] = value
    events = [parse_press(x) for x in a.press] + [parse_hold(x) for x in a.hold]
    log = open(a.out / 'run.log', 'wb')
    process = subprocess.Popen([sys.executable, str(PORT / 'run.py')], cwd=PORT, env=env, stdout=log,
                               stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    start = time.monotonic()
    next_dump = a.every
    current = None
    try:
        while process.poll() is None:
            now = time.monotonic() - start
            if now >= a.seconds:
                break
            tokens = ' '.join(t for s, e, t in events if s <= now < e)
            if tokens != current:
                write_pad(pad, tokens)
                current = tokens
            if now >= next_dump:
                trigger.touch()
                next_dump += a.every
            time.sleep(0.05)
    finally:
        kill_tree(process)
        log.close()
    time.sleep(1)
    subprocess.run([sys.executable, str(PORT / 'tools' / 'raw_to_png.py'), str(a.out)], capture_output=True)
    frames = sorted(a.out.glob('output_*.png'))
    made = sheet(frames, a.out / 'sheet.png')
    print(f'Exit: {process.returncode}; {len(frames)} frames' + (f'; sheet {made}' if made else ''))
    for line in audio_levels(a.out / 'audio'):
        print('Audio:', line)
    text = (a.out / 'run.log').read_text(encoding='utf-8', errors='replace').splitlines()
    for line in text:
        if line.startswith(('STOP', 'Fault', 'API:', 'Caller')) or line.startswith('  #'):
            print(line)


if __name__ == '__main__':
    main()
