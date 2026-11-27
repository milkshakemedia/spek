#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "spek-audio.h"
#include "spek-fft.h"

#include "spek-analysis.h"

namespace {

// Spectrum bands are this wide; each band's level is the median of its bins,
// so a single tone doesn't count as "content" the way broadband energy does.
const double BAND_HZ = 250.0;
// A shelf edge needs the band levels just below it (this many bands) ...
const int BELOW_BANDS = 4;
// ... to be this far above the loudest band from this many bands above it to
// the top. The gap allows for a resampler's or encoder's transition band.
const int GAP_BANDS = 2;
const double SHELF_DROP_DB = 20.0;
// The whole-file average is smoother than a single frame, so a smaller drop
// still means a brick wall: the drop is measured across about 1 kHz, far
// steeper than any natural roll-off. Corpus: false positives start at 6 dB.
const double AVG_DROP_DB = 12.0;
// Under a noise plateau that runs to the top, a wall must drop this far, and
// sit at a standard source Nyquist (44.1 or 48 kHz rates: below WALL_MAX_HZ).
// Real recordings don't end in a 30 dB wall at exactly 22.05 or 24 kHz.
const double WALL_DROP_DB = 30.0;
const double WALL_MAX_HZ = 24500.0;
// Music-following test, for hi-res files whose top is filled with noise louder
// than the music near 20 kHz (no wall to find). Over the active frames, each
// 1 kHz band's level is correlated with the 4-12 kHz level. In an upsample the
// bands just under the source's Nyquist follow the music and those above
// don't follow it at all. Corpus: files with real ultrasonic content keep 0.16
// or more above the Nyquist, the hiss-filled upsample sits at -0.01; the
// threshold is halfway. Files with no dynamics (pink noise) never reach the
// 0.35 needed below the Nyquist, so they are left alone.
const double TRACK_FROM_HZ = 16000.0;
const double TRACK_TO_HZ = 34000.0;
const double TRACK_BELOW_MIN = 0.35;  // mean correlation 1-5 kHz under the Nyquist
const double TRACK_ABOVE_MAX = 0.06;  // mean correlation 1-9 kHz over it
const int TRACK_MIN_FRAMES = 50;
// Edges are only looked for above this frequency.
const double MIN_EDGE_HZ = 4000.0;
// A frame is judged only if its median level from 1 to 15 kHz, normalised to
// the frame size, is above this: about 20 dB over an empty 24-bit file's floor.
// A 16-bit dither floor (about -107) counts as content on purpose: a lossy
// encoder strips it above its low-pass too, which shows up even in quiet music.
const double ACTIVE_LEVEL_DB = -135.0;
// Lossy encoders low-pass below this; CD anti-alias filters sit above it.
const double LOSSY_MAX_HZ = 21000.0;
// Share of active frames needed for a whole-file verdict.
const double MAJORITY = 0.8;
const double MINORITY = 0.25;  // share of active frames on one shared band-limit edge
// A lossy section must last this long to count in a MIXED verdict ...
const double MIN_SEGMENT_S = 2.0;
// ... and is not broken by gaps up to this long (a transient can briefly
// fill the spectrum above the encoder's low-pass).
const double MAX_GAP_S = 0.35;

double median(std::vector<double> v)
{
    if (v.empty()) {
        return 0.0;
    }
    size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    double m = v[mid];
    if (v.size() % 2 == 0) {
        m = (m + *std::max_element(v.begin(), v.begin() + mid)) / 2.0;
    }
    return m;
}

int frame_bits(int sample_rate)
{
    // About 85 ms per frame: 4096 samples at 44.1/48 kHz, 8192 at 88.2/96 kHz,
    // 16384 at 176.4/192 kHz. The band math needs a few bins per 250 Hz band.
    int bits = 12;
    while (bits < 16 && (1 << (bits + 1)) <= sample_rate * 0.17) {
        ++bits;
    }
    return bits;
}

enum class FrameClass { INACTIVE, FULL, LOSSY, BAND_LIMITED };

FrameClass classify(const CutoffFrame& f, int sample_rate)
{
    if (!f.active) {
        return FrameClass::INACTIVE;
    }
    double nyquist = sample_rate / 2.0;
    if (f.cutoff_hz <= 0) {
        return FrameClass::FULL;
    }
    if (f.cutoff_hz < LOSSY_MAX_HZ) {
        return FrameClass::LOSSY;
    }
    if (sample_rate > 48000 && f.cutoff_hz < 0.9 * nyquist) {
        return FrameClass::BAND_LIMITED;
    }
    return FrameClass::FULL;
}

int likely_source_rate(double edge_hz)
{
    // A good resampler (soxr) puts the edge at about 96% of the source's
    // Nyquist frequency. An edge well clear of every standard Nyquist (FFmpeg's
    // default swresample leaks a few kHz past it) leaves the source unknown: 0.
    const int rates[] = { 44100, 48000, 88200, 96000, 176400 };
    for (int rate : rates) {
        double nyquist = rate / 2.0;
        if (edge_hz <= nyquist + 300.0) {
            return edge_hz >= 0.85 * nyquist ? rate : 0;
        }
    }
    return 0;
}

std::string codec_hint(double cutoff_hz)
{
    // Measured on the test corpus (LAME 3.100, FFmpeg's AAC, libopus); other
    // encoders and settings differ, so this is a hint, not an identification.
    if (cutoff_hz < 15500) return "low-bitrate MP3 or AAC (under 128k)";
    if (cutoff_hz < 16900) return "MP3 128k";
    if (cutoff_hz < 17800) return "AAC 128k or MP3 160k";
    if (cutoff_hz < 19300) return "MP3 192k";
    if (cutoff_hz < LOSSY_MAX_HZ) return "MP3 256-320k or Opus";
    return "";
}

// Band levels: the median bin level (dB, normalised by `norm`) in each band of
// `bins_per_band` bins, from bin 1 up to `top_bin`.
std::vector<double> band_levels(const std::vector<double>& bin_db, int bins_per_band, int top_bin)
{
    std::vector<double> bands;
    std::vector<double> bins;
    for (int start = 1; start + bins_per_band <= top_bin; start += bins_per_band) {
        bins.assign(bin_db.begin() + start, bin_db.begin() + start + bins_per_band);
        bands.push_back(median(bins));
    }
    return bands;
}

// Find the highest shelf edge in a band spectrum. Returns false if there is none.
bool find_shelf(const std::vector<double>& bands, double band_hz, double& edge_hz, double& drop_db,
                double min_drop = SHELF_DROP_DB, double max_edge_hz = 1e12)
{
    const int nbands = (int)bands.size();
    // top[k]: loudest band from k up to the top of the analysed range.
    std::vector<double> top(nbands + 1, -1e9);
    for (int k = nbands - 1; k >= 0; --k) {
        top[k] = std::max(top[k + 1], bands[k]);
    }
    // The highest edge with a big enough drop is where the content ends.
    int start = nbands - GAP_BANDS - 1;
    if (max_edge_hz / band_hz < start) {
        start = (int)(max_edge_hz / band_hz);
    }
    for (int k = start; k >= BELOW_BANDS; --k) {
        if (k * band_hz < MIN_EDGE_HZ) {
            break;
        }
        std::vector<double> below(bands.begin() + k - BELOW_BANDS, bands.begin() + k);
        double drop = median(below) - top[k + GAP_BANDS];
        if (drop >= min_drop) {
            // k sits near the bottom of the stop band. Report the edge where
            // the level first falls 10 dB under the pass band just below the
            // transition, comparable to an encoder's low-pass setting.
            int lo = std::max(0, k - 2 * BELOW_BANDS);
            std::vector<double> pass(bands.begin() + lo, bands.begin() + k - BELOW_BANDS + 1);
            double ref = median(pass);
            int edge = k;
            for (int j = lo; j <= k; ++j) {
                if (bands[j] < ref - 10.0) {
                    edge = j;
                    break;
                }
            }
            edge_hz = edge * band_hz;
            drop_db = drop;
            return true;
        }
    }
    return false;
}

} // namespace

