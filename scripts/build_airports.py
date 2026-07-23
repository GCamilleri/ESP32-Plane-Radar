#!/usr/bin/env python3
"""Build runway datasets from OurAirports (large + medium + small airports)."""

from __future__ import annotations

import csv
import io
import math
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_H = ROOT / "include" / "data" / "airports.h"
OUT_CPP = ROOT / "src" / "data" / "airports_data.cpp"

AIRPORTS_URL = (
    "https://raw.githubusercontent.com/davidmegginson/ourairports-data/main/"
    "airports.csv"
)
RUNWAYS_URL = (
    "https://raw.githubusercontent.com/davidmegginson/ourairports-data/main/"
    "runways.csv"
)


def fetch_csv(url: str) -> list[dict[str, str]]:
    with urllib.request.urlopen(url, timeout=60) as resp:
        text = resp.read().decode("utf-8")
    return list(csv.DictReader(io.StringIO(text)))


def coord_e7(s: str | None) -> int | None:
    if not s or not s.strip():
        return None
    return int(round(float(s) * 1e7))


def is_h_designator(s: str) -> bool:
    if not s or s[0] != "H":
        return False
    rest = s[1:]
    if not rest:
        return True
    if rest[0] in "-_":
        return True
    return rest.isdigit()


def is_helipad(row: dict[str, str]) -> bool:
    le = (row.get("le_ident") or "").strip().upper()
    he = (row.get("he_ident") or "").strip().upper()
    if not is_h_designator(le) and not is_h_designator(he):
        return False
    try:
        length_ft = int(row.get("length_ft") or 0)
    except ValueError:
        length_ft = 0
    if is_h_designator(le) and is_h_designator(he):
        return True
    return length_ft < 2500


def heading_from_ident(ident: str) -> float | None:
    """Derive magnetic heading from runway designator (e.g. '03' -> 30.0)."""
    s = (ident or "").strip().upper().rstrip("LRC")
    if not s or not s.isdigit():
        return None
    deg = int(s) * 10
    if deg == 0:
        deg = 360
    if deg < 1 or deg > 360:
        return None
    return float(deg)


def synthesize_endpoints(
    center_lat_e7: int, center_lon_e7: int, length_ft: int, heading_deg: float
) -> tuple[int, int, int, int]:
    """Project runway endpoints from center + length + heading."""
    half_m = length_ft * 0.3048 / 2.0
    lat_rad = center_lat_e7 * 1e-7 * math.pi / 180.0
    hdg_rad = heading_deg * math.pi / 180.0
    m_per_deg = 111_000.0
    dlat_deg = (half_m * math.cos(hdg_rad)) / m_per_deg
    dlon_deg = (half_m * math.sin(hdg_rad)) / (m_per_deg * math.cos(lat_rad))
    dlat_e7 = int(round(dlat_deg * 1e7))
    dlon_e7 = int(round(dlon_deg * 1e7))
    le_lat = center_lat_e7 - dlat_e7
    le_lon = center_lon_e7 - dlon_e7
    he_lat = center_lat_e7 + dlat_e7
    he_lon = center_lon_e7 + dlon_e7
    return le_lat, le_lon, he_lat, he_lon


def build_tier(
    airport_type: str,
    all_airports: list[dict[str, str]],
    all_runways: list[dict[str, str]],
) -> tuple[list[tuple[str, int, int]], list[tuple[int, int, int, int, int, int]]]:
    idents: dict[str, tuple[int, int]] = {}
    for a in all_airports:
        if a.get("type") != airport_type:
            continue
        ident = (a.get("ident") or "").strip()
        if len(ident) != 4:
            continue
        lat = coord_e7(a.get("latitude_deg"))
        lon = coord_e7(a.get("longitude_deg"))
        if lat is None or lon is None:
            continue
        idents[ident] = (lat, lon)

    airport_rows = sorted(
        (ident, lat, lon) for ident, (lat, lon) in idents.items()
    )
    airport_index = {ident: idx for idx, (ident, _, _) in enumerate(airport_rows)}

    segments: list[tuple[int, int, int, int, int, int]] = []
    for r in all_runways:
        if r.get("closed") == "1":
            continue
        airport = (r.get("airport_ident") or "").strip()
        if airport not in airport_index:
            continue
        if is_helipad(r):
            continue
        try:
            length_ft = int(r.get("length_ft") or 0)
        except ValueError:
            continue
        if length_ft <= 0:
            continue
        le_lat = coord_e7(r.get("le_latitude_deg"))
        le_lon = coord_e7(r.get("le_longitude_deg"))
        he_lat = coord_e7(r.get("he_latitude_deg"))
        he_lon = coord_e7(r.get("he_longitude_deg"))
        if None in (le_lat, le_lon, he_lat, he_lon):
            # Synthesize endpoints from airport center + ident heading + length.
            hdg = heading_from_ident(r.get("le_ident"))
            if hdg is None:
                continue
            center_lat, center_lon = idents[airport]
            le_lat, le_lon, he_lat, he_lon = synthesize_endpoints(
                center_lat, center_lon, length_ft, hdg
            )
        length_m = int(round(length_ft * 0.3048))
        segments.append(
            (
                airport_index[airport],
                le_lat,
                le_lon,
                he_lat,
                he_lon,
                length_m,
            )
        )

    segments.sort(key=lambda row: (row[0], -row[5]))
    return airport_rows, segments


