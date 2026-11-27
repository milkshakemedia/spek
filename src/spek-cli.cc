// spek-cli: batch analysis. Point it at files or folders (scanned recursively)
// and get a spectrogram PNG per file plus a CSV and a JSON report of what the
// detectors found. Verdicts are hints with evidence, never proof.

#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
}

#include <wx/image.h>
#include <wx/init.h>

#include "spek-analysis.h"
#include "spek-audio.h"
#include "spek-fft.h"
#include "spek-font.h"
#include "spek-palette.h"
#include "spek-pipeline.h"

namespace {

const char *USAGE =
    "Usage: spek-cli [options] PATH...\n"
    "\n"
    "Analyse audio files for signs of fake hi-res or lossy transcoding. Folders\n"
    "are scanned recursively.\n"
    "\n"
    "Options:\n"
    "  -o, --output DIR   write the report and spectrograms here (default: spek-report)\n"
    "      --no-png       skip the spectrogram images\n"
    "      --width N      spectrogram width in pixels (default: 1024)\n"
    "  -q, --quiet        no progress on stderr\n"
    "  -h, --help         show this help\n"
    "\n"
    "Writes DIR/report.csv, DIR/report.json and DIR/png/<file>.png.\n";

const char *EXTENSIONS[] = {
    "flac", "wav", "wave", "aif", "aiff", "aifc", "m4a", "mp4", "alac", "caf", "wv", "ape",
    "tta", "dsf", "dff", "mp3", "mp2", "ogg", "oga", "opus", "aac", "wma", "mpc", "ac3", "dts",
    "mka", "w64", "rf64",
};

// Spectrogram, as the window draws it: SoX palette, -120..0 dB.
const int FFT_BITS = 10;   // 513 frequency rows
const double LRANGE = -120.0;
const double URANGE = 0.0;
const int LPAD = 46, TPAD = 10, RPAD = 60, BPAD = 24;
const int HEADER = 86;      // metadata and verdict band above the spectrogram

#ifdef _WIN32
const char SEP = '\\';
#else
const char SEP = '/';
#endif

bool is_dir(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool make_dir(const std::string& path)
{
#ifdef _WIN32
    return mkdir(path.c_str()) == 0 || is_dir(path);
#else
    return mkdir(path.c_str(), 0755) == 0 || is_dir(path);
#endif
}

bool make_dirs(const std::string& path)
{
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/' || path[i] == '\\') {
            make_dir(path.substr(0, i));
        }
    }
    return make_dir(path);
}

std::string lower(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    }
    return s;
}

bool is_audio(const std::string& name)
{
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = lower(name.substr(dot + 1));
    for (const char *e : EXTENSIONS) {
        if (ext == e) return true;
    }
    return false;
}

struct Item
{
    std::string path;   // as opened
    std::string rel;    // relative to the PATH argument it came from
};

void scan(const std::string& dir, const std::string& rel, std::vector<Item>& out)
{
    DIR *d = opendir(dir.c_str());
    if (!d) {
        fprintf(stderr, "spek-cli: can't read folder %s\n", dir.c_str());
        return;
    }
    std::vector<std::string> names;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] != '.') names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        std::string path = dir + SEP + name;
        std::string r = rel.empty() ? name : rel + "/" + name;
        if (is_dir(path)) {
            scan(path, r, out);
        } else if (is_audio(name)) {
            out.push_back({ path, r });
        }
    }
}

// --- Tags -----------------------------------------------------------------

struct Tags
{
    std::string artist, title, album, format;  // format: container's short codec name, e.g. "FLAC"
    int64_t bit_rate = 0;                       // bits per second, whole file
};

std::string tag(AVDictionary *d, const char *key)
{
    AVDictionaryEntry *e = d ? av_dict_get(d, key, nullptr, 0) : nullptr;
    return e && e->value ? e->value : "";
}