const char *cutoff_verdict_name(CutoffVerdict verdict)
{
    switch (verdict) {
    case CutoffVerdict::FULL_BAND: return "full-band";
    case CutoffVerdict::LOSSY: return "lossy";
    case CutoffVerdict::UPSAMPLED: return "upsampled";
    case CutoffVerdict::LOSSY_UPSAMPLED: return "lossy+upsampled";
    case CutoffVerdict::MIXED: return "mixed";
    case CutoffVerdict::UNKNOWN: break;
    }
    return "unknown";
}

CutoffAnalyser::CutoffAnalyser(int sample_rate) :
    sample_rate(sample_rate), nbits(frame_bits(sample_rate)), nfft(1 << nbits),
    window(nfft), plan(FFT().create(nbits)), consumed(0), power_frames(0)
{
    // 4-term Blackman-Harris: -92 dB side lobes, so a 100 dB shelf stays sharp.
    for (int i = 0; i < nfft; ++i) {
        double x = 2.0 * M_PI * i / (nfft - 1);
        this->window[i] = 0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x);
    }
    this->pending.reserve(nfft);
}

CutoffAnalyser::~CutoffAnalyser() {}

void CutoffAnalyser::feed(const float *samples, int count)
{
    for (int i = 0; i < count; ++i) {
        this->pending.push_back(samples[i]);
        if ((int)this->pending.size() == this->nfft) {
            this->analyse_frame();
            this->pending.clear();
        }
    }
}

