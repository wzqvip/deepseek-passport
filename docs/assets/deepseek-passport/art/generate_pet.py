#!/usr/bin/env python3
"""Generate the DeepSeek Passport pet sprites as PNG frames.

Pure standard library: no Pillow, no image library, no network. Every frame is
drawn into a small indexed grid and then scaled by an integer factor, so the
output is deterministic and the logical pixel size is explicit.

The logical canvas is 32x32 and frames are written at 2x (64x64). `--report`
prints the per-frame byte cost at the size the firmware would store, so the flash
budget in the architecture document stays honest.

Usage:
    python generate_pet.py --report --out .
    python generate_pet.py --scale 8 --out preview   # large frames for eyeballing

Design notes, palette, and the frame list are documented in README.md beside this
script.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

# --- canvas -----------------------------------------------------------------

LOGICAL = 32
SCALE = 2

# --- palette ----------------------------------------------------------------
# Index 0 is transparent and is never drawn.
TRANSPARENT = 0
OUTLINE = 1
BODY = 2
BODY_DARK = 3
BODY_LIGHT = 4
BELLY = 5
EYE = 6
EYE_WHITE = 7
BLUSH = 8
WATER = 9
WATER_LIGHT = 10
SLEEP = 11

PALETTE: tuple[tuple[str, tuple[int, int, int]] | None, ...] = (
    None,                               # 0 transparent
    ("outline", (0x10, 0x18, 0x28)),
    ("body", (0x4D, 0x6B, 0xFE)),       # DeepSeek brand blue
    ("body-dark", (0x2B, 0x3A, 0x8F)),
    ("body-light", (0x7A, 0x92, 0xFF)),
    ("belly", (0xEA, 0xF0, 0xFF)),
    ("eye", (0x10, 0x18, 0x28)),
    ("eye-white", (0xFF, 0xFF, 0xFF)),
    ("blush", (0xFF, 0x9E, 0xB1)),
    ("water", (0x8F, 0xD4, 0xFF)),
    ("water-light", (0xFF, 0xFF, 0xFF)),
    ("sleep", (0xC8, 0xD6, 0xFF)),
)

# Colours that receive the dark silhouette border. Decorations such as the spout,
# the listening waves, and the sleep marks intentionally do not.
OUTLINED = frozenset({BODY, BODY_DARK, BODY_LIGHT, BELLY, EYE, EYE_WHITE, BLUSH})

# --- grid -------------------------------------------------------------------


class Grid:
    """An indexed pixel grid with a few filled-shape primitives."""

    def __init__(self, size: int = LOGICAL) -> None:
        self.size = size
        self.px = [[TRANSPARENT] * size for _ in range(size)]

    def set(self, x: int, y: int, c: int) -> None:
        if 0 <= x < self.size and 0 <= y < self.size:
            self.px[y][x] = c

    def get(self, x: int, y: int) -> int:
        if 0 <= x < self.size and 0 <= y < self.size:
            return self.px[y][x]
        return TRANSPARENT

    def ellipse(self, cx: float, cy: float, rx: float, ry: float, c: int,
                only_over: frozenset[int] | None = None) -> None:
        for y in range(self.size):
            for x in range(self.size):
                if only_over is not None and self.px[y][x] not in only_over:
                    continue
                dx = (x + 0.5 - cx) / rx
                dy = (y + 0.5 - cy) / ry
                if dx * dx + dy * dy <= 1.0:
                    self.px[y][x] = c

    def rect(self, x: int, y: int, w: int, h: int, c: int,
             only_over: frozenset[int] | None = None) -> None:
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                if only_over is not None and self.get(xx, yy) not in only_over:
                    continue
                self.set(xx, yy, c)

    def triangle(self, p0, p1, p2, c: int) -> None:
        (x0, y0), (x1, y1), (x2, y2) = p0, p1, p2
        for y in range(min(y0, y1, y2), max(y0, y1, y2) + 1):
            for x in range(min(x0, x1, x2), max(x0, x1, x2) + 1):
                px, py = x + 0.5, y + 0.5
                w0 = (x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)
                w1 = (x2 - x1) * (py - y1) - (px - x1) * (y2 - y1)
                w2 = (x0 - x2) * (py - y2) - (px - x2) * (y0 - y2)
                if (w0 >= 0 and w1 >= 0 and w2 >= 0) or (w0 <= 0 and w1 <= 0 and w2 <= 0):
                    self.set(x, y, c)

    def outline(self, c: int = OUTLINE) -> None:
        """Wrap the silhouette in a one-pixel dark border."""
        border = []
        for y in range(self.size):
            for x in range(self.size):
                if self.px[y][x] != TRANSPARENT:
                    continue
                for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                    if self.get(nx, ny) in OUTLINED:
                        border.append((x, y))
                        break
        for x, y in border:
            self.px[y][x] = c


# --- the whale --------------------------------------------------------------
# Facing right. 3/4 view: both eyes visible, tail at the back left, spout on top.
# All coordinates are logical pixels on the 32x32 grid.

EYE_X = (16, 22)          # left edge of each 3x3 eye
BODY_CY = 19


def draw_whale(g: Grid, bob: int = 0, tail_up: bool = False) -> None:
    y = bob
    # tail peduncle and fluke
    g.rect(6, 17 + y, 3, 4, BODY)
    if tail_up:
        g.triangle((7, 17 + y), (2, 11 + y), (2, 17 + y), BODY)
        g.triangle((7, 20 + y), (2, 23 + y), (2, 20 + y), BODY)
    else:
        g.triangle((7, 17 + y), (2, 13 + y), (2, 17 + y), BODY)
        g.triangle((7, 20 + y), (2, 25 + y), (2, 20 + y), BODY)
    # dorsal fin and pectoral fin
    g.triangle((13, 15 + y), (16, 8 + y), (19, 15 + y), BODY)
    g.triangle((19, 24 + y), (24, 24 + y), (21, 28 + y), BODY)
    # body, then the rounder head at the front
    g.ellipse(16, BODY_CY + y, 10, 6.5, BODY)
    g.ellipse(21, 18 + y, 6, 7, BODY)
    # light band along the back, and the pale belly
    g.ellipse(16, 14.5 + y, 8, 1.6, BODY_LIGHT, only_over=frozenset({BODY}))
    g.ellipse(21, 23 + y, 6.5, 3.5, BELLY, only_over=frozenset({BODY}))


def draw_eyes(g: Grid, style: str, look: tuple[int, int] = (0, 0), bob: int = 0) -> None:
    y = bob
    dx, dy = look
    for ex in EYE_X:
        if style == "open":
            g.rect(ex, 15 + y, 3, 3, EYE_WHITE)
            g.rect(ex + 1 + dx, 16 + y + dy, 2, 2, EYE)
        elif style == "blink":
            g.rect(ex, 17 + y, 3, 1, EYE)
        elif style == "happy":
            g.set(ex, 18 + y, EYE)
            g.set(ex + 1, 17 + y, EYE)
            g.set(ex + 2, 18 + y, EYE)
        elif style == "sleep":
            g.rect(ex, 17 + y, 3, 1, EYE)
            g.set(ex + 2, 18 + y, EYE)
        elif style == "sad":
            g.set(ex, 17 + y, EYE)
            g.set(ex + 1, 18 + y, EYE)
            g.set(ex + 2, 17 + y, EYE)


def draw_face(g: Grid, mouth: str, bob: int = 0, blush: bool = True) -> None:
    y = bob
    if blush:
        g.rect(14, 20 + y, 2, 1, BLUSH)
        g.rect(26, 20 + y, 2, 1, BLUSH)
    if mouth == "smile":
        g.rect(19, 22 + y, 4, 1, EYE)
        g.set(18, 21 + y, EYE)
        g.set(23, 21 + y, EYE)
    elif mouth == "open":
        g.rect(20, 21 + y, 3, 2, EYE)
    elif mouth == "flat":
        g.rect(19, 22 + y, 4, 1, EYE)
    elif mouth == "worry":
        g.set(19, 23 + y, EYE)
        g.set(20, 22 + y, EYE)
        g.set(22, 22 + y, EYE)
        g.set(23, 23 + y, EYE)


def draw_spout(g: Grid, lift: int = 0, bob: int = 0) -> None:
    base = 9 + bob - lift
    g.rect(23, base, 1, 3, WATER)
    g.ellipse(23, base - 2, 3.5, 2.4, WATER)
    g.ellipse(20, base - 3, 1.6, 1.4, WATER_LIGHT)
    g.ellipse(26, base - 2, 1.4, 1.2, WATER_LIGHT)


def draw_waves(g: Grid, bob: int = 0) -> None:
    g.rect(28, 16 + bob, 1, 5, WATER)
    g.rect(30, 14 + bob, 1, 9, WATER)


def draw_thought(g: Grid, bob: int = 0) -> None:
    g.ellipse(24, 11 + bob, 1.0, 1.0, SLEEP)
    g.ellipse(27, 8 + bob, 1.5, 1.5, SLEEP)
    g.ellipse(30, 5 + bob, 1.1, 1.1, SLEEP)


def draw_zzz(g: Grid, bob: int = 0) -> None:
    g.rect(26, 8 + bob, 4, 1, SLEEP)
    g.set(29, 9 + bob, SLEEP)
    g.set(28, 10 + bob, SLEEP)
    g.set(27, 11 + bob, SLEEP)
    g.rect(26, 12 + bob, 4, 1, SLEEP)


def draw_sweat(g: Grid, bob: int = 0) -> None:
    g.set(28, 13 + bob, WATER)
    g.rect(29, 14 + bob, 1, 2, WATER)
    g.set(30, 13 + bob, WATER)


# --- frames -----------------------------------------------------------------

FRAMES: tuple[str, ...] = (
    "idle_a",
    "idle_b",
    "blink",
    "curious",
    "listening",
    "thinking",
    "working",
    "speaking",
    "happy",
    "sleeping",
    "distressed",
)


def build(name: str) -> Grid:
    """Build one named frame."""
    g = Grid()
    if name == "idle_a":
        draw_whale(g, bob=0)
        draw_eyes(g, "open", bob=0)
        draw_face(g, "smile", bob=0)
    elif name == "idle_b":
        draw_whale(g, bob=1)
        draw_eyes(g, "open", bob=1)
        draw_face(g, "smile", bob=1)
    elif name == "blink":
        draw_whale(g, bob=0)
        draw_eyes(g, "blink", bob=0)
        draw_face(g, "smile", bob=0)
    elif name == "curious":
        draw_whale(g, bob=0, tail_up=True)
        draw_eyes(g, "open", look=(-1, -1), bob=0)
        draw_face(g, "smile", bob=0)
    elif name == "listening":
        draw_whale(g, bob=0)
        draw_eyes(g, "open", look=(1, 0), bob=0)
        draw_face(g, "flat", bob=0)
        draw_waves(g)
    elif name == "thinking":
        draw_whale(g, bob=1)
        draw_eyes(g, "open", look=(0, -1), bob=1)
        draw_face(g, "flat", bob=1)
        draw_thought(g, bob=1)
    elif name == "working":
        # The tail is raised while work is in progress. DeepSeek's own harness
        # uses an animated whale tail as its "agent is working" indicator, so the
        # pet matches that convention rather than inventing a new one.
        draw_whale(g, bob=0, tail_up=True)
        draw_eyes(g, "open", look=(1, -1), bob=0)
        draw_face(g, "flat", bob=0, blush=False)
    elif name == "speaking":
        draw_whale(g, bob=0)
        draw_eyes(g, "open", bob=0)
        draw_face(g, "open", bob=0)
        draw_spout(g, lift=0)
    elif name == "happy":
        draw_whale(g, bob=-1, tail_up=True)
        draw_eyes(g, "happy", bob=-1)
        draw_face(g, "open", bob=-1)
        draw_spout(g, lift=3, bob=-1)
    elif name == "sleeping":
        draw_whale(g, bob=1)
        draw_eyes(g, "sleep", bob=1)
        draw_face(g, "flat", bob=1, blush=False)
        draw_zzz(g, bob=1)
    elif name == "distressed":
        draw_whale(g, bob=0)
        draw_eyes(g, "sad", bob=0)
        draw_face(g, "worry", bob=0)
        draw_sweat(g)
    else:  # pragma: no cover - guarded by FRAMES
        raise ValueError(f"unknown frame {name!r}")
    g.outline()
    return g


# --- output -----------------------------------------------------------------


def render(grid: Grid, scale: int):
    size = grid.size * scale
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            idx = grid.px[y // scale][x // scale]
            entry = PALETTE[idx] if idx < len(PALETTE) else None
            if idx == TRANSPARENT or entry is None:
                row.append((0, 0, 0, 0))
            else:
                r, gg, b = entry[1]
                row.append((r, gg, b, 255))
        rows.append(row)
    return size, rows


def write_png(path: Path, rows) -> None:
    height = len(rows)
    width = len(rows[0])
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for r, g, b, a in row:
            raw += bytes((r, g, b, a))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def write_contact_sheet(path: Path, size: int, frames, columns: int = 5,
                        gap: int = 2) -> None:
    rows_count = (len(frames) + columns - 1) // columns
    sheet_w = columns * size + (columns + 1) * gap
    sheet_h = rows_count * size + (rows_count + 1) * gap
    background = (0x17, 0x20, 0x2A, 255)
    sheet = [[background for _ in range(sheet_w)] for _ in range(sheet_h)]
    for i, (_name, rows) in enumerate(frames):
        col, row = i % columns, i // columns
        ox = gap + col * (size + gap)
        oy = gap + row * (size + gap)
        for y in range(size):
            for x in range(size):
                if rows[y][x][3]:
                    sheet[oy + y][ox + x] = rows[y][x]
    write_png(path, sheet)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=".", help="output directory")
    parser.add_argument("--scale", type=int, default=SCALE)
    parser.add_argument("--report", action="store_true",
                        help="print per-frame sizes and the palette")
    parser.add_argument("--sheet", action="store_true", default=True,
                        help="also write pet_contact_sheet.png")
    args = parser.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    rendered = []
    for name in FRAMES:
        grid = build(name)
        size, rows = render(grid, args.scale)
        write_png(out / f"pet_{name}.png", rows)
        rendered.append((name, rows))
        if args.report:
            opaque = sum(1 for row in rows for px in row if px[3])
            print(f"{name:11s} {size}x{size}  opaque={opaque:5d}  "
                  f"rgb565a8={size * size * 3:6d} B")
    size = len(rendered[0][1])
    if args.sheet:
        write_contact_sheet(out / "pet_contact_sheet.png", size, rendered)
    if args.report:
        print(f"\nlogical grid {LOGICAL}x{LOGICAL}, scale {args.scale}, "
              f"frames {len(FRAMES)}")
        print(f"total rgb565a8 {len(FRAMES) * size * size * 3} B "
              f"({len(FRAMES) * size * size * 3 / 1024:.1f} KiB) in flash")
        print("\npalette:")
        for i, entry in enumerate(PALETTE):
            if entry is not None:
                print(f"  {i:2d} {entry[0]:12s} "
                      f"#{entry[1][0]:02X}{entry[1][1]:02X}{entry[1][2]:02X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