Tags read_tags(const std::string& path)
{
    Tags t;
    AVFormatContext *fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) != 0) return t;
    avformat_find_stream_info(fc, nullptr);
    int s = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    AVDictionary *sd = s >= 0 ? fc->streams[s]->metadata : nullptr;
    // FLAC and ID3 keep tags on the file, Ogg on the stream.
    for (const char *k : { "artist", "album_artist" }) {
        if (t.artist.empty()) t.artist = tag(fc->metadata, k);
        if (t.artist.empty()) t.artist = tag(sd, k);
    }
    t.title = tag(fc->metadata, "title");
    if (t.title.empty()) t.title = tag(sd, "title");
    t.album = tag(fc->metadata, "album");
    if (t.album.empty()) t.album = tag(sd, "album");
    if (s >= 0) {
        t.format = avcodec_get_name(fc->streams[s]->codecpar->codec_id);
        for (char& c : t.format) {
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        }
        if (t.format.rfind("PCM_", 0) == 0) t.format = "PCM";
    }
    t.bit_rate = fc->bit_rate;
    avformat_close_input(&fc);
    return t;
}

// --- Spectrogram ----------------------------------------------------------

struct Collector
{
    std::mutex lock;
    std::condition_variable done_cv;
    bool done = false;
    int bands = 0;
    int width = 0;
    std::vector<float> values;  // width x bands, column-major
};

void collect(int bands, int sample, float *values, void *data)
{
    Collector *c = static_cast<Collector *>(data);
    std::lock_guard<std::mutex> guard(c->lock);
    if (sample < 0) {
        c->done = true;
        c->done_cv.notify_all();
        return;
    }
    if (sample < c->width && bands == c->bands) {
        std::copy(values, values + bands, c->values.begin() + (size_t)sample * bands);
    }
}

// A 3x5 pixel font for axis labels, drawn at twice the size.
const char *glyph(char ch)
{
    switch (ch) {
    case '0': return "111101101101111";
    case '1': return "010110010010111";
    case '2': return "111001111100111";
    case '3': return "111001111001111";
    case '4': return "101101111001001";
    case '5': return "111100111001111";
    case '6': return "111100111101111";
    case '7': return "111001001001001";
    case '8': return "111101111101111";
    case '9': return "111101111001111";
    case 'k': return "100101110101101";
    case 's': return "011100010001110";
    case ':': return "000010000010000";
    case '.': return "000000000000010";
    }
    return nullptr;
}

void put(wxImage& img, int x, int y, uint32_t rgb)
{
    if (x >= 0 && y >= 0 && x < img.GetWidth() && y < img.GetHeight()) {
        img.SetRGB(x, y, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
    }
}

int text_width(const std::string& s)
{
    return (int)s.size() * 8 - 2;
}

void text(wxImage& img, int x, int y, const std::string& s, uint32_t rgb)
{
    for (char ch : s) {
        const char *g = glyph(ch);
        if (g) {
            for (int r = 0; r < 5; ++r) {
                for (int c = 0; c < 3; ++c) {
                    if (g[r * 3 + c] == '1') {
                        put(img, x + 2 * c, y + 2 * r, rgb);
                        put(img, x + 2 * c + 1, y + 2 * r, rgb);
                        put(img, x + 2 * c, y + 2 * r + 1, rgb);
                        put(img, x + 2 * c + 1, y + 2 * r + 1, rgb);
                    }
                }
            }
        }
        x += 8;
    }
}

// Anti-aliased text from the baked DejaVu glyphs in spek-font.h.
std::vector<unsigned int> utf8(const std::string& s)
{
    std::vector<unsigned int> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size() + (n ? 0 : 1)) {
            out.push_back('?');
            ++i;
            continue;
        }
        unsigned int cp = n == 0 ? c : n == 1 ? c & 0x1f : n == 2 ? c & 0x0f : c & 0x07;
        for (int k = 1; k <= n; ++k) cp = (cp << 6) | (s[i + k] & 0x3f);
        out.push_back(cp);
        i += n + 1;
    }
    return out;
}