void CutoffAnalyser::analyse_frame()
{
    for (int i = 0; i < this->nfft; ++i) {
        this->plan->set_input(i, this->pending[i] * this->window[i]);
    }
    this->plan->execute();

    CutoffFrame frame;
    frame.time = (double)this->consumed / this->sample_rate;
    frame.active = false;
    frame.cutoff_hz = 0;
    frame.drop_db = 0;
    this->consumed += this->nfft;

    // Band levels: median bin level in each 250 Hz band, normalised so that
    // they don't depend on the frame size.
    const double bin_hz = (double)this->sample_rate / this->nfft;
    const double nyquist = this->sample_rate / 2.0;
    const int bins_per_band = std::max(1, (int)lround(BAND_HZ / bin_hz));
    const double norm = 10.0 * log10((double)this->nfft);
    const int top_bin = (int)(0.98 * nyquist / bin_hz);
    std::vector<double> bin_db(this->plan->get_output_size());
    for (size_t b = 0; b < bin_db.size(); ++b) {
        double v = this->plan->get_output(b);
        if (!std::isfinite(v) || v < -300.0) {
            v = -300.0;  // digital silence
        }
        bin_db[b] = v + norm;
    }
    std::vector<double> bands = band_levels(bin_db, bins_per_band, top_bin);
    const double band_hz = bins_per_band * bin_hz;
    const int nbands = (int)bands.size();

    // Active: median level from 1 to 15 kHz.
    std::vector<double> mid;
    for (int k = 0; k < nbands; ++k) {
        double f = k * band_hz;
        if (f >= 1000.0 && f < std::min(15000.0, nyquist)) {
            mid.push_back(bands[k]);
        }
    }
    frame.active = !mid.empty() && median(mid) > ACTIVE_LEVEL_DB;

    if (frame.active) {
        bool found = find_shelf(bands, band_hz, frame.cutoff_hz, frame.drop_db);
        // Noise filling the top. If the spectrum runs (nearly) to Nyquist, look
        // under it: a wall at a standard source rate's Nyquist, with only a far
        // quieter plateau above, is an upsample with noise or dither added.
        if (this->sample_rate > 48000 && (!found || frame.cutoff_hz >= 0.9 * nyquist)) {
            double edge = 0, drop = 0;
            if (find_shelf(bands, band_hz, edge, drop, WALL_DROP_DB, WALL_MAX_HZ)) {
                int source = likely_source_rate(edge);
                if (source && source < this->sample_rate) {
                    frame.cutoff_hz = edge;
                    frame.drop_db = drop;
                }
            }
        }
        // Levels for the music-following test.
        if (this->sample_rate > 48000 && 0.9 * nyquist >= TRACK_TO_HZ) {
            auto level = [&](double lo, double hi) {
                double sum = 0;
                int n = 0;
                for (int b = (int)(lo / bin_hz); b < (int)(hi / bin_hz); ++b) {
                    sum += pow(10.0, bin_db[b] / 10.0);
                    ++n;
                }
                return 10.0 * log10(sum / std::max(1, n) + 1e-30);
            };
            std::vector<double> row;
            row.push_back(level(4000.0, 12000.0));
            for (double f = TRACK_FROM_HZ; f < TRACK_TO_HZ; f += 1000.0) {
                row.push_back(level(f, f + 1000.0));
            }
            this->track.push_back(row);
        }
        // Power average over active frames, for the whole-file check.
        if (this->power_sum.empty()) {
            this->power_sum.assign(bin_db.size(), 0.0);
        }
        for (size_t b = 0; b < bin_db.size(); ++b) {
            this->power_sum[b] += pow(10.0, bin_db[b] / 10.0);
        }
        ++this->power_frames;
    }
    this->frames.push_back(frame);
}

