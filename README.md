# Spek

Spek is an acoustic spectrum analyser written in C++. It uses FFmpeg
libraries for audio decoding and wxWidgets for the GUI.

Spek is available on *BSD, GNU/Linux, Windows and Mac OS X.

Find out more about Spek on its website: <https://www.spek.cc/>

## This fork: spotting fake hi-res

This fork (milkshakemedia/spek) adds an analysis engine that looks for audio
that isn't what it claims to be: lossy transcodes in lossless files, upsampled
"hi-res", 16-bit masters in 24-bit files, and albums with one transcoded
track. Verdicts are hints with evidence, never proof.

The batch tool, `spek-cli`, scans files and folders and writes a spectrogram
PNG per file plus a CSV and JSON report:

    spek-cli -o report ~/Music/Downloads

On the Mac it ships inside the app: `Spek.app/Contents/MacOS/spek-cli`.

Each file gets a `verdict`:

| Verdict | Meaning |
|---|---|
| `clean` | full band, and (for 24-bit) all 24 bits in use |
| `lossy` | the spectrum ends in a lossy encoder's low-pass shelf |
| `upsampled` | nothing from the music above a lower rate's Nyquist |
| `lossy+upsampled` | both |
| `partly-lossy` | a lossy shelf in some sections only (timed in the report) |
| `fake-bits` | a 16- or 20-bit master in a 24-bit file |
| `lossy-format` | an MP3, AAC etc. file: lossy, but honestly so |
| `unknown` | too little audio to judge |

Flags combine (`upsampled+fake-bits`). Each row also carries a confidence
(0 to 1), the cutoff, the likely source rate and depth, a codec hint and
plain-language evidence. The spectrogram marks the cutoff the verdict rests
on with a dashed line (cyan; magenta over just the lossy sections of a
partly lossy file).

How the detectors work and how they score on the test corpus:
[tests/corpus/README.md](tests/corpus/README.md).

## Spek 0.8.5 - Released 2023-01-09

### New Features And Enhancements

New features since 0.8.2:

 * Upgrade to FFmpeg 5.1
 * Add 2 more palettes and change the default.
 * Allow changing the DFT window size and function.
 * Allow switching between audio streams and channels.
 * Add translations for 14 additional languages.

Enhancements:

 * Remove dependency on `intltool`.
 * Fix FFmpeg build warnings.
 * Detect AR tool.
 * Use Homebrew for macOS dependencies.
 * Improve test coverage.
 * Use `XDG_CONFIG_HOME` on Unix systems.

Bugfixes:

 * Remove association with .mod and MIDI files.
 * Fix autoconf errors.
 * Fix an AVX-related crash.

### Sources / Packages

Spek 0.8.5 tarball:

 * <https://github.com/alexkay/spek/releases/download/v0.8.5/spek-0.8.5.tar.xz>

Windows and Mac OS X binaries:

 * <https://github.com/alexkay/spek/releases/download/v0.8.2/spek-0.8.2.msi>
 * <https://github.com/alexkay/spek/releases/download/v0.8.2/spek-0.8.2.zip>
 * <https://github.com/alexkay/spek/releases/download/v0.8.2/spek-0.8.2.dmg>

Unix packages:

 * <https://github.com/alexkay/spek/blob/master/INSTALL.md#bsd-and-gnulinux>

### Dependencies

 * wxWidgets >= 3.1.7
 * FFmpeg >= 6.0 (libavformat, libavcodec, libavutil)