const SpekGlyph *find_glyph(const SpekFont& f, unsigned int cp)
{
    for (int i = 0; i < f.count; ++i) {
        if (f.glyphs[i].cp == cp) return &f.glyphs[i];
    }
    for (int i = 0; i < f.count; ++i) {
        if (f.glyphs[i].cp == '?') return &f.glyphs[i];
    }
    return nullptr;
}

void blend(wxImage& img, int x, int y, uint32_t rgb, int alpha)
{
    if (alpha <= 0 || x < 0 || y < 0 || x >= img.GetWidth() || y >= img.GetHeight()) return;
    auto mix = [&](int bg, int fg) { return (unsigned char)((bg * (255 - alpha) + fg * alpha) / 255); };
    img.SetRGB(x, y, mix(img.GetRed(x, y), (rgb >> 16) & 0xff), mix(img.GetGreen(x, y), (rgb >> 8) & 0xff),
               mix(img.GetBlue(x, y), rgb & 0xff));
}

int ttext_width(const SpekFont& f, const std::string& s)
{
    int w = 0;
    for (unsigned int cp : utf8(s)) {
        if (const SpekGlyph *g = find_glyph(f, cp)) w += g->advance;
    }
    return w;
}

// Draws s with its top at y; returns the x after the last glyph.
int ttext(wxImage& img, const SpekFont& f, int x, int y, const std::string& s, uint32_t rgb)
{
    for (unsigned int cp : utf8(s)) {
        const SpekGlyph *g = find_glyph(f, cp);
        if (!g) continue;
        const unsigned char *a = f.alpha + g->offset;
        for (int r = 0; r < f.height; ++r) {
            for (int c = 0; c < g->width; ++c) blend(img, x + c, y + r, rgb, a[r * g->width + c]);
        }
        x += g->advance;
    }
    return x;
}

// Cut s to fit max_w pixels, ending with an ellipsis.
std::string fit(const SpekFont& f, const std::string& s, int max_w)
{
    if (ttext_width(f, s) <= max_w) return s;
    std::vector<unsigned int> cps = utf8(s);
    std::string out;
    for (unsigned int cp : cps) {
        std::string next = out;
        if (cp < 0x80) {
            next += (char)cp;
        } else if (cp < 0x800) {
            next += (char)(0xc0 | (cp >> 6));
            next += (char)(0x80 | (cp & 0x3f));
        } else {
            next += (char)(0xe0 | (cp >> 12));
            next += (char)(0x80 | ((cp >> 6) & 0x3f));
            next += (char)(0x80 | (cp & 0x3f));
        }
        if (ttext_width(f, next + "\xe2\x80\xa6") > max_w) break;
        out = next;
    }
    return out + "\xe2\x80\xa6";
}

void fill(wxImage& img, int x0, int y0, int w, int h, uint32_t rgb)
{
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) put(img, x, y, rgb);
    }
}

std::string time_label(double s)
{
    char buf[32];
    if (s < 60) {
        snprintf(buf, sizeof(buf), "%gs", s);
    } else {
        snprintf(buf, sizeof(buf), "%d:%02d", (int)s / 60, (int)s % 60);
    }
    return buf;
}

std::string freq_label(double hz)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%gk", hz / 1000.0);
    return buf;
}

std::string thousands(long long v)
{
    std::string d = std::to_string(v), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return out;
}

std::string khz(double hz)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%g kHz", std::round(hz / 100.0) / 10.0);
    return buf;
}