CutoffResult CutoffAnalyser::finish()
{
    CutoffResult r;
    r.sample_rate = this->sample_rate;
    r.frames = (int)this->frames.size();
    const double frame_s = (double)this->nfft / this->sample_rate;

    std::vector<double> lossy_edges, band_edges, drops;
    int n_full = 0, n_lossy = 0, n_band = 0;
    for (const CutoffFrame& f : this->frames) {
        switch (classify(f, this->sample_rate)) {
        case FrameClass::INACTIVE: continue;
        case FrameClass::FULL: ++n_full; break;
        case FrameClass::LOSSY: ++n_lossy; lossy_edges.push_back(f.cutoff_hz); break;
        case FrameClass::BAND_LIMITED: ++n_band; band_edges.push_back(f.cutoff_hz); break;
        }
        ++r.active_frames;
        if (f.cutoff_hz > 0) {
            ++r.shelf_frames;
            drops.push_back(f.drop_db);
        }
    }
    r.drop_db = median(drops);

    // Runs of lossy frames, bridging short gaps.
    double run_start = -1, run_end = 0;
    std::vector<double> run_edges;
    auto close_run = [&]() {
        if (run_start >= 0 && run_end - run_start >= MIN_SEGMENT_S) {
            r.lossy_segments.push_back({ run_start, run_end, median(run_edges) });
        }
        run_start = -1;
        run_edges.clear();
    };
    for (const CutoffFrame& f : this->frames) {
        if (classify(f, this->sample_rate) != FrameClass::LOSSY) {
            continue;
        }
        if (run_start >= 0 && f.time - run_end > MAX_GAP_S) {
            close_run();
        }
        if (run_start < 0) {
            run_start = f.time;
        }
        run_end = f.time + frame_s;
        run_edges.push_back(f.cutoff_hz);
    }
    close_run();

    const double active = r.active_frames;
    if (r.active_frames < 10) {
        r.verdict = CutoffVerdict::UNKNOWN;
    } else if (n_lossy >= MAJORITY * active) {
        r.verdict = this->sample_rate > 48000 ? CutoffVerdict::LOSSY_UPSAMPLED : CutoffVerdict::LOSSY;
        r.cutoff_hz = median(lossy_edges);
        r.confidence = n_lossy / active;
    } else if (n_band >= MAJORITY * active) {
        r.verdict = CutoffVerdict::UPSAMPLED;
        r.cutoff_hz = median(band_edges);
        r.source_rate = likely_source_rate(r.cutoff_hz);
        r.confidence = n_band / active;
    } else if (n_full >= 0.5 * active && !r.lossy_segments.empty()) {
        r.verdict = CutoffVerdict::MIXED;
        r.cutoff_hz = median(lossy_edges);
        double lossy_s = 0;
        for (const CutoffSegment& s : r.lossy_segments) {
            lossy_s += s.end - s.start;
        }
        // Agreement: frames in the lossy runs plus the full-band frames.
        r.confidence = std::min(1.0, (lossy_s / frame_s + n_full) / active);
    } else if (n_full >= MAJORITY * active) {
        r.verdict = CutoffVerdict::FULL_BAND;
        r.confidence = n_full / active;
    } else {
        // No clear majority: report the plurality with its (low) share.
        int best = std::max(n_full, std::max(n_lossy, n_band));
        if (best == n_lossy) {
            r.verdict = this->sample_rate > 48000 ? CutoffVerdict::LOSSY_UPSAMPLED : CutoffVerdict::LOSSY;
            r.cutoff_hz = median(lossy_edges);
        } else if (best == n_band) {
            r.verdict = CutoffVerdict::UPSAMPLED;
            r.cutoff_hz = median(band_edges);
            r.source_rate = likely_source_rate(r.cutoff_hz);
        } else {
            r.verdict = CutoffVerdict::FULL_BAND;
        }
        r.confidence = best / active;
    }

    // A consistent minority. In quiet material many frames have nothing near
    // the top to cut, but where the shelf does show it sits at the same
    // frequency every time. Genuine files rarely put more than 1-2% of their
    // frames on one shared edge (corpus); a quarter of them is not chance.
    if (r.verdict == CutoffVerdict::FULL_BAND && n_band >= MINORITY * active) {
        double edge = median(band_edges);
        int near = 0;
        for (double e : band_edges) {
            if (fabs(e - edge) < 750) {
                ++near;
            }
        }
        if (near >= MINORITY * active) {
            r.verdict = CutoffVerdict::UPSAMPLED;
            r.cutoff_hz = edge;
            r.source_rate = likely_source_rate(edge);
            r.confidence = near / active;
        }
    }

    // Whole-file check. Quiet or sparse material can bury a frame's shelf under
    // the noise floor, so a power average over all active frames can show a
    // shelf no single frame does. It's weaker evidence (a genuinely dark master
    // averages the same way), so it only fills in when the frames saw nothing,
    // with confidence capped at 0.5.
    if (this->power_frames >= 10 && r.verdict == CutoffVerdict::FULL_BAND) {
        const double bin_hz = (double)this->sample_rate / this->nfft;
        const int bins_per_band = std::max(1, (int)lround(BAND_HZ / bin_hz));
        const int top_bin = (int)(0.98 * this->sample_rate / 2.0 / bin_hz);
        std::vector<double> avg_db(this->power_sum.size());
        for (size_t b = 0; b < avg_db.size(); ++b) {
            avg_db[b] = 10.0 * log10(this->power_sum[b] / this->power_frames + 1e-300);
        }
        std::vector<double> bands = band_levels(avg_db, bins_per_band, top_bin);
        double edge = 0, drop = 0;
        if (find_shelf(bands, bins_per_band * bin_hz, edge, drop, AVG_DROP_DB)) {
            CutoffFrame avg = { 0.0, true, edge, drop };
            FrameClass c = classify(avg, this->sample_rate);
            if (c == FrameClass::LOSSY || c == FrameClass::BAND_LIMITED) {
                r.basis = "average";
                r.cutoff_hz = edge;
                r.drop_db = drop;
                r.confidence = 0.5;
                if (c == FrameClass::LOSSY) {
                    r.verdict = this->sample_rate > 48000 ? CutoffVerdict::LOSSY_UPSAMPLED
                                                          : CutoffVerdict::LOSSY;
                } else {
                    r.verdict = CutoffVerdict::UPSAMPLED;
                    r.source_rate = likely_source_rate(edge);
                }
            }
        }
    }

    // Music-following test: only when nothing else found a band limit.
    if (r.verdict == CutoffVerdict::FULL_BAND && (int)this->track.size() >= TRACK_MIN_FRAMES) {
        const size_t nt = this->track.size();
        auto corr = [&](size_t col) {
            double ma = 0, mb = 0;
            for (const auto& row : this->track) {
                ma += row[0];
                mb += row[col];
            }
            ma /= nt;
            mb /= nt;
            double sab = 0, saa = 0, sbb = 0;
            for (const auto& row : this->track) {
                double a = row[0] - ma, b = row[col] - mb;
                sab += a * b;
                saa += a * a;
                sbb += b * b;
            }
            return saa > 0 && sbb > 0 ? sab / sqrt(saa * sbb) : 0.0;
        };
        // Mean correlation of the 1 kHz bands in [lo, hi).
        auto mean_corr = [&](double lo, double hi) {
            double sum = 0;
            int n = 0;
            for (double f = lo; f < hi; f += 1000.0) {
                sum += corr(1 + (size_t)lround((f - TRACK_FROM_HZ) / 1000.0));
                ++n;
            }
            return sum / n;
        };
        double best_margin = 0;
        for (int source : { 44100, 48000 }) {
            double nyq = std::floor(source / 2000.0) * 1000.0;  // 22 or 24 kHz
            double below = mean_corr(nyq - 5000.0, nyq - 1000.0);
            double above = mean_corr(nyq + 1000.0, nyq + 9000.0);
            if (below >= TRACK_BELOW_MIN && above <= TRACK_ABOVE_MAX && below - above > best_margin) {
                best_margin = below - above;
                r.verdict = CutoffVerdict::UPSAMPLED;
                r.basis = "tracking";
                r.source_rate = source;
                r.cutoff_hz = source / 2.0;
                r.confidence = 0.5;
            }
        }
    }

    if (r.cutoff_hz > 0) {
        int near = 0;
        for (const CutoffFrame& f : this->frames) {
            if (f.active && f.cutoff_hz > 0 && fabs(f.cutoff_hz - r.cutoff_hz) <= 750.0) {
                ++near;
            }
        }
        r.consistency = active > 0 ? near / active : 0;
        if (r.cutoff_hz < LOSSY_MAX_HZ) {
            r.codec_hint = codec_hint(r.cutoff_hz);
        }
    } else {
        r.consistency = active > 0 ? n_full / active : 0;
    }
    r.frame_log = this->frames;
    return r;
}

