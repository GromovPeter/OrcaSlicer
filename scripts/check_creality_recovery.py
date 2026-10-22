#!/usr/bin/env python3
"""Check the pinned Creality snapshot and unchanged recovery implementation.

Offline only. Direct includes, include guards, comments and whitespace are
excluded from the code comparison; runtime logic must still match upstream.
"""
from pathlib import Path
import hashlib
import json
import re

CPP_PARTS = re.compile(r'"(?:\\.|[^"\\])*"|\x27(?:\\.|[^\x27\\])*\x27|//[^\n]*|/\*.*?\*/|\s+|[^\s]', re.S)


def code(path):
    text = path.read_text(encoding="utf-8-sig")
    text = "".join(part for part in CPP_PARTS.findall(text)
                   if not part.startswith(("//", "/*")))
    text = re.sub(r"^\s*#\s*(?:include\b[^\n]*|pragma\s+once\s*)$", "", text, flags=re.M)
    guard = re.search(r"^\s*#ifndef\s+(slic3r_GCode_\w+_hpp_)\s*\n\s*#define\s+\1\s*$", text, re.M)
    if guard:
        text = text[:guard.start()] + text[guard.end():]
        text = re.sub(r"#endif\s*\Z", "", text)
    return text


def normalized(text):
    return "".join(part for part in CPP_PARTS.findall(text) if not part.isspace())


def section(text, name):
    start = text.index(name)
    first = text.index("{", start)
    depth = 0
    for i in range(first, len(text)):
        depth += (text[i] == "{") - (text[i] == "}")
        if depth == 0:
            return text[start:i + 1]
    raise ValueError("Unterminated reference section: " + name)


def main():
    root = Path(__file__).resolve().parents[1]
    reference = root / "third_party/crealityprint-recovery"
    production = root / "src/libslic3r/GCode"
    provenance = json.loads((reference / "provenance.json").read_text(encoding="utf-8"))
    for name, expected in provenance["source_files_sha256"].items():
        actual = hashlib.sha256((reference / name).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError("Immutable reference hash mismatch: " + name)
    actual_license = hashlib.sha256((reference / "LICENSE.txt").read_bytes()).hexdigest()
    if actual_license != provenance["license_sha256"]:
        raise ValueError("Immutable reference license hash mismatch")
    for name in ("AppearanceUnderExtrusionAccelRecoveryFilter.cpp",
                 "AppearanceUnderExtrusionAccelRecoveryFilter.hpp"):
        if normalized(code(reference / name)) != normalized(code(production / name)):
            raise ValueError("Recovery implementation differs from upstream: " + name)
    original = code(reference / "InterestRegion.hpp")
    current = code(production / "InterestRegion.hpp")
    for name in ("class MotionSegments", "struct AppearanceUnderExtrusionDefinition",
                 "detect_continuous_slow_spans_impl", "compute_aue_L_safe_accel_mm",
                 "compute_aue_L_safe_total_mm",
                 "detect_appearance_under_extrusion_interest_region_impl"):
        if normalized(section(original, name)) != normalized(section(current, name)):
            raise ValueError("Recovery detection differs from upstream: " + name)
    print("Creality reference hashes and recovery code equivalence passed.")


if __name__ == "__main__":
    main()
