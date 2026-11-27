#!/usr/bin/env python3
"""Generate Spek's labelled test corpus of genuine and fake hi-res audio.

Every detection rule in the analysis engine is judged against this corpus, so
each file carries a ground-truth label in manifest.csv / manifest.json: what the
file claims to be (container sample rate and bit depth) and what it really is
(true source rate, true bit depth, lossy codec, expected verdict).

Two kinds of source material:

* Synthetic (default). Deterministic signals synthesised natively at each sample
  rate, so "genuine" really is genuine: a full-band pink noise, a music-like
  piece (harmonic notes, hi-hats, a noise bed, a quiet passage), and a sparse
  piano-like piece with almost nothing above 20 kHz but a 24-bit noise floor.
  Nothing copyrighted, so the generator (not the audio) lives in the repo.

* Real (--from FILE ...). Your own verified hi-res masters or CD rips. The same
  fakes are derived from each one. These files never leave your machine and are
  never committed.

Requires Python 3.8+, numpy, and an ffmpeg with libmp3lame, aac, libvorbis,
libopus and libsoxr (Homebrew's and Ubuntu's ffmpeg have all of them).

    tests/corpus/generate.py                    # synthetic corpus in tests/corpus/out
    tests/corpus/generate.py --out ~/corpus --from master.flac --from cdrip.flac
    tests/corpus/verify.py tests/corpus/out     # check every file matches its label
"""

import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

DURATION = 20.0  # seconds per synthetic clip
SPLICE = (8.0, 12.0)  # seconds replaced by a transcoded segment in the splice case
CORPUS_VERSION = 1

# Lossy codecs applied to a 44.1 kHz / 16-bit "CD" master. Each entry:
# id -> (ffmpeg encoder args, container extension, codec label)
LOSSY = {
    "mp3-128": (["-c:a", "libmp3lame", "-b:a", "128k"], "mp3", "MP3 128k CBR (LAME)"),
    "mp3-192": (["-c:a", "libmp3lame", "-b:a", "192k"], "mp3", "MP3 192k CBR (LAME)"),
    "mp3-320": (["-c:a", "libmp3lame", "-b:a", "320k"], "mp3", "MP3 320k CBR (LAME)"),
    "mp3-v0": (["-c:a", "libmp3lame", "-q:a", "0"], "mp3", "MP3 V0 VBR (LAME)"),
    "aac-128": (["-c:a", "aac", "-b:a", "128k"], "m4a", "AAC-LC 128k (FFmpeg)"),
    "aac-256": (["-c:a", "aac", "-b:a", "256k"], "m4a", "AAC-LC 256k (FFmpeg)"),
    "vorbis-q6": (["-c:a", "libvorbis", "-q:a", "6"], "ogg", "Vorbis q6"),
    "opus-160": (["-c:a", "libopus", "-b:a", "160k"], "opus", "Opus 160k"),
}

FIELDS = [
    "id", "path", "material", "source", "case",
    "container_rate", "container_bits",
    "true_rate", "true_bits", "lossy_codec", "lossy_span",
    "expected_verdict", "difficulty", "reference", "notes",
]


# --------------------------------------------------------------------------
# Synthesis


def pink_noise(n, rng):
    """Pink (1/f power) noise, unit RMS, by shaping white noise in frequency."""
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.arange(spec.size, dtype=np.float64)
    f[0] = 1.0
    spec /= np.sqrt(f)
    spec[0] = 0.0
    x = np.fft.irfft(spec, n)
    return x / np.sqrt(np.mean(x ** 2))


def highpassed_noise(n, rng):
    """White noise with a first-difference high-pass: energy rising to Nyquist."""
    w = rng.standard_normal(n + 1)
    return np.diff(w) / 2.0


def normalise(x, peak_db=-1.0):
    return x * (10 ** (peak_db / 20) / np.max(np.abs(x)))


def synth_pink(rate, seed):
    rng = np.random.default_rng(seed)
    n = int(DURATION * rate)
    x = np.stack([pink_noise(n, rng), pink_noise(n, rng)], axis=1)
    # Peak at -3 dBFS (about -19 dBFS RMS), leaving headroom for resampler
    # overshoot: clipping would spray distortion above the source's Nyquist.
    return normalise(x, -3.0)