CutoffResult analyse_cutoff(AudioFile& file)
{
    CutoffAnalyser analyser(file.get_sample_rate());
    if (!!file.get_error()) {
        return analyser.finish();
    }
    file.start(0, 1);
    int n;
    while ((n = file.read()) > 0) {
        analyser.feed(file.get_buffer(), n);
    }
    return analyser.finish();
}

// ---------------------------------------------------------------------------
// True bit depth.

namespace {

// Histogram reach for 24-bit files, in codes either side of zero: 2^19 codes
// is 2048 steps of a 16-bit source, plenty to see a lattice.
const int HIST_RANGE_24 = 1 << 19;
// The comb test wants this many samples inside its window ...
const long long COMB_SAMPLES = 200000;
// ... and never looks at less than this many codes either side of zero.
const int COMB_MIN_RANGE = 1 << 16;
// Too few samples near zero to judge.
const long long COMB_MIN_SAMPLES = 20000;
// The histogram's own shape is removed with a moving average this wide.
const int COMB_SMOOTH = 1024;
// Lattice steps looked for, in 24-bit codes: 12 (a 20-bit source) to 512
// (a 15-bit source). Shorter periods say nothing about a lower source depth.
const double COMB_MIN_PERIOD = 12.0;
const double COMB_MAX_PERIOD = 512.0;
// Each spectrum bin is compared with the median of this many bins either side.
const int COMB_NEIGHBOURS = 40;
// A comb peak must stand this far above its neighbours. Corpus: genuine files
// reach 12.6 dB at most, resampled and lossy ones 15.0 dB, fake 24-bit files
// 22.2 dB or more. The neighbourhood median of pure noise puts the largest of
// tens of thousands of bins about 12 dB up by chance alone.
const double COMB_MIN_DB = 19.0;
// Zero low bits in every sample this many or more means padding (a 20-bit or
// shallower source); fewer could be chance in a short quiet file.
const int PAD_MIN_BITS = 4;

} // namespace

