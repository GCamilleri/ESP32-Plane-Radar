#!/usr/bin/env python3
"""Choose the social-tag colour palette so it cannot be confused with existing UI.

The radar already spends most of the colour wheel on meaning: red aircraft, cyan
military, amber type, light-blue altitude, magenta track vectors, violet runways,
white labels. Picking tag colours by eye put two of them next to the altitude
colour, so this picks them by measurement instead.

Method: generate saturated, bright candidates across the hue circle, convert to
CIELAB, and greedily take the candidate whose nearest neighbour (among the reserved
UI colours and the colours already chosen) is furthest away. That maximises the
worst case, which is what matters -- one indistinguishable pair is the whole
problem.

Candidates are quantised to RGB565 first, because that is what the panel actually
shows.

Run:  python3 scripts/pick_tag_palette.py [count]
"""
import sys

# Colours already carrying meaning on screen, from include/ui/radar_theme.h.
RESERVED = {
    "background": (4, 10, 28),
    "grid": (16, 100, 32),
    "label/select (white)": (255, 255, 255),
    "aircraft (red)": (255, 0, 0),
    "military (cyan)": (0, 230, 255),
    "track vector (magenta)": (255, 0, 255),
    "tag type (amber)": (255, 200, 0),
    "tag altitude (light blue)": (90, 200, 255),
    "runway": (90, 70, 160),
    "runway label": (140, 120, 200),
    "freshness fresh": (0, 200, 0),
    "freshness aging": (255, 180, 0),
}


def to_rgb565_rounded(rgb):
    """Quantise to RGB565 and back, so we compare what the panel can actually show."""
    r, g, b = rgb
    r5, g6, b5 = r >> 3, g >> 2, b >> 3
    return (r5 << 3 | r5 >> 2, g6 << 2 | g6 >> 4, b5 << 3 | b5 >> 2)


def srgb_to_linear(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def rgb_to_lab(rgb):
    r, g, b = (srgb_to_linear(float(c)) for c in rgb)
    # sRGB D65 -> XYZ
    x = r * 0.4124 + g * 0.3576 + b * 0.1805
    y = r * 0.2126 + g * 0.7152 + b * 0.0722
    z = r * 0.0193 + g * 0.1192 + b * 0.9505
    # Normalise to D65 white
    x, y, z = x / 0.95047, y / 1.00000, z / 1.08883

    def f(t):
        return t ** (1 / 3) if t > 0.008856 else (7.787 * t) + (16 / 116)

    fx, fy, fz = f(x), f(y), f(z)
    return (116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))


def delta_e(a, b):
    la, aa, ba = rgb_to_lab(a)
    lb, ab, bb = rgb_to_lab(b)
    return ((la - lb) ** 2 + (aa - ab) ** 2 + (ba - bb) ** 2) ** 0.5


def hsv_to_rgb(h, s, v):
    h = h % 360
    c = v * s
    x = c * (1 - abs(((h / 60) % 2) - 1))
    m = v - c
    table = [(c, x, 0), (x, c, 0), (0, c, x), (0, x, c), (x, 0, c), (c, 0, x)]
    r, g, b = table[int(h // 60) % 6]
    return tuple(round((ch + m) * 255) for ch in (r, g, b))


def candidates():
    """Bright and saturated only: a tag reticle has to read on a dark background."""
    seen = set()
    for hue in range(0, 360, 3):
        for sat in (1.0, 0.82, 0.62):
            for val in (1.0, 0.85):
                rgb = to_rgb565_rounded(hsv_to_rgb(hue, sat, val))
                # Reject anything too dark to see against the navy background.
                if rgb_to_lab(rgb)[0] < 55:
                    continue
                if rgb not in seen:
                    seen.add(rgb)
                    yield rgb


def nearest_reserved(rgb, extra):
    best_name, best_d = None, float("inf")
    for name, other in list(RESERVED.items()) + [(f"tag {i}", c) for i, c in enumerate(extra)]:
        d = delta_e(rgb, other)
        if d < best_d:
            best_name, best_d = name, d
    return best_name, best_d


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    pool = list(candidates())
    chosen = []

    for _ in range(count):
        best, best_d, best_clash = None, -1.0, None
        for rgb in pool:
            name, d = nearest_reserved(rgb, chosen)
            if d > best_d:
                best, best_d, best_clash = rgb, d, name
        chosen.append(best)
        print(
            f"  {{{best[0]:3d}, {best[1]:3d}, {best[2]:3d}}},"
            f"  // nearest: {best_clash} (deltaE {best_d:.1f})"
        )

    worst = min(nearest_reserved(c, [x for x in chosen if x is not c])[1] for c in chosen)
    print(f"\nworst-case deltaE against anything else on screen: {worst:.1f}")
    print("(deltaE 2.3 is 'just noticeable'; above ~25 reads as a different colour)")


if __name__ == "__main__":
    main()
