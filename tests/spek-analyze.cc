// Development driver for the analysis engine: prints one JSON object per file
// with each detector's result.
// Used by tests/corpus/score.py to score the detectors against the corpus.
// (The user-facing batch tool is Phase 3.)

#include <cstdio>
#include <cstring>
#include <string>

#include "spek-analysis.h"
#include "spek-audio.h"

static std::string json_string(const std::string& s)
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

int main(int argc, char **argv)
{
    bool frames = false;
    Audio audio;
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames")) {
            frames = true;
            continue;
        }
        auto file = audio.open(argv[i], 0);
        if (!!file->get_error()) {
            printf("{\"file\": %s, \"error\": %d}\n", json_string(argv[i]).c_str(),
                   static_cast<int>(file->get_error()));
            status = 1;
            continue;
        }
        CutoffResult r = analyse_cutoff(*file);
        printf("{\"file\": %s, \"sample_rate\": %d, \"bits\": %d, \"cutoff\": {"
               "\"verdict\": \"%s\", \"confidence\": %.3f, \"cutoff_hz\": %.0f, "
               "\"consistency\": %.3f, \"drop_db\": %.1f, \"source_rate\": %d, "
               "\"codec_hint\": %s, \"basis\": \"%s\", \"frames\": %d, \"active_frames\": %d, "
               "\"shelf_frames\": %d, \"lossy_segments\": [",
               json_string(argv[i]).c_str(), r.sample_rate, file->get_bits_per_sample(),
               cutoff_verdict_name(r.verdict), r.confidence, r.cutoff_hz, r.consistency,
               r.drop_db, r.source_rate, json_string(r.codec_hint).c_str(), r.basis.c_str(), r.frames,
               r.active_frames, r.shelf_frames);
        for (size_t s = 0; s < r.lossy_segments.size(); ++s) {
            const CutoffSegment& seg = r.lossy_segments[s];
            printf("%s{\"start\": %.2f, \"end\": %.2f, \"cutoff_hz\": %.0f}", s ? ", " : "",
                   seg.start, seg.end, seg.cutoff_hz);
        }
        printf("]");
        if (frames) {
            printf(", \"frame_log\": [");
            for (size_t f = 0; f < r.frame_log.size(); ++f) {
                const CutoffFrame& fr = r.frame_log[f];
                printf("%s[%.3f, %d, %.0f, %.1f]", f ? ", " : "", fr.time, fr.active ? 1 : 0,
                       fr.cutoff_hz, fr.drop_db);
            }
            printf("]");
        }
        printf("}");
        auto file2 = audio.open(argv[i], 0);
        BitDepthResult b = analyse_bitdepth(*file2);
        printf(", \"bitdepth\": {\"verdict\": \"%s\", \"container_bits\": %d, \"effective_bits\": %d, "
               "\"zero_low_bits\": %d, \"comb_period\": %.1f, \"comb_db\": %.1f, \"samples\": %lld}",
               bitdepth_verdict_name(b.verdict), b.container_bits, b.effective_bits, b.zero_low_bits,
               b.comb_period, b.comb_db, b.samples);
        printf("}\n");
    }
    return status;
}