const char *bitdepth_verdict_name(BitDepthVerdict verdict)
{
    switch (verdict) {
    case BitDepthVerdict::NATIVE: return "native";
    case BitDepthVerdict::PADDED: return "padded";
    case BitDepthVerdict::REQUANTISED: return "requantised";
    case BitDepthVerdict::UNKNOWN: break;
    }
    return "unknown";
}

BitDepthAnalyser::BitDepthAnalyser(int container_bits) :
    container_bits(container_bits), used_bits(0), samples(0),
    range(container_bits == 24 ? HIST_RANGE_24 : 0), hist(2 * (size_t)range, 0)
{
}

void BitDepthAnalyser::feed(const int32_t *samples, int count)
{
    for (int i = 0; i < count; ++i) {
        int32_t v = samples[i];
        this->used_bits |= (uint32_t)(v < 0 ? -(int64_t)v : v);
        if (v >= -this->range && v < this->range) {
            ++this->hist[v + this->range];
        }
    }
    this->samples += count;
}

BitDepthResult BitDepthAnalyser::finish()
{
    BitDepthResult r;
    r.container_bits = this->container_bits;
    r.samples = this->samples;
    if ((this->container_bits != 16 && this->container_bits != 24) || this->used_bits == 0) {
        return r;
    }

    while (r.zero_low_bits < this->container_bits && !(this->used_bits & (1u << r.zero_low_bits))) {
        ++r.zero_low_bits;
    }
    if (r.zero_low_bits >= PAD_MIN_BITS) {
        // Four or more bits never set over a whole file of audio is no accident.
        r.verdict = BitDepthVerdict::PADDED;
        r.effective_bits = this->container_bits - r.zero_low_bits;
        r.confidence = 0.99;
        return r;
    }
    r.verdict = BitDepthVerdict::NATIVE;
    if (this->range == 0) {
        return r;  // a 16-bit file has no lower depth worth looking for
    }

    // Window: the smallest power of two either side of zero holding enough
    // samples, so quiet and loud files get a comparable test.
    long long inside = this->hist[this->range];
    int reach = 1;
    while (reach < this->range && inside < COMB_SAMPLES) {
        inside += this->hist[this->range + reach] + this->hist[this->range - reach];
        ++reach;
    }
    int w = COMB_MIN_RANGE;
    while (w < reach && w < this->range) {
        w <<= 1;
    }
    const int n = 2 * w;
    long long in_window = 0;
    for (int i = -w; i < w; ++i) {
        in_window += this->hist[this->range + i];
    }
    if (in_window < COMB_MIN_SAMPLES) {
        r.verdict = BitDepthVerdict::UNKNOWN;
        return r;
    }

    // Remove the histogram's smooth shape, keep the fine structure.
    std::vector<double> prefix(n + 1, 0.0);
    for (int i = 0; i < n; ++i) {
        prefix[i + 1] = prefix[i] + this->hist[this->range - w + i];
    }
    int nbits = 0;
    while ((1 << nbits) < n) {
        ++nbits;
    }
    std::unique_ptr<FFTPlan> plan = FFT().create(nbits);
    const int half = COMB_SMOOTH / 2;
    for (int i = 0; i < n; ++i) {
        float v = 0.0f;
        if (i >= COMB_SMOOTH && i < n - COMB_SMOOTH) {
            double mean = (prefix[i + half] - prefix[i - half]) / COMB_SMOOTH;
            v = (float)(this->hist[this->range - w + i] - mean);
        }
        plan->set_input(i, v);
    }
    plan->execute();

    // A lattice of step P puts a sharp peak at bin n/P (and its harmonics).
    const int lo = (int)(n / COMB_MAX_PERIOD) + 1;
    const int hi = std::min((int)(n / COMB_MIN_PERIOD), plan->get_output_size() - COMB_NEIGHBOURS - 1);
    std::vector<double> prominence(hi + 1, -1e9);
    std::vector<double> local;
    double best = -1e9;
    for (int k = lo; k <= hi; ++k) {
        local.clear();
        for (int j = k - COMB_NEIGHBOURS; j <= k + COMB_NEIGHBOURS; ++j) {
            local.push_back(plan->get_output(j));
        }
        prominence[k] = plan->get_output(k) - median(local);
        best = std::max(best, prominence[k]);
    }
    r.comb_db = best;
    if (best < COMB_MIN_DB) {
        return r;
    }
    // The strongest peak may be a harmonic of the lattice step. The step is
    // the longest one whose fundamental, at a whole fraction of the strongest
    // bin, still stands out on its own.
    int peak = lo;
    for (int k = lo; k <= hi; ++k) {
        if (prominence[k] > prominence[peak]) {
            peak = k;
        }
    }
    int fundamental = peak;
    for (int m = 2; peak / m >= lo; ++m) {
        int c = (int)lround((double)peak / m);
        double p = -1e9;
        int at = c;
        for (int j = std::max(lo, c - 1); j <= std::min(hi, c + 1); ++j) {
            if (prominence[j] > p) {
                p = prominence[j];
                at = j;
            }
        }
        if (p >= COMB_MIN_DB) {
            fundamental = at;
        }
    }
    r.comb_period = (double)n / fundamental;
    r.verdict = BitDepthVerdict::REQUANTISED;
    r.effective_bits = (int)lround(this->container_bits - log2(r.comb_period));
    // 0.5 at the threshold, rising to 0.95 by 10 dB over it (the corpus fakes
    // range from 3 to 29 dB over).
    r.confidence = std::min(0.95, 0.5 + 0.045 * (r.comb_db - COMB_MIN_DB));
    return r;
}