// The band above the spectrogram: what the file claims to be, a tick or a
// cross for each claim, and the overall reading.
void draw_header(wxImage& img, const FileAnalysis& a, const Tags& t, const std::string& name)
{
    const int W = img.GetWidth();
    const uint32_t white = 0xffffff, grey = 0x9a9a9a, green = 0x1f9d55, red = 0xc8372d, amber = 0xc98a00,
                   slate = 0x5a5a5a;
    fill(img, 0, 0, W, HEADER, 0x161616);
    fill(img, 0, HEADER - 1, W, 1, 0x333333);

    // Overall badge, right.
    const bool hires_claim = a.sample_rate > 48000 || a.bits > 16;
    uint32_t bcol;
    std::string mark, head;
    if (a.verdict == "clean") {
        bcol = green, mark = "\xe2\x9c\x93", head = hires_claim ? "GENUINE HI-RES" : "LOSSLESS AS CLAIMED";
    } else if (a.verdict == "unknown") {
        bcol = amber, mark = "?", head = "CAN'T TELL";
    } else if (a.verdict == "lossy-format") {
        bcol = slate, mark = "\xe2\x9c\x97", head = "LOSSY FORMAT";
    } else {
        bcol = red, mark = "\xe2\x9c\x97", head = "NOT AS CLAIMED";
    }
    std::string sub = a.verdict + " \xc2\xb7 confidence " + std::to_string((int)lround(a.confidence * 100)) + "%";
    const int bw = std::min(std::max({ ttext_width(spek_font_title, head), ttext_width(spek_font_body, sub), 150 }) + 52,
                            W / 2),
              bh = HEADER - 20;
    const int bx = W - bw - 10, by = 10;
    fill(img, bx, by, bw, bh, bcol);
    ttext(img, spek_font_title, bx + 12, by + 8, mark, white);
    ttext(img, spek_font_title, bx + 38, by + 8, fit(spek_font_title, head, bw - 46), white);
    ttext(img, spek_font_body, bx + 38, by + 34, fit(spek_font_body, sub, bw - 46), white);

    // Artist - title, album.
    const int maxw = bx - 24;
    std::string who = !t.artist.empty() && !t.title.empty() ? t.artist + " \xe2\x80\x93 " + t.title
                      : !t.title.empty() ? t.title
                                         : name;
    who = fit(spek_font_title, who, maxw);
    int x = ttext(img, spek_font_title, 12, 8, who, white);
    if (!t.album.empty() && x + 40 < maxw) {
        ttext(img, spek_font_body, x + 12, 12, fit(spek_font_body, t.album, maxw - x - 12), grey);
    }

    // What the file claims to be.
    std::string claim = t.format.empty() ? "" : t.format + "  \xc2\xb7  ";
    if (a.bits) claim += std::to_string(a.bits) + "-bit / ";
    claim += khz(a.sample_rate);
    if (t.bit_rate > 0) claim += "  \xc2\xb7  " + thousands(t.bit_rate / 1000) + " kbps";
    claim += "  \xc2\xb7  " + std::to_string(a.channels) + " ch  \xc2\xb7  " +
             std::to_string((int)a.duration / 60) + ":" + (((int)a.duration % 60) < 10 ? "0" : "") +
             std::to_string((int)a.duration % 60);
    x = ttext(img, spek_font_body_bold, 12, 34, "CLAIMED", grey);
    ttext(img, spek_font_body, x + 10, 34, fit(spek_font_body, claim, maxw - x - 10), white);

    // A tick, cross or question mark for each claim.
    struct Check { int state; std::string text; };  // 1 yes, 0 no, -1 can't tell
    std::vector<Check> checks;
    if (!a.lossless) checks.push_back({ 0, "lossy codec" });
    const CutoffResult& c = a.cutoff;
    const std::string rate = khz(a.sample_rate);
    switch (c.verdict) {
    case CutoffVerdict::FULL_BAND:
        checks.push_back({ 1, hires_claim ? rate + " band is real" : "full band, no lossy cut" });
        break;
    case CutoffVerdict::LOSSY:
    case CutoffVerdict::LOSSY_UPSAMPLED:
        checks.push_back({ 0, "lossy cut at " + khz(c.cutoff_hz) });
        break;
    case CutoffVerdict::UPSAMPLED:
        checks.push_back({ 0, c.source_rate ? "upsampled from " + khz(c.source_rate)
                                            : "band stops at " + khz(c.cutoff_hz) });
        break;
    case CutoffVerdict::MIXED:
        checks.push_back({ 0, "lossy sections, cut at " + khz(c.cutoff_hz) });
        break;
    case CutoffVerdict::UNKNOWN:
        checks.push_back({ -1, "band: can't tell" });
        break;
    }
    if (a.bits) {
        const BitDepthResult& b = a.bitdepth;
        if (b.verdict == BitDepthVerdict::NATIVE) {
            checks.push_back({ 1, std::to_string(a.bits) + " bits are real" });
        } else if (b.verdict == BitDepthVerdict::UNKNOWN) {
            checks.push_back({ -1, "bit depth: can't tell" });
        } else {
            checks.push_back({ 0, "only " + std::to_string(b.effective_bits) + " bits used" });
        }
    }
    x = 12;
    for (const Check& ck : checks) {
        const uint32_t col = ck.state > 0 ? green : ck.state == 0 ? red : amber;
        const std::string m = ck.state > 0 ? "\xe2\x9c\x93" : ck.state == 0 ? "\xe2\x9c\x97" : "?";
        fill(img, x, 57, 18, 18, col);
        ttext(img, spek_font_body_bold, x + 9 - ttext_width(spek_font_body_bold, m) / 2, 57, m, white);
        x = ttext(img, spek_font_body, x + 25, 57, ck.text, white) + 22;
        if (x > maxw) break;
    }
}

