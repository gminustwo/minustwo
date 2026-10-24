#!/usr/bin/env python3
"""Movie pacing as it reached the screen, from an MZ_PRESENT_TRACE run.

    python tools/present_report.py out/ptrace/run.log out/ptrace.bin D:/Games/ps4/CUSA02046-GODZILLA/01_op_en.mp4

The game's frames (samples written by the presenter) are matched to the movie's own frames
(decoded with ffmpeg and sampled at the same points). Then, per movie frame: how many game frames
(flips) showed it and how many display refreshes it stayed on screen (each present lands on the
first display vblank after it). Backward steps mean an older picture came back on screen.
"""
import argparse
import re
import shutil
import struct
import subprocess
import sys
from collections import Counter

import numpy as np

COLS, ROWS = 48, 27


def movie_span(path):
    """First and last video frame hand-out of an MZ_AVPLAYER_TRACE=1 log (same clock), or None."""
    pat = re.compile(r"AvPlayer trace: ([\d.]+) ms video ")
    times = [float(m.group(1)) for line in open(path, encoding="utf-8", errors="replace")
             if (m := pat.match(line))]
    return (times[0], times[-1]) if times else None


def read_log(path):
    flips, presents, vblanks = [], [], []
    pat = re.compile(r"PresentTrace: (\w+) ?(\d+)? ([\d.]+)")
    for line in open(path, encoding="utf-8", errors="replace"):
        if not line.startswith("PresentTrace:"):
            continue
        m = pat.match(line)
        if not m:
            continue
        kind, seq, t = m.group(1), m.group(2), float(m.group(3))
        if kind == "vblank":
            vblanks.append(t)
        elif kind == "flip":
            flips.append((int(seq), t))
        elif kind in ("present", "redraw"):
            presents.append((int(seq), t, kind))
    return flips, presents, np.array(vblanks)


def read_samples(path):
    data = open(path, "rb").read()
    out = {}
    pos = 0
    while pos + 20 <= len(data):
        seq, made, texel = struct.unpack_from("<QdI", data, pos)
        pos += 20
        size = COLS * ROWS * texel
        px = np.frombuffer(data, np.uint8, size, pos).reshape(ROWS * COLS, texel)
        pos += size
        out[seq] = px[:, :3].astype(np.float32)
    return out


