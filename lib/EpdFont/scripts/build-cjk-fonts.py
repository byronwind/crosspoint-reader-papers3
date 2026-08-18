#!/usr/bin/env python3
"""Download an open-source CJK font and convert it to SD-card .cpfont files.

Fetches a CJK-capable open-source font (Noto Sans CJK SC / Noto Serif CJK SC
by default), then runs fontconvert_sdcard.py to produce .cpfont files at the
UI + reader sizes (8,10,12,14,16,18). Having the UI sizes (8/10/12) in the
family enables the size-matched CJK fallback for interface strings, and the
reader sizes (12-18) cover book content.

Unlike build-sd-fonts.py (which converts only the Latin intervals for the
Latin families), this script converts the full "cjk" preset plus ascii,
latin1 and general punctuation (U+2000-206F: curly quotes, em dash,
ellipsis). ascii/latin1 are mandatory: any string containing a CJK
codepoint is rendered entirely with the SD font (GfxRenderer::
resolveTextFontId), so digits/Latin letters must exist in it too.

Usage:
    # Build with the default Noto Sans CJK SC (regular style)
    python3 build-cjk-fonts.py

    # Build the serif font (思源宋体, better for long-form reading)
    python3 build-cjk-fonts.py --font source-han-serif

    # Build the kai-style font (霞鹜文楷)
    python3 build-cjk-fonts.py --font lxgw-wenkai

    # Use a local font file instead of downloading
    python3 build-cjk-fonts.py --font-file /path/to/MyCJK.otf

    # Custom sizes / output directory / font name
    python3 build-cjk-fonts.py --sizes 8,10,12,14,16,18 --output-dir ./output

    # Include only reading sizes (no 8/10 UI fallback sizes)
    python3 build-cjk-fonts.py --sizes 12,14,16,18
"""

from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import urllib.request
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
FONTCONVERT = SCRIPT_DIR / "fontconvert_sdcard.py"
DOWNLOAD_DIR = SCRIPT_DIR / "downloaded_fonts"
DEFAULT_OUTPUT = SCRIPT_DIR / "output"

# Open-source CJK fonts with direct download URLs (raw GitHub, no auth).
# Each entry: (display name, family name, download URL). The font is cached
# in DOWNLOAD_DIR/<family_name>/ after the first fetch.
CJK_FONTS = {
    # Google Noto CJK (SIL OFL 1.1). Simplified Chinese faces.
    # 思源黑体 / Noto Sans CJK SC — sans-serif, good default.
    "noto-sans-sc": (
        "思源黑体 / Noto Sans CJK SC",
        "NotoSansCJKsc",
        "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf",
    ),
    # Adobe Source Han Serif SC (aka Noto Serif CJK SC, same outlines).
    # 思源宋体 / Source Han Serif SC — serif, better for long-form reading.
    "source-han-serif": (
        "思源宋体 / Source Han Serif SC",
        "NotoSerifCJKsc",
        "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Serif/OTF/SimplifiedChinese/NotoSerifCJKsc-Regular.otf",
    ),
    # Lxgw WenKai (霞鹜文楷), a popular open-source kai-style font.
    # SIL OFL 1.1. TTF from the v1.522 GitHub release.
    "lxgw-wenkai": (
        "霞鹜文楷 / Lxgw WenKai",
        "LXGWWenKai",
        "https://github.com/lxgw/LxgwWenKai/releases/download/v1.522/LXGWWenKai-Regular.ttf",
    ),
}

# UI sizes (8/12/14) + reader sizes (14/16/18). The 8/12/14 sizes back the
# size-matched CJK UI fallback in SdCardFontSystem::setupUiFallbacks() (the
# PaperS3 fork bumps the UI fonts to 12/14 pt).
DEFAULT_SIZES = "8,10,12,14,16,18"


_orig_getaddrinfo = socket.getaddrinfo


def _ipv4_only_getaddrinfo(*args, **kwargs):
    """getaddrinfo variant that drops AAAA records (IPv4 only)."""
    return [ai for ai in _orig_getaddrinfo(*args, **kwargs) if ai[0] == socket.AF_INET]


def download_font(url: str, dest: Path, retries: int = 3) -> Path:
    """Download a font file if not already cached. Returns the local path.

    Some mirrors advertise IPv6 addresses that a host without an IPv6 route
    cannot reach; retry forcing IPv4 resolution after the first attempt.
    """
    if dest.exists():
        print(f"  Using cached font: {dest}")
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"  Downloading {dest.name}...")
    last_err = None
    for attempt in range(1, retries + 1):
        force_ipv4 = attempt > 1
        if force_ipv4:
            socket.getaddrinfo = _ipv4_only_getaddrinfo
        try:
            urllib.request.urlretrieve(url, dest)
            break
        except Exception as e:  # noqa: BLE001 - reported via RuntimeError below
            last_err = e
            dest.unlink(missing_ok=True)
            if attempt < retries:
                print(f"  Attempt {attempt} failed ({e}); retrying (IPv4-only)...")
        finally:
            if force_ipv4:
                socket.getaddrinfo = _orig_getaddrinfo
    else:
        raise RuntimeError(f"Failed to download {url}: {last_err}") from last_err
    print(f"  Downloaded {dest.name} ({dest.stat().st_size / 1024 / 1024:.1f} MB)")
    return dest


