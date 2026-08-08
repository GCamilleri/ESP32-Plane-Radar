"""Unit tests for the pure logic in scripts/build_airports.py.

Run: python3 -m pytest scripts/test_build_airports.py
These cover the data-generation helpers only; they do no network I/O.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location(
    "build_airports", ROOT / "scripts" / "build_airports.py"
)
ba = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ba)


def test_coord_e7():
    assert ba.coord_e7("52.3676") == 523676000
    assert ba.coord_e7("-0.5") == -5000000
    assert ba.coord_e7("0") == 0
    assert ba.coord_e7("") is None
    assert ba.coord_e7("   ") is None
    assert ba.coord_e7(None) is None


def test_is_h_designator():
    assert ba.is_h_designator("H1")
    assert ba.is_h_designator("H")
    assert ba.is_h_designator("H-2")
    assert not ba.is_h_designator("18")
    assert not ba.is_h_designator("HELO")  # rest is neither empty, -/_ , nor digits
    assert not ba.is_h_designator("")


def test_heading_from_ident():
    assert ba.heading_from_ident("03") == 30.0
    assert ba.heading_from_ident("36") == 360.0
    assert ba.heading_from_ident("18L") == 180.0
    assert ba.heading_from_ident("00") == 360.0  # 0 wraps to 360
    assert ba.heading_from_ident("") is None
    assert ba.heading_from_ident("XX") is None
    assert ba.heading_from_ident("99") is None  # 990 deg is out of range


def test_is_helipad():
    # Both ends are helipad designators -> helipad regardless of length.
    assert ba.is_helipad({"le_ident": "H1", "he_ident": "H1", "length_ft": "50"})
    # One H end and short -> helipad.
    assert ba.is_helipad({"le_ident": "H1", "he_ident": "", "length_ft": "1000"})
    # One H end but long -> treated as a real runway, not a helipad.
    assert not ba.is_helipad({"le_ident": "H1", "he_ident": "", "length_ft": "5000"})
    # Ordinary runway.
    assert not ba.is_helipad({"le_ident": "18", "he_ident": "36", "length_ft": "8000"})


def test_synthesize_endpoints_symmetry():
    clat, clon = 523676000, 49041000
    le_lat, le_lon, he_lat, he_lon = ba.synthesize_endpoints(clat, clon, 3000, 45.0)
    # Endpoints are mirror images of the center.
    assert he_lat - clat == clat - le_lat
    assert he_lon - clon == clon - le_lon
    # Both offsets non-trivial at 45 degrees.
    assert he_lat != clat and he_lon != clon


def test_synthesize_endpoints_due_north():
    clat, clon = 523676000, 49041000
    le_lat, le_lon, he_lat, he_lon = ba.synthesize_endpoints(clat, clon, 3000, 0.0)
    # Heading 0 -> runs north/south only, no east/west component.
    assert he_lon == clon and le_lon == clon
    assert he_lat > clat > le_lat
