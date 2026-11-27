#!/usr/bin/env python3
"""Score Spek's detectors against a generated corpus.

Runs the development driver (tests/spek-analyze) on every file in the
manifest and compares each detector's verdict with the ground-truth label.

    tests/corpus/score.py tests/corpus/out [--driver tests/spek-analyze] [--report scores.csv]
                          [--max-false 0] [--min-correct N] [--min-bitdepth-correct N]

Each file is scored as:
  correct  the detector gave the expected verdict
  partial  it flagged a fake, but as the wrong kind (e.g. lossy+upsampled
           reported as upsampled)
  missed   it called a fake clean
  false    it flagged a genuine file
  wrong    anything else
"""

import argparse
import collections
import csv
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# What the cutoff detector should say for each label. Bit-depth fakes have
# genuine bandwidth, so the cutoff detector must leave them alone.
CUTOFF_EXPECTED = {
    "genuine": "full-band",
    "padded-bits": "full-band",
    "lossy": "lossy",
    "upsampled": "upsampled",
    "lossy+upsampled": "lossy+upsampled",
    "mixed": "mixed",
}
CLEAN = {"full-band"}

# The bit-depth detector should flag exactly the padded-bits files, with the
# right source depth. Everything else (lossy decodes and resampled files
# included) fills its container's bits for real.
BITDEPTH_FLAGS = {"padded", "requantised"}


def score(expected_label, got):
    want = CUTOFF_EXPECTED[expected_label]
    if got == want:
        return "correct"
    if want in CLEAN:
        return "false" if got not in CLEAN | {"unknown"} else "wrong"
    if got in CLEAN:
        return "missed"
    if got == "unknown":
        return "wrong"
    return "partial"


def score_bitdepth(row, b):
    got = b.get("verdict", "error")
    if row["expected_verdict"] != "padded-bits":
        if got in BITDEPTH_FLAGS:
            return "false"
        return "correct" if got in ("native", "unknown") else "wrong"
    if got not in BITDEPTH_FLAGS:
        return "missed"
    return "correct" if b.get("effective_bits") == row["true_bits"] else "partial"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("corpus")
    ap.add_argument("--driver", default=os.path.join(HERE, "..", "spek-analyze"))
    ap.add_argument("--report", help="write per-file scores to this CSV")
    ap.add_argument("--max-false", type=int, help="fail if any detector has more false positives than this")
    ap.add_argument("--min-correct", type=int, help="fail if the cutoff detector gets fewer files right")
    ap.add_argument("--min-bitdepth-correct", type=int, help="fail if the bit-depth detector gets fewer files right")
    args = ap.parse_args()

    with open(os.path.join(args.corpus, "manifest.json")) as f:
        rows = json.load(f)["files"]
    paths = [os.path.join(args.corpus, r["path"]) for r in rows]
    out = subprocess.run([args.driver] + paths, stdout=subprocess.PIPE, check=False)
    results = [json.loads(line) for line in out.stdout.decode().splitlines() if line.strip()]
    if len(results) != len(rows):
        raise SystemExit("driver returned %d results for %d files" % (len(results), len(rows)))

    table = []
    counts = {"cutoff": collections.Counter(), "bitdepth": collections.Counter()}
    by_label = collections.defaultdict(collections.Counter)
    for row, res in zip(rows, results):
        c = res.get("cutoff", {})
        got = c.get("verdict", "error")
        s = score(row["expected_verdict"], got)
        counts["cutoff"][s] += 1
        by_label[row["expected_verdict"]][s] += 1
        evidence = "cutoff %s Hz, consistency %.2f, confidence %.2f" % (
            c.get("cutoff_hz", 0), c.get("consistency", 0), c.get("confidence", 0))
        if c.get("basis") == "average":
            evidence += ", whole-file average only"
        if c.get("source_rate"):
            evidence += ", source %d Hz" % c["source_rate"]
        if c.get("lossy_segments"):
            evidence += ", segments " + " ".join(
                "%.1f-%.1fs" % (g["start"], g["end"]) for g in c["lossy_segments"])

        b = res.get("bitdepth", {})
        bgot = b.get("verdict", "error")
        bs = score_bitdepth(row, b)
        counts["bitdepth"][bs] += 1
        bevidence = "%d-bit container" % b.get("container_bits", 0)
        if bgot in BITDEPTH_FLAGS:
            bevidence += ", %d effective bits" % b.get("effective_bits", 0)
        if b.get("zero_low_bits"):
            bevidence += ", %d zero low bits" % b["zero_low_bits"]
        if b.get("comb_period"):
            bevidence += ", lattice step %.1f" % b["comb_period"]
        if b.get("container_bits") == 24 and bgot != "padded":
            bevidence += ", comb %.1f dB" % b.get("comb_db", 0)

        table.append({"id": row["id"], "expected": row["expected_verdict"],
                      "difficulty": row["difficulty"], "cutoff": got, "cutoff_score": s,
                      "cutoff_evidence": evidence, "bitdepth": bgot, "bitdepth_score": bs,
                      "bitdepth_evidence": bevidence})
        print("%-8s %-8s %-50s %-16s -> %-16s %-12s %s; %s" % (
            s, bs, row["id"], row["expected_verdict"], got, bgot, evidence, bevidence))

    def summary(c, n):
        return "%d correct, %d partial, %d missed, %d false positives, %d wrong, of %d files" % (
            c["correct"], c["partial"], c["missed"], c["false"], c["wrong"], n)

    print("\nBy label (cutoff detector):")
    for label in CUTOFF_EXPECTED:
        if label in by_label:
            b = by_label[label]
            print("  %-16s %s" % (label, ", ".join("%s %d" % (k, b[k]) for k in
                                                    ("correct", "partial", "missed", "false", "wrong") if b[k])))
    print("\nCutoff:    " + summary(counts["cutoff"], len(rows)))
    print("Bit depth: " + summary(counts["bitdepth"], len(rows)))

    if args.report:
        with open(args.report, "w", newline="") as f:
            w = csv.DictWriter(f, list(table[0].keys()))
            w.writeheader()
            w.writerows(table)

    failed = []
    for name, c in counts.items():
        if args.max_false is not None and c["false"] > args.max_false:
            failed.append("%s: %d false positives (allowed %d)" % (name, c["false"], args.max_false))
    for name, need in (("cutoff", args.min_correct), ("bitdepth", args.min_bitdepth_correct)):
        if need is not None and counts[name]["correct"] < need:
            failed.append("%s: %d correct (needed %d)" % (name, counts[name]["correct"], need))
    if failed:
        raise SystemExit("score check failed: " + "; ".join(failed))


if __name__ == "__main__":
    main()