BitDepthResult analyse_bitdepth(AudioFile& file)
{
    const int bits = file.get_bits_per_sample();
    BitDepthAnalyser analyser(bits);
    if (!!file.get_error() || (bits != 16 && bits != 24)) {
        return analyser.finish();
    }
    // The decoder hands out floats: 16-bit samples divided by 32767, 24-bit
    // ones (carried in 32 bits) divided by 2^31, both exactly reversible.
    const float scale = bits == 16 ? 32767.0f : 8388608.0f;
    std::vector<int32_t> ints;
    file.start(0, 1);
    int n;
    while ((n = file.read()) > 0) {
        const float *buf = file.get_buffer();
        ints.resize(n);
        for (int i = 0; i < n; ++i) {
            ints[i] = (int32_t)lrintf(buf[i] * scale);
        }
        analyser.feed(ints.data(), n);
    }
    return analyser.finish();
}

// ---------------------------------------------------------------------------
// Everything about one file.

namespace {

std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::string khz(double hz)
{
    return format("%.1f kHz", hz / 1000.0);
}

void summarise(FileAnalysis& a)
{
    const CutoffResult& c = a.cutoff;
    const BitDepthResult& b = a.bitdepth;
    std::vector<std::string> flags;
    double confidence = 0;

    auto pct = [](double x) { return (int)lround(100.0 * x); };
    auto weak = [&]() {
        if (c.basis == "average") {
            a.evidence.push_back("seen only in the whole-file average, not frame by frame (weaker)");
        } else if (c.basis == "tracking") {
            a.evidence.push_back(format(
                "content above %s doesn't follow the music, below it does (weaker)", khz(c.cutoff_hz).c_str()));
        }
    };

    switch (c.verdict) {
    case CutoffVerdict::LOSSY:
    case CutoffVerdict::LOSSY_UPSAMPLED:
        flags.push_back("lossy");
        if (c.verdict == CutoffVerdict::LOSSY_UPSAMPLED) {
            flags.push_back("upsampled");
        }
        a.evidence.push_back(format("spectrum ends at %s in %d%% of frames (%s)", khz(c.cutoff_hz).c_str(),
                                    pct(c.consistency), c.codec_hint.c_str()));
        weak();
        confidence = std::max(confidence, c.confidence);
        break;
    case CutoffVerdict::UPSAMPLED:
        flags.push_back("upsampled");
        if (c.source_rate) {
            a.evidence.push_back(format("nothing from the music above %s: likely a %g kHz source",
                                        khz(c.cutoff_hz).c_str(), c.source_rate / 1000.0));
        } else {
            a.evidence.push_back(format("band-limited at %s", khz(c.cutoff_hz).c_str()));
        }
        weak();
        confidence = std::max(confidence, c.confidence);
        break;
    case CutoffVerdict::MIXED:
        flags.push_back("partly-lossy");
        for (const CutoffSegment& s : c.lossy_segments) {
            a.evidence.push_back(format("lossy shelf at %s from %.1f to %.1f s", khz(s.cutoff_hz).c_str(),
                                        s.start, s.end));
        }
        confidence = std::max(confidence, c.confidence);
        break;
    case CutoffVerdict::FULL_BAND:
    case CutoffVerdict::UNKNOWN:
        break;
    }

    if (b.verdict == BitDepthVerdict::PADDED || b.verdict == BitDepthVerdict::REQUANTISED) {
        flags.push_back("fake-bits");
        if (b.verdict == BitDepthVerdict::PADDED) {
            a.evidence.push_back(format("%d-bit source: the low %d bits are zero in every sample",
                                        b.effective_bits, b.zero_low_bits));
        } else {
            a.evidence.push_back(format(
                "%d-bit source: sample values sit on a %.1f-code lattice (comb %.0f dB), the trace of a gain "
                "change or dither on a %d-bit master", b.effective_bits, b.comb_period, b.comb_db,
                b.effective_bits));
        }
        confidence = std::max(confidence, b.confidence);
    }

    if (!a.lossless && !a.codec.empty()) {
        // A lossy file is what it says it is. Report what the detectors saw,
        // but don't call it a fake.
        a.verdict = "lossy-format";
        a.confidence = 1.0;
        a.evidence.insert(a.evidence.begin(), "lossy codec: " + a.codec);
        return;
    }
    if (!flags.empty()) {
        a.verdict.clear();
        for (const std::string& f : flags) {
            a.verdict += (a.verdict.empty() ? "" : "+") + f;
        }
        a.confidence = confidence;
        return;
    }
    if (c.verdict != CutoffVerdict::FULL_BAND) {
        // Clean needs the cutoff check to have passed; a 16-bit file's bit
        // depth on its own says nothing.
        a.verdict = "unknown";
        a.confidence = 0;
        a.evidence.push_back("too little active audio to judge the spectrum");
        return;
    }
    a.verdict = "clean";
    a.confidence = c.confidence;
    a.evidence.push_back(format("full band in %d%% of frames", pct(c.consistency)));
    if (b.verdict == BitDepthVerdict::NATIVE && b.container_bits == 24) {
        a.evidence.push_back("uses all 24 bits");
    }
}

} // namespace