bool spectrogram(const std::string& in, const std::string& out, int width, const FileAnalysis& a,
                 const Tags& tags, const std::string& name)
{
    Audio audio;
    FFT fft;
    Collector c;
    c.bands = (1 << (FFT_BITS - 1)) + 1;
    c.width = width;
    c.values.assign((size_t)width * c.bands, (float)LRANGE);
    spek_pipeline *p = spek_pipeline_open(audio.open(in, 0), fft.create(FFT_BITS), 0, 0,
                                          WINDOW_DEFAULT, width, collect, &c);
    spek_pipeline_start(p);
    {
        std::unique_lock<std::mutex> guard(c.lock);
        c.done_cv.wait(guard, [&]() { return c.done; });
    }
    spek_pipeline_close(p);

    const int h = c.bands;
    const int top = HEADER + TPAD;
    wxImage img(std::max(LPAD + width + RPAD, 640), top + h + BPAD);
    img.SetRGB(wxRect(0, 0, img.GetWidth(), img.GetHeight()), 0, 0, 0);
    const double range = URANGE - LRANGE;
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < h; ++y) {
            double v = std::min(URANGE, std::max(LRANGE, (double)c.values[(size_t)x * h + y]));
            put(img, LPAD + x, top + h - 1 - y, spek_palette(PALETTE_SOX, (v - LRANGE) / range));
        }
    }

    // Frequency axis.
    const uint32_t axis = 0xb0b0b0;
    const double nyquist = a.sample_rate / 2.0;
    const double steps[] = { 1000, 2000, 5000, 10000, 20000, 50000 };
    double step = steps[0];
    for (double s : steps) {
        step = s;
        if (nyquist / s <= 12) break;
    }
    for (double f = 0; f <= nyquist; f += step) {
        int y = top + h - 1 - (int)lround(f / nyquist * (h - 1));
        for (int x = LPAD - 5; x < LPAD; ++x) put(img, x, y, axis);
        std::string l = f == 0 ? "0" : freq_label(f);
        text(img, LPAD - 7 - text_width(l), y - 5, l, axis);
    }
    // Time axis.
    const double tsteps[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200 };
    double tstep = tsteps[0];
    for (double s : tsteps) {
        tstep = s;
        if (a.duration / s <= 10) break;
    }
    for (double t = 0; a.duration > 0 && t <= a.duration; t += tstep) {
        int x = LPAD + (int)lround(t / a.duration * (width - 1));
        for (int y = top + h; y < top + h + 5; ++y) put(img, x, y, axis);
        std::string l = time_label(t);
        text(img, std::min(x - text_width(l) / 2, img.GetWidth() - text_width(l) - 1), top + h + 8, l, axis);
    }

    // The evidence: the cutoff the verdict rests on, dashed, across the
    // lossy segments only when the file is mixed.
    // Thick dashes with a black outline, so they stand out on any part of the
    // palette, and the frequency written in the right margin beside a marker.
    auto ypos = [&](double hz) { return top + h - 1 - (int)lround(hz / nyquist * (h - 1)); };
    auto hline = [&](double hz, int x0, int x1, uint32_t rgb) {
        if (hz <= 0 || hz >= nyquist) return;
        const int y = ypos(hz);
        for (int x = x0; x < x1; ++x) {
            if (x % 16 >= 11) continue;  // 11 on, 5 off
            for (int dy = -2; dy <= 2; ++dy) {
                put(img, LPAD + x, y + dy, (dy == -2 || dy == 2) ? 0x000000 : rgb);
            }
        }
    };
    auto label = [&](double hz, uint32_t rgb) {
        if (hz <= 0 || hz >= nyquist) return;
        const int y = ypos(hz);
        const int x = LPAD + width + 2;
        for (int i = 0; i < 6; ++i) {           // a small arrowhead pointing at the line
            for (int dy = -(5 - i); dy <= 5 - i; ++dy) put(img, x + i, y + dy, rgb);
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fk", hz / 1000.0);
        text(img, x + 9, y - 5, buf, rgb);
    };
    const uint32_t whole = 0x00ffff, section = 0x80ff00;  // cyan, lime: off the palette
    const CutoffResult& cr = a.cutoff;
    if (cr.verdict == CutoffVerdict::MIXED && a.duration > 0) {
        double last = -1e9;
        for (const CutoffSegment& s : cr.lossy_segments) {
            int x0 = (int)(s.start / a.duration * width), x1 = (int)(s.end / a.duration * width);
            hline(s.cutoff_hz, x0, std::min(x1, width), section);
            if (fabs(s.cutoff_hz - last) > 400) {
                label(s.cutoff_hz, section);
                last = s.cutoff_hz;
            }
        }
    } else if (cr.verdict != CutoffVerdict::FULL_BAND && cr.verdict != CutoffVerdict::UNKNOWN) {
        hline(cr.cutoff_hz, 0, width, whole);
        label(cr.cutoff_hz, whole);
    }

    draw_header(img, a, tags, name);
    return img.SaveFile(wxString::FromUTF8(out.c_str()), wxBITMAP_TYPE_PNG);
}