def _notes(seed, count, fmin, fmax):
    """Rate-independent note list, so the same piece is synthesised at every rate."""
    rng = np.random.default_rng(seed)
    starts = np.sort(rng.uniform(0, DURATION - 1.5, count))
    f0s = np.exp(rng.uniform(np.log(fmin), np.log(fmax), count))
    lens = rng.uniform(0.6, 1.5, count)
    pans = rng.uniform(0.2, 0.8, count)
    vibs = rng.uniform(3.0, 6.0, count)
    return list(zip(starts, f0s, lens, pans, vibs))


def synth_music(rate, seed):
    """Harmonic notes up to Nyquist, hi-hats, a pink noise bed, a quiet passage."""
    n = int(DURATION * rate)
    out = np.zeros((n, 2))
    for start, f0, length, pan, vib in _notes(seed, 48, 70.0, 900.0):
        i0 = int(start * rate)
        m = min(int(length * rate), n - i0)
        t = np.arange(m) / rate
        env = np.minimum(t / 0.01, 1.0) * np.exp(-t * 2.5)
        phase = 2 * np.pi * f0 * (t + 0.002 * np.sin(2 * np.pi * vib * t) / vib)
        note = np.zeros(m)
        k = 1
        # Sawtooth-like partials (1/k) up to 40 kHz or Nyquist, capped for speed.
        while k * f0 < min(40000.0, rate / 2 * 0.98) and k <= 48:
            note += np.sin(k * phase) / k
            k += 1
        note *= env * 0.12
        out[i0:i0 + m, 0] += note * (1 - pan)
        out[i0:i0 + m, 1] += note * pan
    rng = np.random.default_rng(seed + 1000)
    hat_len = int(0.08 * rate)
    th = np.arange(hat_len) / rate
    for beat in np.arange(0.0, DURATION - 0.1, 0.25):
        i0 = int(beat * rate)
        hat = highpassed_noise(hat_len, rng) * np.exp(-th / 0.025) * 0.15
        out[i0:i0 + hat_len, 0] += hat
        out[i0:i0 + hat_len, 1] += hat * 0.8
    bed = np.stack([pink_noise(n, rng), pink_noise(n, rng)], axis=1)
    out += bed * 10 ** (-66 / 20)
    # A quiet passage (-30 dB) from 14 to 16 s, with 50 ms ramps.
    t = np.arange(n) / rate
    gain = np.ones(n)
    q0, q1, ramp = 14.0, 16.0, 0.05
    gain[(t >= q0) & (t < q1)] = 10 ** (-30 / 20)
    down = (t >= q0 - ramp) & (t < q0)
    gain[down] = 1 - (1 - 10 ** (-30 / 20)) * (t[down] - (q0 - ramp)) / ramp
    up = (t >= q1) & (t < q1 + ramp)
    gain[up] = 10 ** (-30 / 20) + (1 - 10 ** (-30 / 20)) * (t[up] - q1) / ramp
    return normalise(out * gain[:, None])


def synth_sparse(rate, seed):
    """Piano-like decaying partials with steep roll-off, over a -110 dBFS white floor.

    A genuine 24-bit recording with almost no ultrasonic music: the trap for an
    upsampling rule that only looks for energy above 22.05 kHz.
    """
    n = int(DURATION * rate)
    out = np.zeros((n, 2))
    for start, f0, length, pan, _ in _notes(seed, 30, 55.0, 1800.0):
        i0 = int(start * rate)
        m = min(int(3.0 * rate), n - i0)
        t = np.arange(m) / rate
        note = np.zeros(m)
        for k in range(1, 21):
            fk = k * f0 * np.sqrt(1 + 0.0004 * k * k)  # slight inharmonicity
            if fk >= rate / 2 * 0.98:
                break
            note += np.sin(2 * np.pi * fk * t) / (k * k) * np.exp(-t * (1.2 + 0.6 * k))
        note *= np.minimum(t / 0.003, 1.0) * 0.25
        out[i0:i0 + m, 0] += note * (1 - pan)
        out[i0:i0 + m, 1] += note * pan
    out = normalise(out, -3.0)
    rng = np.random.default_rng(seed + 2000)
    out += rng.standard_normal((n, 2)) * 10 ** (-110 / 20)
    return out


SYNTH = {
    "pink": (synth_pink, 11, "full-band pink noise"),
    "music": (synth_music, 22, "music-like: harmonic notes to 40 kHz, hi-hats, noise bed"),
    "sparse": (synth_sparse, 33, "piano-like, nothing musical above ~20 kHz, -110 dBFS floor"),
}


