# Test corpus for the fake hi-res detector

Every detection rule in Spek's analysis engine is judged against this corpus
before it is trusted. The repository holds the **generator and the checks, not
the audio**: the files are rebuilt on demand (about a minute, ~330 MB), and your
own real recordings never leave your machine.

    tests/corpus/generate.py                  # writes tests/corpus/out (git-ignored)
    tests/corpus/verify.py tests/corpus/out   # proves each file matches its label

Needs Python 3.8+ with numpy, and an ffmpeg built with libmp3lame, libvorbis,
libopus and libsoxr (Homebrew's and Ubuntu's are). On a Mac:
`brew install ffmpeg numpy`.

## Ground truth

`out/manifest.csv` (and `manifest.json`) labels every file:

| Field | Meaning |
|---|---|
| `container_rate`, `container_bits` | what the file claims to be |
| `true_rate`, `true_bits` | what it really is |
| `lossy_codec`, `lossy_span` | the lossy step in its history, and over which part of the file |
| `expected_verdict` | `genuine`, `lossy`, `upsampled`, `lossy+upsampled`, `padded-bits` or `mixed` |
| `difficulty` | `easy`, `medium` or `hard`, from what we know about the case |
| `reference` | the genuine file a fake was made from (used by verify.py) |

## What's in it (70 files)

Three synthetic sources, each synthesised natively at every sample rate so that
"genuine" really is genuine:

- **pink**: full-band pink noise, the easy case.
- **music**: harmonic notes with partials up to 40 kHz, hi-hats, a −66 dB noise
  bed and a quiet passage.
- **sparse**: piano-like decaying notes with nothing musical above about
  20 kHz, over a −110 dBFS noise floor. A genuine hi-res file that a naive
  "energy above 22 kHz" test would call upsampled.

From each source:

| Folder | Files | What it tests |
|---|---|---|
| `genuine/` | 96/24, 192/24 (not pink), 44.1/16 | must **not** be flagged; a genuine CD file must not read as lossy |
| `lossy-cd/` | MP3 128/192/320/V0, AAC 128/256, Vorbis q6, Opus 160, each decoded to 44.1/16 FLAC | lossy passed off as lossless |
| `lossy-hires/` | MP3 320, AAC 256, MP3 128, upsampled to 96/24 | lossy passed off as hi-res |
| `upsampled/` | 44.1→96 (soxr and FFmpeg's default swresample), 44.1→192, 48→96 | CD or 48 kHz audio in a hi-res container |
| `bitdepth/` | 16-bit at 96 kHz stored as 24-bit: zero-padded, with low-level 24-bit noise, and after a −0.5 dB gain change | fake 24-bit; the last two defeat an empty-low-bits test |
| `splice/` | genuine 96/24 with 8–12 s replaced by MP3 320 | frame-by-frame detection; a whole-track average dilutes it |

Plus `album-mixed/`: five genuine 96/24 tracks where track 3 is secretly MP3 320.

## Your own recordings

Synthetic signals only go so far. To derive the same fakes from real music:

    tests/corpus/generate.py --out ~/spek-corpus \
        --from "~/Music/some verified 96k master.flac" \
        --from "~/Music/a CD rip.flac"

A hi-res source (over 48 kHz, 24-bit) gets the full set of fakes; a 44.1/16 CD
rip gets the lossy and upsampled ones. The labels assume your source really is
what it says, so use files whose provenance you trust.

## What verify.py checks

It is not the detector: it may use what a detector can't know, chiefly the
genuine source each fake came from. Lossy files must differ from their source by
far more than a lossless copy could (SNR under 70 dB); upsampled files must fall
at least 25 dB above the true source's Nyquist; genuine hi-res must have real
content above 26 kHz and more than 65536 distinct sample values; fake 24-bit
files must sit on a 16-bit grid; the splice must lose its top end only in the
labelled seconds.

Things it found while the corpus was built, worth knowing for the rules:

- **Some lossy encodes keep the full band.** FFmpeg's AAC at 256k, LAME V0 and
  Vorbis q6 show no low-pass at all at 44.1 kHz, so a cutoff rule alone can't
  catch them.
- **FFmpeg's default resampler leaks past Nyquist.** With swresample's defaults,
  strong energy carries on to about 24 kHz and some well beyond, so the band
  edge is much softer than with soxr.
- **The sparse genuine file falls about 20 dB across 22 kHz** on its own, just
  from the natural roll-off of the music. An upsampled file falls 60 dB or more.
  Any upsampling threshold has to sit between those.

## Scoring the detectors

`score.py` runs the development driver (`tests/spek-analyze`, built with
`make -C tests spek-analyze`) on every file and compares its verdict with the
label:

    tests/corpus/score.py tests/corpus/out --report scores.csv

Each file comes out correct, partial (a fake flagged as the wrong kind of fake),
missed, false (a genuine file flagged) or wrong. CI fails on any false positive,
or if fewer files are correct than the last accepted count.

### Frequency cutoff (Phase 2b)

57 of 70 correct, 4 partial, 9 missed, 0 false positives:

| Label | Result |
|---|---|
| genuine (12) | all full-band |
| padded bits (9) | all full-band (that's the bit-depth detector's job) |
| lossy (24) | 14 correct, 1 partial, 9 missed |
| upsampled (12) | all correct, source rate right |
| lossy+upsampled (10) | 7 correct, 3 reported as upsampled |
| mixed (3) | all correct, segments within 0.2 s |

What it can't do yet:

- **Lossy encodes with no visible low-pass**: all 9 misses. AAC 256k, LAME V0
  and Vorbis q6 keep the full band; on the sparse source MP3 320k's 20 kHz
  low-pass sits where the music already ends. They need a detector that looks
  for the encoder's holes and smearing rather than a shelf.
- **AAC 256k upsampled to 96k** cuts at 21.2 kHz, above the 21 kHz lossy line,
  so it's reported as upsampled from 44.1k. That's true, just not the whole story.
- **Quiet, sparse 16-bit lossy files** have no frame where the shelf rises above
  the dither floor. These are caught only by the whole-file power average
  (reported with `basis: average` and confidence 0.5), which needs a 12 dB brick
  wall; false positives start at 6 dB.
- **Quiet upsampled files** show the band edge in only some frames (36% for the
  sparse swresample file). A file is still called upsampled when at least a
  quarter of its frames share one band-limit edge within 750 Hz; genuine files
  put at most 3 of 234 frames on a shared edge.