// --- Reports --------------------------------------------------------------

std::string csv_field(const std::string& s)
{
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

std::string json_string(const std::string& s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if ((unsigned char)c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string num(double v, const char *fmt = "%g")
{
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, v);
    return buf;
}

std::string join(const std::vector<std::string>& v, const char *sep)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

struct Row
{
    Item item;
    bool ok;
    std::string error;
    FileAnalysis a;
    std::string png;
};

void write_csv(const std::string& path, const std::vector<Row>& rows)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "spek-cli: can't write %s\n", path.c_str());
        return;
    }
    fprintf(f, "file,verdict,confidence,codec,sample_rate,bits,channels,duration,cutoff_verdict,cutoff_hz,"
               "cutoff_consistency,source_rate,codec_hint,lossy_segments,bitdepth_verdict,effective_bits,"
               "evidence,png\r\n");
    for (const Row& r : rows) {
        if (!r.ok) {
            fprintf(f, "%s,error,,,,,,,,,,,,,,,%s,\r\n", csv_field(r.item.rel).c_str(), csv_field(r.error).c_str());
            continue;
        }
        const FileAnalysis& a = r.a;
        std::vector<std::string> segs;
        for (const CutoffSegment& s : a.cutoff.lossy_segments) {
            segs.push_back(num(s.start, "%.1f") + "-" + num(s.end, "%.1f") + "s@" + num(s.cutoff_hz, "%.0f"));
        }
        std::vector<std::string> fields = {
            r.item.rel, a.verdict, num(a.confidence, "%.2f"), a.codec, num(a.sample_rate),
            a.bits ? num(a.bits) : "", num(a.channels), num(a.duration, "%.2f"),
            cutoff_verdict_name(a.cutoff.verdict), a.cutoff.cutoff_hz > 0 ? num(a.cutoff.cutoff_hz, "%.0f") : "",
            num(a.cutoff.consistency, "%.2f"), a.cutoff.source_rate ? num(a.cutoff.source_rate) : "",
            a.cutoff.codec_hint, join(segs, " "), bitdepth_verdict_name(a.bitdepth.verdict),
            a.bitdepth.effective_bits ? num(a.bitdepth.effective_bits) : "", join(a.evidence, "; "), r.png,
        };
        for (size_t i = 0; i < fields.size(); ++i) {
            fprintf(f, "%s%s", i ? "," : "", csv_field(fields[i]).c_str());
        }
        fprintf(f, "\r\n");
    }
    fclose(f);
}

