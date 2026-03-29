#!/usr/bin/env python3
"""
Convert a directory of PNG frames to a C++ header with u8g2-compatible
XBM bitmaps (LSB-first, "swap bits in byte" applied).

Usage:
    python3 tools/frames_to_cpp.py \
        --input frames/ \
        --output include/animation_frames.h \
        --threshold 128 \
        --invert \
        --no-swap \
        --resize 64x64 \
        --autocrop

For u8g2 drawXBM + white bg: use --invert --no-swap --autocrop
"""

import argparse
import sys
from pathlib import Path
from PIL import Image

SWAP_TABLE = [int(f"{i:08b}"[::-1], 2) for i in range(256)]


def compute_crop_box(frame_paths: list, threshold: int) -> tuple | None:
    """Find the tight bounding box of dark (face) content across all frames."""
    combined = None
    for p in frame_paths:
        img = Image.open(p).convert("L")
        # Mask: dark pixels (face content) → 255, background → 0
        mask = img.point(lambda px: 255 if px < threshold else 0)
        bbox = mask.getbbox()
        if bbox:
            combined = bbox if combined is None else (
                min(combined[0], bbox[0]),
                min(combined[1], bbox[1]),
                max(combined[2], bbox[2]),
                max(combined[3], bbox[3]),
            )
    if combined is None:
        return None
    # Expand to a square around the centre of the tight box
    cx = (combined[0] + combined[2]) // 2
    cy = (combined[1] + combined[3]) // 2
    half = max(combined[2] - combined[0], combined[3] - combined[1]) // 2 + 4  # 4 px padding
    orig_w, orig_h = Image.open(frame_paths[0]).size
    return (
        max(0, cx - half),
        max(0, cy - half),
        min(orig_w, cx + half),
        min(orig_h, cy + half),
    )


def img_to_xbm_bytes(img: Image.Image, threshold: int, invert: bool, swap: bool) -> list[int]:
    """Convert a PIL image to a list of bytes in XBM (LSB-first) format."""
    bw = img.convert("L")
    w, h = bw.size
    rows = []
    for y in range(h):
        byte = 0
        bit = 0
        row_bytes = []
        for x in range(w):
            pixel = bw.getpixel((x, y))
            on = pixel >= threshold
            if invert:
                on = not on
            if on:
                byte |= (1 << bit)
            bit += 1
            if bit == 8:
                row_bytes.append(SWAP_TABLE[byte] if swap else byte)
                byte = 0
                bit = 0
        if bit > 0:
            row_bytes.append(SWAP_TABLE[byte] if swap else byte)
        rows.extend(row_bytes)
    return rows


def format_bytes(data: list[int], cols: int = 16) -> str:
    lines = []
    for i in range(0, len(data), cols):
        chunk = data[i:i + cols]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk))
    return ",\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="PNG frames → C++ XBM header for u8g2")
    parser.add_argument("--input", default="frames", help="Directory containing frame_XX.png files")
    parser.add_argument("--output", default="include/animation_frames.h", help="Output .h file")
    parser.add_argument("--threshold", type=int, default=128, help="Binarisation threshold 0-255 (default 128)")
    parser.add_argument("--invert", action="store_true", help="Invert pixel values (white↔black)")
    parser.add_argument("--no-swap", dest="swap", action="store_false", help="Disable bit-swap (LSB-first for drawXBM)")
    parser.add_argument("--resize", metavar="WxH", help="Resize frames after crop, e.g. 64x64")
    parser.add_argument("--autocrop", action="store_true", help="Crop to tight bounding box of face content (consistent across all frames)")
    parser.set_defaults(swap=True)
    args = parser.parse_args()

    target_size = None
    if args.resize:
        try:
            rw, rh = args.resize.lower().split("x")
            target_size = (int(rw), int(rh))
        except ValueError:
            print("--resize must be in WxH format, e.g. 64x64", file=sys.stderr)
            sys.exit(1)

    input_dir = Path(args.input)
    output_path = Path(args.output)

    frames = sorted(input_dir.glob("frame_*.png"))
    if not frames:
        print(f"No frame_*.png files found in {input_dir}", file=sys.stderr)
        sys.exit(1)

    crop_box = None
    if args.autocrop:
        crop_box = compute_crop_box(frames, args.threshold)
        if crop_box:
            cw = crop_box[2] - crop_box[0]
            ch = crop_box[3] - crop_box[1]
            print(f"Autocrop box: {crop_box}  ({cw}×{ch} px)")
        else:
            print("WARNING: autocrop found no content, skipping crop")

    # Determine output dimensions from first frame
    sample = Image.open(frames[0])
    if crop_box:
        sample = sample.crop(crop_box)
    if target_size:
        sample = sample.resize(target_size, Image.LANCZOS)
    width, height = sample.size
    bytes_per_row = (width + 7) // 8
    bytes_per_frame = bytes_per_row * height

    print(f"Found {len(frames)} frames → output size {width}×{height}, {bytes_per_frame} bytes/frame")

    lines = [
        "// Auto-generated by tools/frames_to_cpp.py — do not edit manually.",
        f"// {len(frames)} frames, {width}x{height} px, {bytes_per_row} bytes/row.",
        f"// Bit order: {'LSB-first (u8g2 drawXBM)' if not args.swap else 'MSB-first (u8g2 drawBitmap)'}",
        "#pragma once",
        "#include <Arduino.h>",
        "",
        f"static const uint8_t ANIM_FRAME_WIDTH  = {width};",
        f"static const uint8_t ANIM_FRAME_HEIGHT = {height};",
        f"static const uint8_t ANIM_FRAME_COUNT  = {len(frames)};",
        f"static const uint16_t ANIM_BYTES_PER_FRAME = {bytes_per_frame};",
        "",
    ]

    all_frame_data = []
    for frame_path in frames:
        img = Image.open(frame_path)
        if crop_box:
            img = img.crop(crop_box)
        if target_size:
            img = img.resize(target_size, Image.LANCZOS)
        data = img_to_xbm_bytes(img, args.threshold, args.invert, args.swap)
        all_frame_data.append((frame_path.stem, data))

    lines.append("// All frames packed into one PROGMEM array.")
    lines.append(f"static const uint8_t ANIM_FRAMES[{len(frames)}][{bytes_per_frame}] PROGMEM = {{")
    for name, data in all_frame_data:
        lines.append(f"  // {name}")
        lines.append("  {")
        lines.append(format_bytes(data))
        lines.append("  },")
    lines.append("};")
    lines.append("")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(lines) + "\n")
    print(f"Written → {output_path}")


if __name__ == "__main__":
    main()