bool codec_is_lossless(const std::string& codec_name)
{
    // FFmpeg's long names for the lossless audio codecs worth knowing.
    static const char *lossless[] = {
        "FLAC", "PCM", "ALAC", "Apple Lossless", "WavPack", "Monkey", "TTA", "True Audio",
        "MLP", "TrueHD", "lossless", "Lossless", "DSD", "Shorten", "OptimFROG", "TAK",
    };
    for (const char *name : lossless) {
        if (codec_name.find(name) != std::string::npos) {
            return true;
        }
    }
    return false;
}

FileAnalysis analyse_file(AudioFile& file)
{
    FileAnalysis a;
    a.codec = file.get_codec_name();
    a.lossless = codec_is_lossless(a.codec);
    a.sample_rate = file.get_sample_rate();
    a.bits = file.get_bits_per_sample();
    a.channels = file.get_channels();
    a.duration = file.get_duration();

    CutoffAnalyser cutoff(a.sample_rate > 0 ? a.sample_rate : 44100);
    BitDepthAnalyser bitdepth(a.lossless ? a.bits : 0);
    if (!file.get_error()) {
        const bool ints = a.lossless && (a.bits == 16 || a.bits == 24);
        const float scale = a.bits == 16 ? 32767.0f : 8388608.0f;
        std::vector<int32_t> buf;
        file.start(0, 1);
        int n;
        while ((n = file.read()) > 0) {
            const float *samples = file.get_buffer();
            cutoff.feed(samples, n);
            if (ints) {
                buf.resize(n);
                for (int i = 0; i < n; ++i) {
                    buf[i] = (int32_t)lrintf(samples[i] * scale);
                }
                bitdepth.feed(buf.data(), n);
            }
        }
    }
    a.cutoff = cutoff.finish();
    a.bitdepth = bitdepth.finish();
    summarise(a);
    return a;
}
