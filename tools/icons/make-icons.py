#!/usr/bin/env python3
"""Render the desktop icon set from the shared FujiNet Go launcher art.

The artwork (data/icons/src/fujinet-go-sms-foreground.png) is the exact same
transparent FujiNet mark the other desktop apps use -- byte-identical, copied
from fujinet-go-nes-desktop's own copy -- composited over this product's own
background, so the whole family reads as one product line while each target
still gets a distinct badge.

One thing is specific to this target: the background is the Master System's
own livery. The colour is the red of the Master System logo and cartridge
labels (#E4002B), and behind the mark sits the square white grid those labels
(and the boxes) are printed with. The mark itself stays the family's white,
as on the astrocade and ColecoVision icons. Both choices are the user's
(2026-10-07). The red is deliberately brighter than the Astrocade's dark red
(#8B0000) and bluer than the Apple II's (#F44336) so the three stay distinct
in a dock.

The grid is drawn thin and translucent so the mark still leads: at 16px it
reduces to a faint texture, which is what a printed grid does at that size
too. It is drawn at 4x like the rounded square, so the lines are
antialiased, and masked to the rounded square so it never leaks past the
corners.

Everything else about the composite (rounded-square mask, corner radius,
foreground zoom, output sizes) matches the sibling repos' own
tools/icons/make-icons.py exactly.

The results are committed (data/icons/hicolor/..., data/icons/*.icns,
frontends/windows/app.ico) so building the project needs no image tooling;
re-run this only when the artwork, colour or grid changes:

    python3 tools/icons/make-icons.py
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
FOREGROUND = ROOT / "data/icons/src/fujinet-go-sms-foreground.png"
OUTDIR = ROOT / "data/icons/hicolor"
ICNS_OUT = ROOT / "data/icons/fujinet-go-sms.icns"
ICO_OUT = ROOT / "frontends/windows/app.ico"

BACKGROUND = (0xE4, 0x00, 0x2B, 0xFF)    # #E4002B, Master System red
GRID_RGB = (0xFF, 0xFF, 0xFF)            # the labels' white grid
GRID_ALPHA = 0x38                        # translucent: the mark leads
GRID_CELLS = 16                          # squares across the icon
GRID_LINE = 0.0055                       # line width, fraction of the edge
MASTER = 1024                            # render big, downsample with LANCZOS
CORNER_RADIUS = 0.22                     # fraction of the edge
FOREGROUND_ZOOM = 1.18                   # Android's mask crops; compensate a little
SIZES = (16, 24, 32, 48, 64, 128, 192, 256, 512)
ICNS_SIZES = (32, 64, 128, 256, 512, 1024)  # every size macOS's icns TOC references


def render_grid(scale: int) -> Image.Image:
    """The square cartridge-label grid at MASTER*scale, centred so the middle
    of the icon falls on a crossing (the mark's centre disc sits on it)."""
    edge = MASTER * scale
    grid = Image.new("RGBA", (edge, edge), (0, 0, 0, 0))
    draw = ImageDraw.Draw(grid)
    pitch = edge / GRID_CELLS
    width = max(1, round(edge * GRID_LINE))
    colour = GRID_RGB + (GRID_ALPHA,)
    centre = edge / 2
    k = -GRID_CELLS
    while k <= GRID_CELLS:
        pos = centre + k * pitch
        lo = round(pos - width / 2)
        if -width <= lo <= edge:
            draw.rectangle((lo, 0, lo + width - 1, edge - 1), fill=colour)
            draw.rectangle((0, lo, edge - 1, lo + width - 1), fill=colour)
        k += 1
    return grid


def render_master() -> Image.Image:
    art = Image.open(FOREGROUND).convert("RGBA")

    # Rounded-square background on a transparent canvas, drawn at 4x and
    # downsampled so the corners are antialiased.
    scale = 4
    big = Image.new("RGBA", (MASTER * scale, MASTER * scale), (0, 0, 0, 0))
    ImageDraw.Draw(big).rounded_rectangle(
        (0, 0, MASTER * scale - 1, MASTER * scale - 1),
        radius=int(MASTER * scale * CORNER_RADIUS),
        fill=BACKGROUND,
    )
    icon = big.resize((MASTER, MASTER), Image.LANCZOS)

    # The grid, under the mark and inside the rounded silhouette.
    grid = render_grid(scale).resize((MASTER, MASTER), Image.LANCZOS)
    grid_alpha = Image.composite(grid.getchannel("A"),
                                 Image.new("L", (MASTER, MASTER), 0),
                                 icon.getchannel("A"))
    grid.putalpha(grid_alpha)
    icon = Image.alpha_composite(icon, grid)

    art_size = int(MASTER * FOREGROUND_ZOOM)
    art = art.resize((art_size, art_size), Image.LANCZOS)
    offset = (MASTER - art_size) // 2
    overlay = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    overlay.paste(art, (offset, offset), art)

    # Keep the foreground inside the rounded silhouette.
    composed = Image.alpha_composite(icon, overlay)
    composed.putalpha(Image.composite(composed.getchannel("A"),
                                      Image.new("L", (MASTER, MASTER), 0),
                                      icon.getchannel("A")))
    return composed


def main() -> int:
    if not FOREGROUND.exists():
        print(f"missing artwork: {FOREGROUND}", file=sys.stderr)
        return 1

    master = render_master()
    for size in SIZES:
        out = OUTDIR / f"{size}x{size}" / "apps" / "fujinet-go-sms.png"
        out.parent.mkdir(parents=True, exist_ok=True)
        master.resize((size, size), Image.LANCZOS).save(out, optimize=True)
        print(f"wrote {out.relative_to(ROOT)}")

    # Pillow's ICNS writer works on any platform (no iconutil needed): it
    # just packs PNGs into the icns TOC. Pass every non-master size in
    # explicitly, LANCZOS-downsampled from the 1024 master like the hicolor
    # set above, so nothing gets a blurry re-resize from a smaller source.
    variants = [master.resize((size, size), Image.LANCZOS)
                for size in ICNS_SIZES if size != master.width]
    master.save(ICNS_OUT, format="ICNS", append_images=variants)
    print(f"wrote {ICNS_OUT.relative_to(ROOT)}")

    # The Windows .rc-embedded icon (frontends/windows/resource.rc's own
    # IDI_APPICON). Pillow's ICO writer packs whichever sizes are passed as
    # the `sizes` kwarg, resampling from `master` itself -- matching the
    # sibling repos' own app.ico (16x16 and 32x32, both 32bpp).
    ICO_OUT.parent.mkdir(parents=True, exist_ok=True)
    master.save(ICO_OUT, format="ICO", sizes=[(16, 16), (32, 32)])
    print(f"wrote {ICO_OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
