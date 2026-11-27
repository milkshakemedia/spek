#include <algorithm>
#include <cmath>

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
                double min_drop = SHELF_DROP_DB)
{
    const int nbands = (int)bands.size();
    // top[k]: loudest band from k up to the top of the analysed range.
    std::vector<double> top(nbands + 1, -1e9);
    for (int k = nbands - 1; k >= 0; --k) {
        top[k] = std::max(top[k + 1], bands[k]);
    }
    // The highest edge with a big enough drop is where the content ends.
    for (int k = nbands - GAP_BANDS - 1; k >= BELOW_BANDS; --k) {
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
        find_shelf(bands, band_hz, frame.cutoff_hz, frame.drop_db);
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
