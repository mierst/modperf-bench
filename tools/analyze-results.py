#!/usr/bin/env python3
"""Analyze captured evidence offline; never launch a server or network call."""
import argparse
import json
from pathlib import Path
from calibration import analyze_files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results")
    parser.add_argument("--manifest")
    parser.add_argument("--reference")
    parser.add_argument("--reference-manifest")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.reference_manifest and not args.reference:
        parser.error("--reference-manifest requires --reference")
    destination = Path(args.output).resolve()
    sources = [args.results, args.manifest, args.reference, args.reference_manifest]
    if any(destination == Path(source).resolve() for source in sources if source):
        parser.error("--output must differ from every source artifact")
    report = analyze_files(args.results, args.manifest, args.reference, args.reference_manifest)
    try:
        Path(args.output).write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    except OSError as exc:
        parser.exit(2, "cannot write analysis: " + str(exc) + "\n")
    return 2 if report["status"] == "invalid" else 0


if __name__ == "__main__":
    raise SystemExit(main())