# --------------------------------------------------------------------------
# Quantisation and file I/O


def quantise(x, bits, rng, dither=True):
    """Float in [-1, 1) to integers at `bits`, with TPDF dither by default."""
    scale = 2 ** (bits - 1)
    y = x * scale
    if dither:
        y = y + rng.uniform(-0.5, 0.5, x.shape) + rng.uniform(-0.5, 0.5, x.shape)
    return np.clip(np.round(y), -scale, scale - 1).astype(np.int64)


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        raise SystemExit("command failed: " + " ".join(cmd))
    return r.stdout


def write_flac(path, ints, rate, bits):
    """Write integer samples (already at `bits`) to FLAC at that bit depth."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if bits == 16:
        raw, fmt, sfmt = ints.astype("<i2").tobytes(), "s16le", "s16"
        extra = []
    else:
        raw = (ints.astype(np.int64) << (32 - bits)).astype("<i4").tobytes()
        fmt, sfmt = "s32le", "s32"
        extra = ["-bits_per_raw_sample", str(bits)]
    cmd = ["ffmpeg", "-v", "error", "-y", "-f", fmt, "-ar", str(rate), "-ac", "2",
           "-i", "-", "-c:a", "flac", "-sample_fmt", sfmt] + extra + [path]
    r = subprocess.run(cmd, input=raw, stderr=subprocess.PIPE)
    if r.returncode:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        raise SystemExit("ffmpeg failed writing " + path)


def read_float(path, rate=None):
    """Decode any file to float64 stereo, optionally resampling with soxr."""
    cmd = ["ffmpeg", "-v", "error", "-i", path]
    if rate:
        cmd += ["-af", "aresample=%d:resampler=soxr:precision=28" % rate]
    cmd += ["-ac", "2", "-f", "f64le", "-"]
    return np.frombuffer(run(cmd), dtype="<f8").reshape(-1, 2).copy()


def probe(path):
    out = run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
               "stream=sample_rate,bits_per_raw_sample,bits_per_sample,sample_fmt",
               "-of", "json", path])
    s = json.loads(out)["streams"][0]
    bits = int(s.get("bits_per_raw_sample") or 0) or int(s.get("bits_per_sample") or 0)
    if not bits:
        bits = {"s16": 16, "s16p": 16, "s32": 32, "s32p": 32}.get(s.get("sample_fmt"), 0)
    return int(s["sample_rate"]), bits


def transcode(src, dst, rate, bits, resampler="soxr"):
    """Decode src and write FLAC at rate/bits through FFmpeg's resampler."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if resampler == "soxr":
        af = "aresample=%d:resampler=soxr:precision=28" % rate
    else:
        af = "aresample=%d" % rate  # swresample defaults
    sfmt = "s16" if bits == 16 else "s32"
    extra = [] if bits == 16 else ["-bits_per_raw_sample", str(bits)]
    run(["ffmpeg", "-v", "error", "-y", "-i", src, "-af", af, "-ac", "2",
         "-c:a", "flac", "-sample_fmt", sfmt] + extra + [dst])


def lossy_roundtrip(src, tmpdir, codec):
    args, ext, _ = LOSSY[codec]
    enc = os.path.join(tmpdir, "%s.%s" % (codec, ext))
    run(["ffmpeg", "-v", "error", "-y", "-i", src] + args + [enc])
    return enc


def align(ref, x):
    """Shift x to line up with ref (codec delay), by cross-correlating a window."""
    m = min(len(ref), len(x), 1 << 17)
    a = ref[:m, 0] - ref[:m, 0].mean()
    b = x[:m, 0] - x[:m, 0].mean()
    nfft = 1 << (2 * m - 1).bit_length()
    c = np.fft.irfft(np.fft.rfft(a, nfft) * np.conj(np.fft.rfft(b, nfft)), nfft)
    lag = int(np.argmax(c))
    if lag > nfft // 2:
        lag -= nfft
    out = np.zeros_like(ref)
    if lag >= 0:
        k = min(len(ref) - lag, len(x))
        out[lag:lag + k] = x[:k]
    else:
        k = min(len(ref), len(x) + lag)
        out[:k] = x[-lag:-lag + k]
    return out


# --------------------------------------------------------------------------
# Corpus construction