def main():
    parser = argparse.ArgumentParser(
        description="Download an open-source CJK font and build SD-card .cpfont files."
    )
    parser.add_argument(
        "--font", dest="font", default="noto-sans-sc",
        choices=sorted(CJK_FONTS.keys()),
        help="Open-source CJK font to download (default: noto-sans-sc). "
             "Available: " + ", ".join(sorted(CJK_FONTS.keys())) + ".")
    parser.add_argument(
        "--font-file", dest="font_file", default=None,
        help="Use a local font file instead of downloading one.")
    parser.add_argument(
        "--name", dest="name", default=None,
        help="Font family name for output filenames (default: the font's family name).")
    parser.add_argument(
        "--sizes", dest="sizes", default=DEFAULT_SIZES,
        help=f"Comma-separated sizes (default: {DEFAULT_SIZES}).")
    parser.add_argument(
        "--intervals", dest="intervals", default="ascii,latin1,ipa-chars,cjk,punctuation",
        help="Comma-separated interval presets (default: ascii,latin1,ipa-chars,cjk,punctuation). "
        "'ascii'/'latin1' are required: strings containing any CJK codepoint are "
        "rendered ENTIRELY with the SD font (see GfxRenderer::resolveTextFontId), "
        "so the SD font must also cover digits/Latin letters or they vanish in "
        "Chinese text. 'ipa-chars' (U+0250-02FF) covers phonetic symbols like "
        "[bɪ'riːv] in dictionary cards — Noto Sans/Serif CJK lack those glyphs, "
        "so they are rasterized from the --fallback-font. 'punctuation' "
        "(U+2000-206F) adds the general punctuation "
        "Chinese books rely on: curly quotes ‘’ “”, em dash —, ellipsis ….")
    parser.add_argument(
        "--fallback-font", dest="fallback_font",
        default=str(SCRIPT_DIR.parent / "builtinFonts" / "source" / "NotoSans" / "NotoSans-Regular.ttf"),
        help="Latin font supplying glyphs the CJK face lacks (IPA extensions, "
        "spacing modifiers). Default: the bundled NotoSans-Regular.ttf; pass an "
        "empty string to disable.")
    parser.add_argument(
        "--output-dir", dest="output_dir", default=str(DEFAULT_OUTPUT),
        help="Output directory for .cpfont files (default: <scripts>/output).")
    parser.add_argument(
        "--force-autohint", dest="force_autohint", action="store_true",
        help="Force FreeType's auto-hinter instead of the font's native hinting.")
    args = parser.parse_args()

    output_base = Path(args.output_dir)
    output_base.mkdir(parents=True, exist_ok=True)

    # 1. Resolve the source font file (download or local path)
    if args.font_file:
        font_path = Path(args.font_file)
        if not font_path.exists():
            print(f"ERROR: font file not found: {font_path}", file=sys.stderr)
            sys.exit(1)
        family_name = args.name or font_path.stem
        for suffix in ["-Regular", "-Bold", "-Italic", "-BoldItalic",
                       "-regular", "-bold", "-italic", "-bolditalic"]:
            if family_name.endswith(suffix):
                family_name = family_name[: -len(suffix)]
                break
        print(f"  Using local font: {font_path}")
    else:
        display_name, family_name, url = CJK_FONTS[args.font]
        filename = url.rsplit("/", 1)[-1]
        dest = DOWNLOAD_DIR / family_name / filename
        print(f"  Font: {display_name}")
        font_path = download_font(url, dest)

    if args.name:
        family_name = args.name

    # 2. Convert with fontconvert_sdcard.py
    cmd = [
        sys.executable, str(FONTCONVERT),
        str(font_path),
        "--intervals", args.intervals,
        "--sizes", args.sizes,
        "--style", "regular",
        "--name", family_name,
        "--output-dir", str(output_base / family_name) + "/",
    ]
    if args.force_autohint:
        cmd.append("--force-autohint")
    if args.fallback_font:
        fallback_path = Path(args.fallback_font)
        if not fallback_path.exists():
            print(f"WARNING: fallback font not found, continuing without it: {fallback_path}",
                  file=sys.stderr)
        else:
            cmd += ["--fallback-regular", str(fallback_path)]

    print(f"\n=== Converting {family_name} (sizes {args.sizes}) ===")
    print(f"  Command: {' '.join(str(c) for c in cmd)}\n")
    result = subprocess.run(cmd)
    if result.returncode != 0:
        print(f"ERROR: fontconvert_sdcard.py exited with code {result.returncode}",
              file=sys.stderr)
        sys.exit(result.returncode)

    # 3. Summary
    family_dir = output_base / family_name
    files = sorted(family_dir.glob("*.cpfont"))
    total_size = sum(f.stat().st_size for f in files)
    print(f"\n=== Done: {len(files)} .cpfont files, {total_size / 1024 / 1024:.1f} MB ===")
    for f in files:
        print(f"  {f.name} ({f.stat().st_size / 1024:.0f} KB)")
    print(f"\nCopy the folder '{family_dir.relative_to(output_base)}' to the SD card:")
    print(f"  /.fonts/{family_name}/   (hidden root, preferred)")
    print(f"  /fonts/{family_name}/    (visible root)")
    print("Then select it under Settings > Reader > Font Family for CJK rendering "
          "in both book content and the UI.")


if __name__ == "__main__":
    main()