void write_json(const std::string& path, const std::vector<Row>& rows)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "spek-cli: can't write %s\n", path.c_str());
        return;
    }
    fprintf(f, "{\"files\": [\n");
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        fprintf(f, "  {\"file\": %s, ", json_string(r.item.rel).c_str());
        if (!r.ok) {
            fprintf(f, "\"verdict\": \"error\", \"error\": %s}", json_string(r.error).c_str());
        } else {
            const FileAnalysis& a = r.a;
            const CutoffResult& c = a.cutoff;
            const BitDepthResult& b = a.bitdepth;
            std::vector<std::string> ev, segs;
            for (const std::string& e : a.evidence) ev.push_back(json_string(e));
            for (const CutoffSegment& s : c.lossy_segments) {
                segs.push_back("{\"start\": " + num(s.start, "%.2f") + ", \"end\": " + num(s.end, "%.2f") +
                               ", \"cutoff_hz\": " + num(s.cutoff_hz, "%.0f") + "}");
            }
            fprintf(f,
                    "\"verdict\": %s, \"confidence\": %.3f, \"codec\": %s, \"lossless\": %s, "
                    "\"sample_rate\": %d, \"bits\": %d, \"channels\": %d, \"duration\": %.3f,\n"
                    "   \"cutoff\": {\"verdict\": \"%s\", \"confidence\": %.3f, \"cutoff_hz\": %.0f, "
                    "\"consistency\": %.3f, \"drop_db\": %.1f, \"source_rate\": %d, \"codec_hint\": %s, "
                    "\"basis\": %s, \"frames\": %d, \"active_frames\": %d, \"lossy_segments\": [%s]},\n"
                    "   \"bitdepth\": {\"verdict\": \"%s\", \"confidence\": %.3f, \"container_bits\": %d, "
                    "\"effective_bits\": %d, \"zero_low_bits\": %d, \"comb_period\": %.1f, \"comb_db\": %.1f},\n"
                    "   \"evidence\": [%s], \"png\": %s}",
                    json_string(a.verdict).c_str(), a.confidence, json_string(a.codec).c_str(),
                    a.lossless ? "true" : "false", a.sample_rate, a.bits, a.channels, a.duration,
                    cutoff_verdict_name(c.verdict), c.confidence, c.cutoff_hz, c.consistency, c.drop_db,
                    c.source_rate, json_string(c.codec_hint).c_str(), json_string(c.basis).c_str(), c.frames,
                    c.active_frames, join(segs, ", ").c_str(), bitdepth_verdict_name(b.verdict), b.confidence,
                    b.container_bits, b.effective_bits, b.zero_low_bits, b.comb_period,
                    b.container_bits == 24 ? b.comb_db : 0.0, join(ev, ", ").c_str(),
                    r.png.empty() ? "null" : json_string(r.png).c_str());
        }
        fprintf(f, "%s\n", i + 1 < rows.size() ? "," : "");
    }
    fprintf(f, "]}\n");
    fclose(f);
}