class Corpus:
    def __init__(self, out):
        self.out = out
        self.rows = []

    def add(self, path, **label):
        row = {k: "" for k in FIELDS}
        row.update(label)
        row["path"] = os.path.relpath(path, self.out)
        row["id"] = os.path.splitext(row["path"])[0].replace(os.sep, "/")
        rate, bits = probe(path)
        row["container_rate"], row["container_bits"] = rate, bits
        self.rows.append(row)
        print("  %-58s %6d Hz %2d-bit  %s" % (row["path"], rate, bits, row["expected_verdict"]))

    def write_manifest(self):
        self.rows.sort(key=lambda r: r["id"])
        with open(os.path.join(self.out, "manifest.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, FIELDS)
            w.writeheader()
            w.writerows(self.rows)
        with open(os.path.join(self.out, "manifest.json"), "w") as f:
            json.dump({"version": CORPUS_VERSION, "files": self.rows}, f, indent=1)


def derive_fakes(c, src_name, master_hi, hi_rate, cd_master, material, tmp, d):
    """Every fake derived from one source: a hi-res master and its 44.1/16 version.

    master_hi: path of the genuine hi-res FLAC (24-bit at hi_rate), or None.
    cd_master: path of a genuine 44.1 kHz / 16-bit FLAC of the same material.
    d: dict of common label fields.
    """
    base = os.path.join(c.out, material, src_name)
    # Every fake records the genuine file it was made from, for verify.py.
    cd_ref = os.path.relpath(cd_master, c.out)
    hi_ref = os.path.relpath(master_hi, c.out) if master_hi else ""

    # Lossy at CD resolution: a "lossless" file that isn't.
    for codec in LOSSY:
        enc = lossy_roundtrip(cd_master, tmp, codec)
        out = os.path.join(base, "lossy-cd", "%s.flac" % codec)
        transcode(enc, out, 44100, 16)
        c.add(out, reference=cd_ref, source=src_name, case="lossy %s -> FLAC 44.1/16" % codec,
              true_rate=44100, true_bits=16, lossy_codec=LOSSY[codec][2],
              lossy_span="whole", expected_verdict="lossy",
              difficulty="hard" if codec in ("mp3-320", "mp3-v0", "aac-256", "vorbis-q6") else "easy",
              notes="Opus decodes at 48 kHz; resampled to 44.1" if codec.startswith("opus") else "",
              **d)

    # Lossy dressed up as hi-res.
    for codec in ("mp3-320", "aac-256", "mp3-128"):
        enc = lossy_roundtrip(cd_master, tmp, codec)
        out = os.path.join(base, "lossy-hires", "%s-to-96k24.flac" % codec)
        transcode(enc, out, 96000, 24)
        c.add(out, reference=cd_ref, source=src_name, case="lossy %s -> upsampled FLAC 96/24" % codec,
              true_rate=44100, true_bits=16, lossy_codec=LOSSY[codec][2],
              lossy_span="whole", expected_verdict="lossy+upsampled", difficulty="easy", **d)

    # CD master upsampled to hi-res containers, with different resamplers.
    for name, rate, rs in (("44k-to-96k-soxr", 96000, "soxr"),
                           ("44k-to-96k-swr", 96000, "swr"),
                           ("44k-to-192k-soxr", 192000, "soxr")):
        out = os.path.join(base, "upsampled", name + ".flac")
        transcode(cd_master, out, rate, 24, rs)
        c.add(out, reference=cd_ref, source=src_name, case="CD 44.1/16 upsampled to %d/24 (%s)" % (rate, rs),
              true_rate=44100, true_bits=16, expected_verdict="upsampled",
              difficulty="medium" if rs == "swr" else "easy",
              notes="swresample defaults leave a softer, less obvious band edge" if rs == "swr" else "",
              **d)

    if not master_hi:
        return

    # 48 kHz source upsampled to 96 kHz.
    tmp48 = os.path.join(tmp, "48k.flac")
    transcode(master_hi, tmp48, 48000, 24)
    out = os.path.join(base, "upsampled", "48k-to-96k-soxr.flac")
    transcode(tmp48, out, 96000, 24)
    c.add(out, reference=hi_ref, source=src_name, case="48/24 upsampled to 96/24 (soxr)",
          true_rate=48000, true_bits=24, expected_verdict="upsampled", difficulty="easy", **d)

    # Bit-depth fakes at the hi-res rate: genuine bandwidth, 16-bit resolution.
    hi = read_float(master_hi)
    rng = np.random.default_rng(4242)
    x16 = quantise(hi, 16, rng)
    padded = x16 << 8
    out = os.path.join(base, "bitdepth", "16-in-24-zero-padded.flac")
    write_flac(out, padded, hi_rate, 24)
    c.add(out, reference=hi_ref, source=src_name, case="16-bit padded to 24 with zero low bits",
          true_rate=hi_rate, true_bits=16, expected_verdict="padded-bits", difficulty="easy",
          notes="SoniqTools-style empty-low-bits test catches this", **d)

    # 16-bit signal plus a little noise in the 24-bit domain (4 LSB RMS, about
    # -126 dBFS): what a 24-bit processing chain with dither adds. The low byte is
    # busy, but every sample stays within a few LSB of the 16-bit grid.
    noise = rng.standard_normal(x16.shape) * 4.0
    dith = np.clip(np.round((x16 << 8) + noise), -2 ** 23, 2 ** 23 - 1).astype(np.int64)
    out = os.path.join(base, "bitdepth", "16-in-24-dithered.flac")
    write_flac(out, dith, hi_rate, 24)
    c.add(out, reference=hi_ref, source=src_name, case="16-bit converted to 24 with low-level 24-bit noise/dither",
          true_rate=hi_rate, true_bits=16, expected_verdict="padded-bits", difficulty="hard",
          notes="low bits are busy, so an empty-low-bits test passes it; "
                "only the LSB histogram gives it away", **d)

    gained = quantise(x16 / 32768.0 * 10 ** (-0.5 / 20), 24, rng, dither=False)
    out = os.path.join(base, "bitdepth", "16-in-24-gain-0.5dB.flac")
    write_flac(out, gained, hi_rate, 24)
    c.add(out, reference=hi_ref, source=src_name, case="16-bit, -0.5 dB gain change, requantised to 24 undithered",
          true_rate=hi_rate, true_bits=16, expected_verdict="padded-bits", difficulty="hard",
          notes="gain change fills the low bits deterministically (only 65536 distinct levels)", **d)

    # Splice: genuine hi-res with a transcoded section in the middle.
    enc = lossy_roundtrip(cd_master, tmp, "mp3-320")
    fake = align(hi, read_float(enc, hi_rate))
    n0, n1 = int(SPLICE[0] * hi_rate), int(SPLICE[1] * hi_rate)
    xf = int(0.01 * hi_rate)
    w = np.ones(len(hi))
    w[n0:n1] = 0.0
    w[n0 - xf:n0] = np.linspace(1, 0, xf)
    w[n1:n1 + xf] = np.linspace(0, 1, xf)
    spliced = hi * w[:, None] + fake * (1 - w[:, None])
    out = os.path.join(base, "splice", "mp3-320-section-8s-12s.flac")
    write_flac(out, quantise(spliced, 24, rng), hi_rate, 24)
    c.add(out, reference=hi_ref, source=src_name,
          case="genuine %d/24 with 8-12 s replaced by MP3 320 (upsampled)" % hi_rate,
          true_rate=hi_rate, true_bits=24, lossy_codec=LOSSY["mp3-320"][2],
          lossy_span="%g-%gs" % SPLICE, expected_verdict="mixed", difficulty="hard",
          notes="a whole-track average dilutes the 4 s transcoded section", **d)


def build_synthetic(c, tmp):
    for name, (fn, seed, desc) in SYNTH.items():
        print("%s: %s" % (name, desc))
        d = {"material": "synthetic"}
        hard = name == "sparse"
        rng = np.random.default_rng(seed + 7)
        # Genuine masters, synthesised natively at each rate.
        hi_path = None
        for rate in (96000, 192000) if name != "pink" else (96000,):
            path = os.path.join(c.out, "synthetic", name, "genuine", "%dk24.flac" % (rate // 1000))
            write_flac(path, quantise(fn(rate, seed), 24, rng), rate, 24)
            c.add(path, source=name, case="genuine %d/24" % rate, true_rate=rate, true_bits=24,
                  expected_verdict="genuine", difficulty="hard" if hard else "easy",
                  notes="little ultrasonic content; must not read as upsampled" if hard else "",
                  **d)
            if rate == 96000:
                hi_path = path
        cd_path = os.path.join(c.out, "synthetic", name, "genuine", "44k16.flac")
        write_flac(cd_path, quantise(fn(44100, seed), 16, rng), 44100, 16)
        c.add(cd_path, source=name, case="genuine CD 44.1/16", true_rate=44100, true_bits=16,
              expected_verdict="genuine", difficulty="easy",
              notes="genuine CD resolution: full band to 22.05 kHz, must not read as lossy", **d)
        derive_fakes(c, name, hi_path, 96000, cd_path, "synthetic", tmp, d)

    # Mixed album: five genuine 96/24 tracks, track 3 secretly from MP3 320.
    print("album: five tracks, track 3 transcoded")
    d = {"material": "synthetic"}
    album = os.path.join(c.out, "synthetic", "album-mixed")
    for track in range(1, 6):
        seed = 100 + track
        rng = np.random.default_rng(seed)
        path = os.path.join(album, "%02d.flac" % track)
        if track != 3:
            write_flac(path, quantise(synth_music(96000, seed), 24, rng), 96000, 24)
            c.add(path, source="album", case="album track %d, genuine 96/24" % track,
                  true_rate=96000, true_bits=24, expected_verdict="genuine",
                  difficulty="easy", **d)
        else:
            cd = os.path.join(tmp, "album-cd.flac")
            write_flac(cd, quantise(synth_music(44100, seed), 16, rng), 44100, 16)
            transcode(lossy_roundtrip(cd, tmp, "mp3-320"), path, 96000, 24)
            c.add(path, source="album", case="album track 3, MP3 320 upsampled to 96/24",
                  true_rate=44100, true_bits=16, lossy_codec=LOSSY["mp3-320"][2],
                  lossy_span="whole", expected_verdict="lossy+upsampled", difficulty="easy",
                  notes="the one bad track in an otherwise genuine album", **d)


def build_real(c, files, tmp):
    for src in files:
        name = os.path.splitext(os.path.basename(src))[0]
        rate, bits = probe(src)
        print("%s: %d Hz, %d-bit (real, never committed)" % (name, rate, bits))
        d = {"material": "real"}
        base = os.path.join(c.out, "real", name)
        if rate > 48000 and bits >= 24:
            hi = os.path.join(base, "genuine", "master.flac")
            transcode(src, hi, rate, 24)
            c.add(hi, source=name, case="your genuine master", true_rate=rate, true_bits=24,
                  expected_verdict="genuine", difficulty="real",
                  notes="label assumes the master is genuine hi-res", **d)
            cd = os.path.join(base, "genuine", "44k16.flac")
            x = read_float(src, 44100)
            write_flac(cd, quantise(x, 16, np.random.default_rng(1)), 44100, 16)
            c.add(cd, source=name, case="your master, downconverted to CD 44.1/16",
                  true_rate=44100, true_bits=16, expected_verdict="genuine",
                  difficulty="real", **d)
            derive_fakes(c, name, hi, rate, cd, "real", tmp, d)
        elif rate == 44100 and bits == 16:
            cd = os.path.join(base, "genuine", "44k16.flac")
            transcode(src, cd, 44100, 16)
            c.add(cd, source=name, case="your genuine CD rip", true_rate=44100, true_bits=16,
                  expected_verdict="genuine", difficulty="real",
                  notes="label assumes a lossless rip of a CD", **d)
            derive_fakes(c, name, None, 0, cd, "real", tmp, d)
        else:
            print("  skipped: need a hi-res (>48 kHz, 24-bit) master or a 44.1/16 CD rip")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "out"),
                    help="output directory (default: tests/corpus/out)")
    ap.add_argument("--from", dest="real", action="append", default=[], metavar="FILE",
                    help="also derive fakes from this genuine file (repeatable)")
    ap.add_argument("--no-synthetic", action="store_true", help="only process --from files")
    args = ap.parse_args()

    for tool in ("ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            raise SystemExit("%s not found on PATH" % tool)
    os.makedirs(args.out, exist_ok=True)
    c = Corpus(args.out)
    with tempfile.TemporaryDirectory() as tmp:
        if not args.no_synthetic:
            build_synthetic(c, tmp)
        build_real(c, args.real, tmp)
    c.write_manifest()
    print("%d files, manifest in %s" % (len(c.rows), os.path.join(args.out, "manifest.csv")))


if __name__ == "__main__":
    main()
