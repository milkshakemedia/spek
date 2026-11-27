#pragma once

// Analysis engine for spotting fake hi-res and transcoded audio.
//
// Everything here is independent of the GUI so the window, a batch command
// line tool and the tests can share it. Results are evidence, not proof: every
// field a verdict rests on is reported so a person can check it.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class AudioFile;
class FFTPlan;

// ---------------------------------------------------------------------------
// Frequency cutoff, frame by frame.
//
// Lossy encoders low-pass the signal (MP3 128k at about 16 kHz, 320k at about
// 20 kHz), and upsampled files carry nothing above the source's Nyquist
// frequency. Both leave a "shelf": a frequency above which the spectrum is far
// below the level just under it, all the way up. The shelf is looked for in
// every frame on its own and only then aggregated, so a single transcoded
// section isn't averaged away by the rest of the file.

struct CutoffFrame
{
    double time;       // start of the frame, in seconds
    bool active;       // loud enough to judge
    double cutoff_hz;  // highest shelf edge found, or 0 if the frame is full band
    double drop_db;    // level just below the edge minus the loudest band above it
};

struct CutoffSegment
{
    double start;      // seconds
    double end;        // seconds
    double cutoff_hz;  // median shelf edge over the segment
};

enum class CutoffVerdict
{
    UNKNOWN,           // too little active audio to judge
    FULL_BAND,         // no shelf below the file's own Nyquist frequency
    LOSSY,             // a shelf below 21 kHz, typical of a lossy encoder
    UPSAMPLED,         // a hi-res file band-limited to a lower rate's Nyquist
    LOSSY_UPSAMPLED,   // a lossy shelf inside a hi-res container
    MIXED,             // part of the file has a lossy shelf, part doesn't
};

const char *cutoff_verdict_name(CutoffVerdict verdict);

struct CutoffResult
{
    int sample_rate = 0;
    int frames = 0;           // frames analysed
    int active_frames = 0;    // frames loud enough to judge
    int shelf_frames = 0;     // active frames with a shelf edge
    double cutoff_hz = 0;     // median edge over the frames behind the verdict (0: none)
    double consistency = 0;   // share of active frames within 750 Hz of cutoff_hz
    double drop_db = 0;       // median drop over shelf frames
    int source_rate = 0;      // for UPSAMPLED: the likely original sample rate
    std::string codec_hint;   // for LOSSY*: encoders known to cut around cutoff_hz
    std::vector<CutoffSegment> lossy_segments;  // runs of lossy shelf of 2 s or more
    CutoffVerdict verdict = CutoffVerdict::UNKNOWN;
    double confidence = 0;    // 0..1: share of active frames that agree with the verdict
    std::string basis = "frames";  // "frames"; "average" if only the whole-file power
                                   // average showed the shelf; "tracking" if only the
                                   // music-following test did (both weaker evidence)
    std::vector<CutoffFrame> frame_log;
};

class CutoffAnalyser
{
public:
    explicit CutoffAnalyser(int sample_rate);
    ~CutoffAnalyser();

    // Feed mono samples, in any block size.
    void feed(const float *samples, int count);
    // Analyse what's been fed and return the result.
    CutoffResult finish();

    // Analysis frame size in samples (a power of two that scales with the rate).
    int frame_size() const { return this->nfft; }

private:
    void analyse_frame();

    int sample_rate;
    int nbits;
    int nfft;
    std::vector<float> window;
    std::vector<float> pending;
    std::unique_ptr<FFTPlan> plan;
    std::vector<CutoffFrame> frames;
    long long consumed;
    std::vector<double> power_sum;  // summed linear power per bin over active frames
    int power_frames;
    // Per active frame: the 4-12 kHz level, then 1 kHz bands from 16 kHz up,
    // for the check that ultrasonic content follows the music.
    std::vector<std::vector<double>> track;
};

// Decode the first channel of an open audio file and analyse it.
CutoffResult analyse_cutoff(AudioFile& file);

// ---------------------------------------------------------------------------
// True bit depth.
//
// A 16-bit master in a 24-bit file shows one of two traces. Padded with zeros,
// the low bits are never set. Gain-changed or dithered on the way, the low
// bits are busy but the sample values still crowd around a lattice with the
// 16-bit step (a little off it after a gain change), which shows as a strong
// periodic "comb" in the histogram of sample values near zero. A genuine
// 24-bit recording's histogram is smooth at that scale.

enum class BitDepthVerdict
{
    UNKNOWN,           // not integer PCM, too quiet, or too short to judge
    NATIVE,            // uses the container's full depth
    PADDED,            // low bits always zero
    REQUANTISED,       // low bits busy, but values sit on a coarser lattice
};

const char *bitdepth_verdict_name(BitDepthVerdict verdict);

struct BitDepthResult
{
    int container_bits = 0;   // bits per sample the file stores
    int effective_bits = 0;   // for PADDED/REQUANTISED: the likely source depth
    int zero_low_bits = 0;    // low bits that are zero in every sample
    double comb_period = 0;   // lattice step found, in container codes (0: none)
    double comb_db = 0;       // how far the comb peak stands above its neighbours
    long long samples = 0;    // samples analysed
    BitDepthVerdict verdict = BitDepthVerdict::UNKNOWN;
    double confidence = 0;    // 0..1, for PADDED/REQUANTISED only
};

class BitDepthAnalyser
{
public:
    // container_bits: 16 or 24. Other depths are reported as UNKNOWN.
    explicit BitDepthAnalyser(int container_bits);

    // Feed integer samples at the container's depth, in any block size.
    void feed(const int32_t *samples, int count);
    BitDepthResult finish();

private:
    int container_bits;
    uint32_t used_bits;           // OR of the magnitudes of every sample
    long long samples;
    int range;                    // histogram covers -range .. range-1
    std::vector<uint32_t> hist;   // counts of sample values near zero
};

// Decode the first channel of an open audio file and analyse its bit depth.
BitDepthResult analyse_bitdepth(AudioFile& file);

// ---------------------------------------------------------------------------
// Everything about one file, from a single decode.

struct FileAnalysis
{
    std::string codec;        // decoder's long name, e.g. "FLAC (Free Lossless Audio Codec)"
    bool lossless = false;    // a lossless or PCM codec
    int sample_rate = 0;
    int bits = 0;             // bits per sample, 0 for lossy codecs
    int channels = 0;
    double duration = 0;      // seconds
    CutoffResult cutoff;
    BitDepthResult bitdepth;

    // Overall reading. verdict is "clean", "unknown", "lossy-format" (a lossy
    // file, which is no fake), or a "+"-joined list of what's wrong: "lossy",
    // "upsampled", "partly-lossy", "fake-bits". Evidence for a person.
    std::string verdict;
    double confidence = 0;
    std::vector<std::string> evidence;
};

bool codec_is_lossless(const std::string& codec_name);

// Decode the first channel once and run every detector on it.
FileAnalysis analyse_file(AudioFile& file);