def movie_samples(movie):
    ffmpeg = shutil.which("ffmpeg") or "C:/msys64/clang64/bin/ffmpeg.exe"
    # Nearest-neighbour scaling picks the same pixel centres the presenter samples.
    cmd = [ffmpeg, "-v", "error", "-i", movie, "-vf",
           f"scale={COLS}:{ROWS}:flags=neighbor:in_color_matrix=bt709,format=rgb24",
           "-f", "rawvideo", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    frames = np.frombuffer(raw, np.uint8).reshape(-1, ROWS * COLS, 3).astype(np.float32)
    return frames


def match(game, movie):
    """Best movie frame per game frame: (index, error, error of the best other frame)."""
    seqs = sorted(game)
    g = np.stack([game[s].reshape(-1) for s in seqs])
    m = movie.reshape(len(movie), -1)
    best = {}
    for order, name in (((0, 1, 2), "rgb"), ((2, 1, 0), "bgr")):
        gg = g.reshape(len(seqs), -1, 3)[:, :, order].reshape(len(seqs), -1)
        d = (gg * gg).sum(1)[:, None] + (m * m).sum(1)[None, :] - 2 * gg @ m.T
        d = np.maximum(d, 0) / gg.shape[1]
        idx = d.argmin(1)
        err = d[np.arange(len(seqs)), idx]
        best[name] = (err.mean(), idx, err, d)
    name = min(best, key=lambda k: best[k][0])
    _, idx, err, d = best[name]
    return seqs, idx, err, d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("samples")
    ap.add_argument("movie")
    ap.add_argument("--max-error", type=float, default=150.0,
                    help="mean squared error above which a game frame is not the movie")
    args = ap.parse_args()

    flips, presents, vblanks = read_log(args.log)
    # With MZ_AVPLAYER_TRACE=1 only what was drawn while the movie played (logos and menus can
    # look like a dark movie frame).
    game = read_samples(args.samples)
    if span := movie_span(args.log):
        flips = [(s, t) for s, t in flips if span[0] <= t <= span[1] + 100]
        kept = {s for s, _ in flips}
        presents = [p for p in presents if p[0] in kept]
        game = {s: v for s, v in game.items() if s in kept}
    movie = movie_samples(args.movie)
    seqs, idx, err, d = match(game, movie)
    shown = {s: (int(i), float(e)) for s, i, e in zip(seqs, idx, err)}
    movie_seqs = [s for s in seqs if shown[s][1] <= args.max_error]
    if not movie_seqs:
        sys.exit("no game frame looks like the movie")
    first, last = movie_seqs[0], movie_seqs[-1]
    print(f"movie: {len(movie)} frames; game frames {first}..{last} show it "
          f"(median match error {np.median([shown[s][1] for s in movie_seqs]):.1f})")

    # The 60 Hz layer: the movie frame each flip showed, in flip order.
    flip_frames = [(s, t, shown[s][0]) for s, t in flips if first <= s <= last and s in shown]
    runs = []  # (movie frame, flips it was shown, first flip time)
    back = []
    for s, t, f in flip_frames:
        if runs and runs[-1][0] == f:
            runs[-1][1] += 1
        else:
            if runs and f < runs[-1][0]:
                back.append((t, runs[-1][0], f))
            runs.append([f, 1, t])
    steps = Counter(b[0] - a[0] for a, b in zip(runs, runs[1:]))
    print("flips per movie frame:", dict(sorted(Counter(r[1] for r in runs).items())))
    print("movie frame steps between pictures:", dict(sorted(steps.items())))
    if back:
        print(f"backward steps: {len(back)}; first:",
              [(round(t / 1000, 2), a, b) for t, a, b in back[:10]])
    odd = [(round(r[2] / 1000, 2), r[0], r[1]) for r in runs[1:-1] if r[1] != 2]
    print(f"movie frames not shown for 2 flips: {len(odd)}; first:", odd[:20])

    # The display: each present lands on the first vblank after it (and after the previous one).
    if len(vblanks):
        start = flip_frames[0][1] if flip_frames else 0
        end = flip_frames[-1][1] if flip_frames else 0
        on_screen = []  # (vblank index, movie frame)
        last_v = -1
        for s, t, kind in presents:
            if s not in shown or not (start - 100 <= t <= end + 100):
                continue
            v = int(np.searchsorted(vblanks, t + 0.3))
            v = max(v, last_v + 1)
            if v >= len(vblanks):
                break
            last_v = v
            on_screen.append((v, shown[s][0]))
        spans = []  # (movie frame, refreshes, vblank time)
        for (v0, f0), (v1, f1) in zip(on_screen, on_screen[1:]):
            if spans and spans[-1][0] == f0:
                spans[-1][1] += v1 - v0
            else:
                spans.append([f0, v1 - v0, vblanks[v0]])
        period = np.median(np.diff(vblanks))
        print(f"display: {1000 / period:.3f} Hz; refreshes per movie frame "
              f"(29.97 fps would be {1000 / 29.97 / period:.2f}):",
              dict(sorted(Counter(sp[1] for sp in spans[1:-1]).items())))
        ms = np.array([sp[1] * period for sp in spans[1:-1]])
        if len(ms):
            print(f"  on-screen time per movie frame: mean {ms.mean():.2f} ms, "
                  f"spread (std) {ms.std():.2f} ms, min {ms.min():.1f}, max {ms.max():.1f}")
        far = [(round(sp[2] / 1000, 2), sp[0], sp[1]) for sp in spans[1:-1]
               if abs(sp[1] - 1000 / 29.97 / period) > 1.0]
        print(f"  movie frames more than one refresh off: {len(far)}; first:", far[:20])


if __name__ == "__main__":
    main()