std::string error_text(AudioError e)
{
    switch (e) {
    case AudioError::CANNOT_OPEN_FILE: return "cannot open file";
    case AudioError::NO_STREAMS: return "no streams";
    case AudioError::NO_AUDIO: return "no audio stream";
    case AudioError::NO_DECODER: return "no decoder for this format";
    case AudioError::NO_DURATION: return "unknown duration";
    case AudioError::NO_CHANNELS: return "no audio channels";
    case AudioError::CANNOT_OPEN_DECODER: return "cannot open decoder";
    case AudioError::BAD_SAMPLE_FORMAT: return "unsupported sample format";
    case AudioError::OK: break;
    }
    return "error";
}

} // namespace

int main(int argc, char **argv)
{
    std::string out_dir = "spek-report";
    bool png = true, quiet = false;
    int width = 1024;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            fputs(USAGE, stdout);
            return 0;
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--no-png") {
            png = false;
        } else if (arg == "--width" && i + 1 < argc) {
            width = atoi(argv[++i]);
            if (width < 64 || width > 8192) {
                fprintf(stderr, "spek-cli: --width must be 64 to 8192\n");
                return 2;
            }
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (!arg.empty() && arg[0] == '-') {
            fprintf(stderr, "spek-cli: unknown option %s\n\n%s", arg.c_str(), USAGE);
            return 2;
        } else {
            paths.push_back(arg);
        }
    }
    if (paths.empty()) {
        fputs(USAGE, stderr);
        return 2;
    }

    // FFmpeg's warnings (timestamps, probe scores) aren't findings.
    av_log_set_level(AV_LOG_ERROR);

    wxInitializer wx;
    if (!wx.IsOk()) {
        fprintf(stderr, "spek-cli: can't initialise wxWidgets\n");
        return 1;
    }
    wxImage::AddHandler(new wxPNGHandler);

    std::vector<Item> items;
    for (const std::string& p : paths) {
        if (is_dir(p)) {
            scan(p, "", items);
        } else {
            size_t slash = p.find_last_of("/\\");
            items.push_back({ p, slash == std::string::npos ? p : p.substr(slash + 1) });
        }
    }
    if (items.empty()) {
        fprintf(stderr, "spek-cli: no audio files found\n");
        return 1;
    }
    if (!make_dirs(out_dir)) {
        fprintf(stderr, "spek-cli: can't create %s\n", out_dir.c_str());
        return 1;
    }

    Audio audio;
    std::vector<Row> rows;
    int errors = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        Row r;
        r.item = items[i];
        auto file = audio.open(r.item.path, 0);
        r.ok = !file->get_error();
        if (!r.ok) {
            r.error = error_text(file->get_error());
            ++errors;
        } else {
            r.a = analyse_file(*file);
            if (png) {
                std::string rel_png = "png/" + r.item.rel + ".png";
                std::string full = out_dir + "/" + rel_png;
                make_dirs(full.substr(0, full.find_last_of('/')));
                if (spectrogram(r.item.path, full, width, r.a, read_tags(r.item.path), r.item.rel)) {
                    r.png = rel_png;
                } else {
                    fprintf(stderr, "spek-cli: can't write %s\n", full.c_str());
                }
            }
        }
        if (!quiet) {
            fprintf(stderr, "[%zu/%zu] %s: %s\n", i + 1, items.size(), r.item.rel.c_str(),
                    r.ok ? (r.a.verdict + " (" + num(r.a.confidence, "%.2f") + ")").c_str() : r.error.c_str());
        }
        rows.push_back(r);
    }

    write_csv(out_dir + "/report.csv", rows);
    write_json(out_dir + "/report.json", rows);
    if (!quiet) {
        fprintf(stderr, "%zu files, %d unreadable. Report in %s/report.csv and report.json\n", items.size(),
                errors, out_dir.c_str());
    }
    return 0;
}
