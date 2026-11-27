#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "spek-analysis.h"

#include "test.h"

// Deterministic white noise in [-amp, amp] (xorshift, same on every platform).
static std::vector<float> noise(int count, float amp, uint32_t seed = 2463534242u)
{
    std::vector<float> out(count);
    uint32_t x = seed;
    for (int i = 0; i < count; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = amp * (2.0f * (x / 4294967296.0f) - 1.0f);
    }
    return out;
}

// Blackman-windowed sinc low-pass FIR, applied by direct convolution.
static std::vector<float> lowpass(const std::vector<float>& in, int rate, double cutoff_hz, int taps = 255)
{
    std::vector<double> h(taps);
    double fc = cutoff_hz / rate;
    int m = taps - 1;
    double sum = 0;
    for (int i = 0; i < taps; ++i) {
        double n = i - m / 2.0;
        double sinc = n == 0 ? 2 * fc : sin(2 * M_PI * fc * n) / (M_PI * n);
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / m) + 0.08 * cos(4 * M_PI * i / m);
        h[i] = sinc * w;
        sum += h[i];
    }
    std::vector<float> out(in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); ++i) {
        double acc = 0;
        for (int k = 0; k < taps && k <= (int)i; ++k) {
            acc += h[k] / sum * in[i - k];
        }
        out[i] = (float)acc;
    }
    return out;
}

static CutoffResult analyse(const std::vector<float>& samples, int rate)
{
    CutoffAnalyser analyser(rate);
    // Feed in uneven blocks, as a decoder would.
    size_t pos = 0, block = 1000;
    while (pos < samples.size()) {
        size_t n = std::min(block, samples.size() - pos);
        analyser.feed(samples.data() + pos, (int)n);
        pos += n;
        block = block == 1000 ? 1152 : 1000;
    }
    return analyser.finish();
}

static std::string verdict(const CutoffResult& r)
{
    return cutoff_verdict_name(r.verdict);
}

static void test_full_band()
{
    CutoffResult r = analyse(noise(44100 * 3, 0.3f), 44100);
    test("white noise at 44.1 kHz is full band", std::string("full-band"), verdict(r));
    test("all frames active", r.frames, r.active_frames);
    test("no shelf frames", 0, r.shelf_frames);
}

static void test_lossy_shelf()
{
    CutoffResult r = analyse(lowpass(noise(44100 * 3, 0.3f), 44100, 16000), 44100);
    test("16 kHz low-pass at 44.1 kHz is lossy", std::string("lossy"), verdict(r));
    test("cutoff near 16 kHz", true, r.cutoff_hz > 15000 && r.cutoff_hz < 16500);
    test("consistent cutoff", true, r.consistency > 0.9);
    test("codec hint", std::string("MP3 128k"), r.codec_hint);
}

static void test_upsampled()
{
    CutoffResult r = analyse(lowpass(noise(96000 * 3, 0.3f), 96000, 21500), 96000);
    test("96 kHz band-limited to 21.5 kHz is upsampled", std::string("upsampled"), verdict(r));
    test("source rate 44.1 kHz", 44100, r.source_rate);

    r = analyse(lowpass(noise(96000 * 3, 0.3f), 96000, 16000), 96000);
    test("96 kHz with a 16 kHz shelf is lossy+upsampled", std::string("lossy+upsampled"), verdict(r));
}

static void test_upsampled_with_hiss()
{
    // 44.1 kHz material upsampled to 96 kHz, then steady hiss 60 dB down
    // filling 23-46 kHz: the spectrum runs to the top, the wall is underneath.
    const int rate = 96000;
    std::vector<float> music = lowpass(noise(rate * 3, 0.3f), rate, 21500);
    std::vector<float> wide = noise(rate * 3, 0.0003f, 99991u);
    std::vector<float> hi = lowpass(wide, rate, 46000);
    std::vector<float> lo = lowpass(wide, rate, 23000);
    for (size_t i = 0; i < music.size(); ++i) {
        music[i] += hi[i] - lo[i];
    }
    CutoffResult r = analyse(music, rate);
    test("upsampled with hiss on top is upsampled", std::string("upsampled"), verdict(r));
    test("hiss: source rate 44.1 kHz", 44100, r.source_rate);
}

static void test_mixed()
{
    const int rate = 96000;
    std::vector<float> x = noise(rate * 8, 0.3f);
    std::vector<float> section(x.begin() + rate * 3, x.begin() + rate * 6);
    section = lowpass(section, rate, 18000, 127);
    std::copy(section.begin(), section.end(), x.begin() + rate * 3);
    CutoffResult r = analyse(x, rate);
    test("96 kHz with a 3 s low-passed section is mixed", std::string("mixed"), verdict(r));
    test("one lossy segment", (size_t)1, r.lossy_segments.size());
    if (!r.lossy_segments.empty()) {
        test("segment starts near 3 s", true, fabs(r.lossy_segments[0].start - 3.0) < 0.2);
        test("segment ends near 6 s", true, fabs(r.lossy_segments[0].end - 6.0) < 0.2);
    }
}

