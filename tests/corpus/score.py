#!/usr/bin/env python3
"""Score Spek's detectors against a generated corpus.

Runs the development driver (tests/spek-analyze) on every file in the
manifest and compares its verdict with the ground-truth label.

    tests/corpus/score.py tests/corpus/out [--driver tests/spek-analyze] [--report scores.csv]
                          [--max-false 0] [--min-correct N]

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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("corpus")
    ap.add_argument("--driver", default=os.path.join(HERE, "..", "spek-analyze"))
    ap.add_argument("--report", help="write per-file scores to this CSV")
    ap.add_argument("--max-false", type=int, help="fail if there are more false positives than this")
    ap.add_argument("--min-correct", type=int, help="fail if fewer files than this are correct")
    args = ap.parse_args()

    with open(os.path.join(args.corpus, "manifest.json")) as f:
        rows = json.load(f)["files"]
    paths = [os.path.join(args.corpus, r["path"]) for r in rows]
    out = subprocess.run([args.driver] + paths, stdout=subprocess.PIPE, check=False)
    results = [json.loads(line) for line in out.stdout.decode().splitlines() if line.strip()]
    if len(results) != len(rows):
        raise SystemExit("driver returned %d results for %d files" % (len(results), len(rows)))

    table, counts = [], collections.Counter()
    by_label = collections.defaultdict(collections.Counter)
    for row, res in zip(rows, results):
        c = res.get("cutoff", {})
        got = c.get("verdict", "error")
        s = score(row["expected_verdict"], got)
        counts[s] += 1
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
        table.append({"id": row["id"], "expected": row["expected_verdict"],
                      "difficulty": row["difficulty"], "got": got, "score": s,
                      "evidence": evidence})
        print("%-8s %-50s %-16s -> %-16s %s" % (s, row["id"], row["expected_verdict"], got, evidence))

    print("\nBy label (cutoff detector):")
    for label in CUTOFF_EXPECTED:
        if label in by_label:
            b = by_label[label]
            print("  %-16s %s" % (label, ", ".join("%s %d" % (k, b[k]) for k in
                                                    ("correct", "partial", "missed", "false", "wrong") if b[k])))
    print("\n%d correct, %d partial, %d missed, %d false positives, %d wrong, of %d files" % (
        counts["correct"], counts["partial"], counts["missed"], counts["false"], counts["wrong"], len(rows)))

    if args.report:
        with open(args.report, "w", newline="") as f:
            w = csv.DictWriter(f, ["id", "expected", "difficulty", "got", "score", "evidence"])
            w.writeheader()
            w.writerows(table)

    failed = []
    if args.max_false is not None and counts["false"] > args.max_false:
        failed.append("%d false positives (allowed %d)" % (counts["false"], args.max_false))
    if args.min_correct is not None and counts["correct"] < args.min_correct:
        failed.append("%d correct (needed %d)" % (counts["correct"], args.min_correct))
    if failed:
        raise SystemExit("score check failed: " + "; ".join(failed))


if __name__ == "__main__":
    main()
