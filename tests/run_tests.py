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


def tempo_ok(got, want, tol=2.0, allow_half=False):
    if abs(got - want) <= tol:
        return True
    return allow_half and abs(got - want / 2) <= tol / 2


# ---- tempo at a range of speeds
for bpm in (75, 90, 100, 110, 120, 128, 140, 150, 160):
    r = analyze(write_wav(f"drums_{bpm}", drums(bpm)))
    check(f"tempo {bpm} BPM", tempo_ok(r["bpm"], bpm), f"read {r['bpm']} (per second: {r['bpm_series'][3::4]})")

r = analyze(write_wav("drums_174", drums(174)))
check("tempo 174 BPM (174 or half-time 87)", tempo_ok(r["bpm"], 174, allow_half=True), f"read {r['bpm']}")

# ---- tempo must not depend on volume
for level, label in ((0.9, "loud"), (0.02, "quiet -34 dB"), (0.002, "very quiet -54 dB")):
    r = analyze(write_wav(f"drums_120_{level}", drums(120, level=level)))
    check(f"tempo 120 BPM, {label}", tempo_ok(r["bpm"], 120), f"read {r['bpm']}")

# ---- other shapes of music and other sample rates
r = analyze(write_wav("break_140", drums(140, pattern="break")))
check("tempo 140 BPM, broken beat", tempo_ok(r["bpm"], 140, allow_half=True), f"read {r['bpm']}")
r = analyze(write_wav("nopad_120", drums(120, pad=False)))
check("tempo 120 BPM, drums only", tempo_ok(r["bpm"], 120), f"read {r['bpm']}")
for sr in (44100, 96000):
    r = analyze(write_wav(f"drums_120_{sr}", drums(120, sr=sr), sr=sr))
    check(f"tempo 120 BPM at {sr} Hz", tempo_ok(r["bpm"], 120), f"read {r['bpm']}")
r = analyze(write_wav("mono_120", drums(120), channels=1))
check("tempo 120 BPM, mono file", tempo_ok(r["bpm"], 120), f"read {r['bpm']}")

# ---- the tempo shows up within a few seconds and stays put
r = analyze(write_wav("drums_120_long", drums(120, seconds=40)))
first = next((i + 1 for i, b in enumerate(r["bpm_series"]) if abs(b - 120) <= 2), None)
check("tempo appears within 8 s", first is not None and first <= 8, f"first correct at {first} s")
settled = r["bpm_series"][10:]
check("tempo stays steady", all(abs(b - 120) <= 2 for b in settled), f"range {min(settled)} to {max(settled)}")

# ---- beat pulses: about one per kick on a plain four-to-the-floor beat (24 s at 120 BPM = 48 kicks)
r = analyze(write_wav("beats_120", drums(120, pad=False)))
check("beat pulses match the kicks", 40 <= r["beats"] <= 56, f"{r['beats']} pulses for 48 kicks")

# ---- no rhythm, no tempo
sr = 48000
t = np.arange(sr * 12) / sr
r = analyze(write_wav("silence", np.zeros(sr * 12)))
check("silence: no tempo, no bars", r["bpm"] == 0 and r["lv_max"] == 0 and r["beats"] == 0, f"bpm {r['bpm']} max bar {r['lv_max']} pulses {r['beats']}")
r = analyze(write_wav("noise", np.random.default_rng(1).standard_normal(sr * 12) * 0.1))
check("steady noise: no tempo, almost no pulses", r["bpm"] == 0 and r["beats"] <= 3, f"bpm {r['bpm']}, pulses {r['beats']}")
r = analyze(write_wav("tone", np.sin(2 * np.pi * 440 * t) * 0.3))
check("steady tone: no tempo, no pulses", r["bpm"] == 0 and r["beats"] <= 1, f"bpm {r['bpm']}, pulses {r['beats']}")

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

# ---- loudness readout
r = analyze(write_wav("sine_level", np.sin(2 * np.pi * 1000 * t) * 0.1))
check("level readout", abs(r["rms_db"] - (-23.0)) <= 0.5, f"read {r['rms_db']} dB, expected -23.0")

print(f"\n{sum(results)} of {len(results)} passed")
sys.exit(0 if all(results) else 1)