def render_tier_header(
    tier_name: str, airport_count: int, runway_count: int
) -> list[str]:
    return [
        f"namespace {tier_name} {{",
        f"constexpr size_t kAirportCount = {airport_count};",
        f"constexpr size_t kRunwayCount = {runway_count};",
        "extern const Airport kAirports[];",
        "extern const Runway kRunways[];",
        f"}}  // namespace {tier_name}",
        "",
    ]


def render_header(tier_counts: list[tuple[str, int, int]]) -> str:
    lines = [
        "// Generated by scripts/build_airports.py -- do not edit.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace data::airports {",
        "",
        "struct Airport {",
        "  char ident[5];",
        "  int32_t lat_e7;",
        "  int32_t lon_e7;",
        "};",
        "",
        "struct Runway {",
        "  uint16_t airport_idx;",
        "  int32_t le_lat_e7;",
        "  int32_t le_lon_e7;",
        "  int32_t he_lat_e7;",
        "  int32_t he_lon_e7;",
        "  uint16_t length_m;",
        "};",
        "",
    ]
    for tier_name, airport_count, runway_count in tier_counts:
        lines += render_tier_header(tier_name, airport_count, runway_count)
    lines += [
        "}  // namespace data::airports",
        "",
    ]
    return "\n".join(lines)


def render_tier_cpp(
    tier_name: str,
    airport_rows: list[tuple[str, int, int]],
    segments: list[tuple[int, int, int, int, int, int]],
) -> list[str]:
    lines = [
        f"namespace {tier_name} {{",
        "",
        "const Airport kAirports[] = {",
    ]
    for ident, lat, lon in airport_rows:
        lines.append(f'  {{"{ident}", {lat}, {lon}}},')
    lines += [
        "};",
        "",
        "const Runway kRunways[] = {",
    ]
    for airport_idx, le_lat, le_lon, he_lat, he_lon, length_m in segments:
        lines.append(
            f"  {{{airport_idx}, {le_lat}, {le_lon}, {he_lat}, {he_lon}, {length_m}}},"
        )
    lines += [
        "};",
        "",
        f"}}  // namespace {tier_name}",
    ]
    return lines


def render_cpp(
    tiers: list[
        tuple[
            str,
            list[tuple[str, int, int]],
            list[tuple[int, int, int, int, int, int]],
        ]
    ],
) -> str:
    lines = [
        "// Generated by scripts/build_airports.py -- do not edit.",
        '#include "data/airports.h"',
        "",
        "namespace data::airports {",
        "",
    ]
    for i, (tier_name, airports, runways) in enumerate(tiers):
        if i > 0:
            lines += [""]
        lines += render_tier_cpp(tier_name, airports, runways)
    lines += [
        "",
        "}  // namespace data::airports",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    print("Fetching airport data from OurAirports...")
    all_airports = fetch_csv(AIRPORTS_URL)
    all_runways = fetch_csv(RUNWAYS_URL)

    print("Building large airport tier...")
    large_ap, large_rw = build_tier("large_airport", all_airports, all_runways)
    print(f"  large: {len(large_ap)} airports, {len(large_rw)} runways")

    print("Building medium airport tier...")
    medium_ap, medium_rw = build_tier("medium_airport", all_airports, all_runways)
    print(f"  medium: {len(medium_ap)} airports, {len(medium_rw)} runways")

    print("Building small airport tier...")
    small_ap, small_rw = build_tier("small_airport", all_airports, all_runways)
    print(f"  small: {len(small_ap)} airports, {len(small_rw)} runways")

    # airport_idx is stored as uint16_t, so a tier must not exceed 65535 airports.
    for name, ap in (("large", large_ap), ("medium", medium_ap), ("small", small_ap)):
        if len(ap) > 0xFFFF:
            raise SystemExit(
                f"{name} tier has {len(ap)} airports, exceeds uint16_t index range"
            )

    header = render_header(
        [
            ("large", len(large_ap), len(large_rw)),
            ("medium", len(medium_ap), len(medium_rw)),
            ("small", len(small_ap), len(small_rw)),
        ]
    )
    cpp = render_cpp(
        [
            ("large", large_ap, large_rw),
            ("medium", medium_ap, medium_rw),
            ("small", small_ap, small_rw),
        ]
    )

    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_CPP.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.write_text(header, encoding="utf-8")
    OUT_CPP.write_text(cpp, encoding="utf-8")

    cpp_size_kb = len(cpp.encode("utf-8")) / 1024
    print(
        f"Wrote {OUT_H.name} + {OUT_CPP.name} "
        f"(cpp size: {cpp_size_kb:.0f} KB)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