static void test_silence()
{
    CutoffResult r = analyse(std::vector<float>(44100 * 3, 0.0f), 44100);
    test("silence is unknown", std::string("unknown"), verdict(r));
    test("no active frames", 0, r.active_frames);
}

// Deterministic Gaussian noise (sum of 12 uniforms), standard deviation `sd`.
static std::vector<double> gauss(int count, double sd, uint32_t seed = 88172645u)
{
    std::vector<double> out(count);
    uint32_t x = seed;
    for (int i = 0; i < count; ++i) {
        double sum = 0;
        for (int k = 0; k < 12; ++k) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            sum += x / 4294967296.0;
        }
        out[i] = sd * (sum - 6.0);
    }
    return out;
}

static BitDepthResult depth(const std::vector<int32_t>& samples, int bits)
{
    BitDepthAnalyser analyser(bits);
    size_t pos = 0;
    while (pos < samples.size()) {
        size_t n = std::min((size_t)4096, samples.size() - pos);
        analyser.feed(samples.data() + pos, (int)n);
        pos += n;
    }
    return analyser.finish();
}

static std::string verdict(const BitDepthResult& r)
{
    return bitdepth_verdict_name(r.verdict);
}

static const int DEPTH_SAMPLES = 400000;

static void test_bitdepth_native()
{
    std::vector<double> x = gauss(DEPTH_SAMPLES, 65536.0);
    std::vector<int32_t> s(x.size());
    for (size_t i = 0; i < x.size(); ++i) s[i] = (int32_t)lrint(x[i]);
    BitDepthResult r = depth(s, 24);
    test("24-bit noise is native", std::string("native"), verdict(r));
    test("no zero low bits", 0, r.zero_low_bits);
    test("no comb", true, r.comb_db < 19.0);

    std::vector<int32_t> s16(x.size());
    for (size_t i = 0; i < x.size(); ++i) s16[i] = (int32_t)lrint(x[i] / 256.0);
    test("16-bit noise in a 16-bit file is native", std::string("native"), verdict(depth(s16, 16)));
}

static void test_bitdepth_padded()
{
    std::vector<double> x = gauss(DEPTH_SAMPLES, 256.0);
    std::vector<int32_t> s(x.size());
    for (size_t i = 0; i < x.size(); ++i) s[i] = (int32_t)lrint(x[i]) * 256;
    BitDepthResult r = depth(s, 24);
    test("16-bit padded with zeros is padded", std::string("padded"), verdict(r));
    test("8 zero low bits", 8, r.zero_low_bits);
    test("effective 16 bits", 16, r.effective_bits);
}

static void test_bitdepth_requantised()
{
    std::vector<double> x = gauss(DEPTH_SAMPLES, 256.0);
    std::vector<double> dither = gauss(DEPTH_SAMPLES, 4.0, 1234567u);
    std::vector<int32_t> gained(x.size()), dithered(x.size());
    const double g = pow(10.0, -0.5 / 20);
    for (size_t i = 0; i < x.size(); ++i) {
        double v16 = (double)lrint(x[i]);
        gained[i] = (int32_t)lrint(v16 * 256.0 * g);
        dithered[i] = (int32_t)lrint(v16 * 256.0 + dither[i]);
    }
    BitDepthResult r = depth(gained, 24);
    test("16-bit with a gain change is requantised", std::string("requantised"), verdict(r));
    test("lattice step 256 * gain", true, fabs(r.comb_period - 256.0 * g) < 2.0);
    test("gain change: effective 16 bits", 16, r.effective_bits);

    r = depth(dithered, 24);
    test("16-bit with 24-bit dither is requantised", std::string("requantised"), verdict(r));
    test("lattice step 256", true, fabs(r.comb_period - 256.0) < 2.0);
    test("dither: effective 16 bits", 16, r.effective_bits);
}

static void test_bitdepth_silence()
{
    test("silence is unknown", std::string("unknown"),
         verdict(depth(std::vector<int32_t>(DEPTH_SAMPLES, 0), 24)));
    test("32-bit is unknown", std::string("unknown"),
         verdict(depth(std::vector<int32_t>(1000, 1), 32)));
}

void test_analysis()
{
    run("cutoff full band", test_full_band);
    run("cutoff lossy shelf", test_lossy_shelf);
    run("cutoff upsampled", test_upsampled);
    run("cutoff upsampled with hiss", test_upsampled_with_hiss);
    run("cutoff mixed", test_mixed);
    run("cutoff silence", test_silence);
    run("bit depth native", test_bitdepth_native);
    run("bit depth padded", test_bitdepth_padded);
    run("bit depth requantised", test_bitdepth_requantised);
    run("bit depth silence", test_bitdepth_silence);
}
