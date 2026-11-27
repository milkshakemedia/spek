#!/usr/bin/env python3
"""Check that every file in a generated corpus really has the property its label claims.

This is not the detector. It is a guard on the corpus itself: before any rule is
judged against these labels, each label has to be shown to be true. The checks
may use knowledge a detector doesn't have, chiefly the genuine source each fake
was made from, so they are simple and hard to fool:

* lossy: after aligning it with its genuine source, the file differs from it by
  far more than a lossless copy could (SNR under 70 dB). The measured bandwidth
  is reported too; some encoders keep the full band, which is exactly why a
  cutoff rule alone is not enough.
* upsampled / lossy+upsampled: the spectrum falls at least 25 dB from just
  below the true source's Nyquist frequency to 4-8 kHz above it. Where noise
  was added up top to hide that fall, the file minus a plain upsample of the
  same source must instead be noise at the labelled level, at least 60 dB
  under the music below 22.05 kHz.
* genuine hi-res: no such fall at 22.05 or 24 kHz, real content above 26 kHz
  (20 dB over the 24-bit noise floor), and more than 65536 distinct values.
* padded-bits: samples sit exactly on a 16-bit grid (zero-padded), within a
  few LSB of it (dithered), or take at most 65536 distinct values (gain change).
* mixed: second by second, only the labelled span loses its top end.

    tests/corpus/verify.py tests/corpus/out [--report report.csv]
"""

import argparse
import csv
import json
import os
import subprocess
import sys

import numpy as np

NFFT = 8192
LOSSLESS_SNR_DB = 70.0


def probe_rate(path):
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
                        "stream=sample_rate", "-of", "csv=p=0", path],
                       stdout=subprocess.PIPE, check=True)
    return int(r.stdout.strip())


def decode(path, as_int=False, rate=None):
    fmt, dt = ("s32le", "<i4") if as_int else ("f64le", "<f8")
    cmd = ["ffmpeg", "-v", "error", "-i", path]
    if rate:
        cmd += ["-af", "aresample=%d:resampler=soxr:precision=28" % rate]
    cmd += ["-ac", "2", "-f", fmt, "-"]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return np.frombuffer(r.stdout, dtype=dt).reshape(-1, 2)


