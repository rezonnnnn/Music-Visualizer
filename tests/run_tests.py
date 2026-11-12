"""Analysis tests: generate signals with known answers, run them through the analyzer, check what it reports.

Usage: python tests/run_tests.py <command that runs the test program...>
  native:  python tests/run_tests.py build/analysis_test.exe
  wasm:    python tests/run_tests.py node tests/wasi_run.mjs build/analysis_test.wasm
Needs numpy.
"""
import json
import math
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

CMD = sys.argv[1:]
TMP = Path(tempfile.mkdtemp(prefix="mvtest_"))
results = []


def write_wav(name, mono, sr=48000, channels=2):
    path = TMP / f"{name}.wav"
    data = np.clip(mono, -1, 1)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes((np.repeat(data[:, None], channels, axis=1) * 32767).astype("<i2").tobytes())
    return path


def analyze(path, columns=48):
    out = subprocess.run(CMD + [str(path), str(columns)], capture_output=True, text=True, check=True).stdout
    return json.loads(out.strip().splitlines()[-1])


def drums(bpm, seconds=24, sr=48000, level=0.5, pattern="four", pad=True, seed=7):
    """Kick, snare, hats and an optional sustained pad at a given tempo."""
    t = np.arange(int(sr * seconds)) / sr
    beat = 60.0 / bpm
    rng = np.random.default_rng(seed)
    noise = rng.standard_normal(t.size)
    pos = t / beat                      # position in beats
    step = np.floor(pos * 2) % 8        # eighth-note index within a bar of four beats
    ph8 = (pos * 2) % 1 * beat / 2      # seconds since the last eighth note
    kick_steps = {"four": (0, 2, 4, 6), "break": (0, 3, 5)}[pattern]
    kick_env = np.where(np.isin(step, kick_steps), np.exp(-ph8 * 18), 0)
    kick = np.sin(2 * np.pi * (48 * ph8 + 4 * (1 - np.exp(-ph8 * 40)))) * kick_env
    snare = noise * np.where(np.isin(step, (2, 6)), np.exp(-ph8 * 28), 0) * 0.45
    hat = np.diff(noise, prepend=0) * np.where(step % 2 == 1, np.exp(-ph8 * 60), 0) * 0.3
    mix = kick * 0.9 + snare + hat
    if pad:
        root = np.array([110, 87.31, 130.81, 98])[(np.floor(pos / 8) % 4).astype(int)]
        phase = 2 * np.pi * np.cumsum(root) / sr
        mix = mix + sum(np.sin(phase * k) / k for k in (1, 2, 3, 4, 6)) * 0.15 * (0.6 + 0.4 * np.sin(t * 0.6))
    return mix / np.abs(mix).max() * level


def check(name, ok, detail):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")


# ---- music fills the bars at any volume (automatic gain), sample rate and channel count
tops = {}
for level, label in ((0.9, "loud"), (0.02, "quiet -34 dB")):
    r = analyze(write_wav(f"drums_120_{level}", drums(120, seconds=12, level=level)))
    tops[label] = r["lv_top"]
    check(f"music fills the bars, {label}", r["lv_top"] > 0.8, f"tallest bar reached {r['lv_top']}")
check("loud and quiet music draw the same height", abs(tops["loud"] - tops["quiet -34 dB"]) < 0.05, f"{tops}")
# the gain stops at -60 dB so that noise is not blown up: very quiet music is smaller, but still there
r = analyze(write_wav("drums_120_vquiet", drums(120, seconds=12, level=0.002)))
check("very quiet music (-54 dB) still shows", 0.3 < r["lv_top"] < 0.8, f"tallest bar reached {r['lv_top']}")
for sr in (44100, 96000):
    r = analyze(write_wav(f"drums_120_{sr}", drums(120, seconds=12, sr=sr), sr=sr))
    check(f"music fills the bars at {sr} Hz", r["lv_top"] > 0.8, f"tallest bar reached {r['lv_top']}")
r = analyze(write_wav("mono_120", drums(120, seconds=12), channels=1))
check("music fills the bars, mono file", r["lv_top"] > 0.8, f"tallest bar reached {r['lv_top']}")

# ---- bars glide: even on hard drum hits no bar jumps far in one frame (60 frames a second)
r = analyze(write_wav("glide_120", drums(120, seconds=12, pad=False)))
check("bars glide on drum hits", 0.02 < r["max_step"] < 0.3, f"biggest move in one frame: {r['max_step']} of full height")
r = analyze(write_wav("glide_174", drums(174, seconds=12, pattern="break")))
check("bars glide on a fast broken beat", 0.02 < r["max_step"] < 0.3, f"biggest move in one frame: {r['max_step']} of full height")

# ---- silence draws nothing
sr = 48000
t = np.arange(sr * 12) / sr
r = analyze(write_wav("silence", np.zeros(sr * 12)))
check("silence: no bars", r["lv_top"] == 0, f"tallest bar {r['lv_top']}")
# ---- a sound that stops lets the bars sink back to nothing
tail = np.concatenate([np.sin(2 * np.pi * 440 * t[: sr * 4]) * 0.3, np.zeros(sr * 4)])
r = analyze(write_wav("tone_then_silence", tail))
check("bars sink to nothing after the sound stops", r["lv_top"] > 0.9 and r["lv_max"] < 0.01, f"reached {r['lv_top']}, ended at {r['lv_max']}")

# ---- pure tones land in the right column
for hz in (60, 250, 1000, 4000, 12000):
    r = analyze(write_wav(f"sine_{hz}", np.sin(2 * np.pi * hz * t) * 0.5))
    want = int(48 * math.log(hz / 30) / math.log(16000 / 30))
    far = [v for i, v in enumerate(r["lv"]) if abs(i - want) > 6]
    check(f"{hz} Hz tone lights column {want}", abs(r["peak_col"] - want) <= 1 and r["lv_max"] > 0.95 and max(far) < 0.35,
          f"peak column {r['peak_col']} ({r['peak_hz']} Hz), height {r['lv_max']}, tallest far column {max(far):.2f}")
for columns in (8, 96):
    r = analyze(write_wav("sine_1000_c", np.sin(2 * np.pi * 1000 * t) * 0.5), columns)
    want = int(columns * math.log(1000 / 30) / math.log(16000 / 30))
    check(f"1 kHz tone with {columns} columns", abs(r["peak_col"] - want) <= 1, f"peak column {r['peak_col']}, expected {want}")


print(f"\n{sum(results)} of {len(results)} passed")
sys.exit(0 if all(results) else 1)