def psd_db(x, rate):
    """Welch average power spectrum of the mid channel, in dB, plus bin frequencies."""
    mono = x.mean(axis=1)
    win = np.hanning(NFFT)
    hop = NFFT // 2
    frames = (len(mono) - NFFT) // hop + 1
    acc = np.zeros(NFFT // 2 + 1)
    for i in range(frames):
        acc += np.abs(np.fft.rfft(mono[i * hop:i * hop + NFFT] * win)) ** 2
    acc /= max(frames, 1)
    return np.arange(acc.size) * rate / NFFT, 10 * np.log10(acc + 1e-30)


# Per-bin level of 24-bit TPDF dither in psd_db's scaling (mid channel of two
# independent channels), the floor of a genuine 24-bit file with nothing in it.
FLOOR_24 = 10 * np.log10((1 / 6) * 2.0 ** -46 / 2 * np.sum(np.hanning(NFFT) ** 2))


def band(freqs, db, lo, hi):
    sel = (freqs >= lo) & (freqs < hi)
    return float(np.median(db[sel]))


def fall_db(freqs, db, edge):
    """Level just below `edge` minus level 4-8 kHz above it (positive = a fall).

    The gap skips a resampler's transition band; FFmpeg's default swresample
    filter, for one, leaks well past the Nyquist frequency.
    """
    return band(freqs, db, edge - 3000, edge - 1000) - band(freqs, db, edge + 4000, edge + 8000)


def smooth(y, n):
    return np.convolve(y, np.ones(n) / n, mode="same")


def bandwidth_vs(freqs, db, ref_db, nyquist):
    """First frequency above 5 kHz where the file falls 15 dB below its source."""
    diff = smooth(db - ref_db, max(3, int(500 / (freqs[1] - freqs[0]))))
    sel = np.where((freqs > 5000) & (freqs < nyquist * 0.99) & (diff < -15))[0]
    return float(freqs[sel[0]]) if sel.size else float(nyquist)


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


def snr_vs(path, ref):
    """SNR of a file against its genuine 44.1 kHz source, in dB, after alignment."""
    r = decode(ref)
    x = align(r, decode(path, rate=44100))
    edge = 44100  # skip the first and last second (codec warm-up, padding)
    r, x = r[edge:-edge], x[edge:-edge]
    return 10 * np.log10(np.sum(r ** 2) / max(np.sum((r - x) ** 2), 1e-30))


def check(row, root, cache):
    path = os.path.join(root, row["path"])
    verdict = row["expected_verdict"]
    rate = int(row["container_rate"])
    true_rate = int(row["true_rate"])
    ref = os.path.join(root, row["reference"]) if row.get("reference") else None

    def spectrum(p):
        if p not in cache:
            cache[p] = psd_db(decode(p), probe_rate(p))
        return cache[p]

    freqs, db = spectrum(path)

    if verdict == "lossy":
        snr = snr_vs(path, ref)
        _, rdb = spectrum(ref)
        bw = bandwidth_vs(freqs, db, rdb, rate / 2)
        note = "" if bw < 21000 else " (no low-pass: invisible to a cutoff rule)"
        return snr < LOSSLESS_SNR_DB, "SNR %.0f dB vs source, bandwidth %.1f kHz%s" % (
            snr, bw / 1000, note)

    if verdict == "upsampled" and "noise at" in row.get("notes", ""):
        # Noise added above the old Nyquist hides the fall, by design. Instead
        # compare with the plain soxr upsample of the same CD source (same
        # pipeline, same rate): the difference must be only noise at the stated
        # level, with next to nothing below 22.05 kHz, so nothing up top came
        # from the music.
        plain = os.path.join(os.path.dirname(path), "44k-to-96k-soxr.flac")
        x, y = decode(path), decode(plain)
        k = min(len(x), len(y))
        diff = (x[:k] - y[:k])[:, 0]
        level = 10 * np.log10(np.mean(diff ** 2))
        want = float(row["notes"].split("noise at ")[1].split(" dBFS")[0])
        fd, dd = psd_db(diff[:, None].repeat(2, axis=1), rate)
        fs, sd = psd_db(y[:k], rate)
        below = band(fd, dd, 1000, true_rate / 2 * 0.9) - band(fs, sd, 1000, true_rate / 2 * 0.9)
        ok = abs(level - want) < 3 and below < -60
        return ok, "added noise %.0f dBFS RMS (labelled %.0f), %.0f dB under the music below 22.05 kHz" % (
            level, want, below)

    if verdict in ("upsampled", "lossy+upsampled"):
        f = fall_db(freqs, db, true_rate / 2)
        ok = f >= 25
        msg = "%.0f dB fall above %.2f kHz" % (f, true_rate / 2000)
        if verdict == "lossy+upsampled" and ref:
            snr = snr_vs(path, ref)
            ok = ok and snr < LOSSLESS_SNR_DB
            msg += ", SNR %.0f dB vs CD source" % snr
        return ok, msg

    if verdict == "genuine":
        msgs, ok = [], True
        if rate > 48000:
            for edge in (22050, 24000):
                f = fall_db(freqs, db, edge)
                msgs.append("%+.0f dB above %.2f kHz" % (f, edge / 1000))
                ok = ok and f < 25
            hf = band(freqs, db, 26000, min(40000, rate / 2 * 0.9)) - FLOOR_24
            msgs.append("26-40 kHz %+.0f dB over 24-bit floor" % hf)
            ok = ok and hf > 20
        else:
            f = band(freqs, db, 15000, 19000) - band(freqs, db, 20500, 21800)
            msgs.append("%+.0f dB from 15-19 kHz to 20.5-21.8 kHz" % f)
            ok = ok and f < 15
        if int(row["true_bits"]) == 24:
            v = decode(path, as_int=True)[:, 0] >> 8
            u = np.unique(v).size
            msgs.append("%d distinct values" % u)
            ok = ok and u > 65536
        return ok, ", ".join(msgs)

    if verdict == "padded-bits":
        v = decode(path, as_int=True)[:, 0].astype(np.int64) >> 8  # 24-bit values
        low = v & 0xFF
        busy = float(np.mean(low != 0))
        name = os.path.basename(path)
        if "zero-padded" in name:
            return busy == 0, "%.1f%% of samples have non-zero low bits" % (100 * busy)
        if "dithered" in name:
            near = float(np.mean(np.minimum(low, 256 - low) <= 32))
            return busy > 0.5 and near > 0.999, \
                "%.1f%% have non-zero low bits, %.2f%% within 1/8 LSB of the 16-bit grid " \
                "(uniform 24-bit: 25%%)" % (100 * busy, 100 * near)
        u = np.unique(v).size
        return u <= 65536 and busy > 0.5, \
            "%d distinct values, %.1f%% with non-zero low bits" % (u, 100 * busy)

    if verdict == "mixed":
        x, r = decode(path), decode(ref)
        lo, hi = [float(s.rstrip("s")) for s in row["lossy_span"].split("-")]
        secs, bad = [], []
        for s in range(int(len(x) / rate)):
            f, d = psd_db(x[s * rate:(s + 1) * rate], rate)
            _, rd = psd_db(r[s * rate:(s + 1) * rate], rate)
            bw = bandwidth_vs(f, d, rd, rate / 2)
            secs.append(bw)
            if (lo <= s and s + 1 <= hi) != (bw < 21000):
                bad.append(s)
        return not bad, "per-second bandwidth (kHz): %s%s" % (
            " ".join("%.0f" % (b / 1000) for b in secs),
            ("; unexpected at s=%s" % bad) if bad else "")

    return False, "unknown verdict %r" % verdict


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("corpus", help="directory containing manifest.json")
    ap.add_argument("--report", help="write per-file results to this CSV")
    args = ap.parse_args()

    with open(os.path.join(args.corpus, "manifest.json")) as f:
        rows = json.load(f)["files"]
    cache, results, failed = {}, [], 0
    for row in rows:
        ok, msg = check(row, args.corpus, cache)
        failed += not ok
        results.append({"id": row["id"], "expected_verdict": row["expected_verdict"],
                        "difficulty": row["difficulty"], "ok": ok, "evidence": msg})
        print("%s  %-50s %-16s %s" % ("PASS" if ok else "FAIL", row["id"],
                                      row["expected_verdict"], msg))
    if args.report:
        with open(args.report, "w", newline="") as f:
            w = csv.DictWriter(f, ["id", "expected_verdict", "difficulty", "ok", "evidence"])
            w.writeheader()
            w.writerows(results)
    print("%d of %d labels verified" % (len(rows) - failed, len(rows)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
