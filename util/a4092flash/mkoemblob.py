#!/usr/bin/env python3
"""Build an OEM flash blob for the boot menu.

The blob contains up to three optional images:
  - 5 bitplanes (32 colors)
  - 4 bitplanes (16 colors)
  - 3 bitplanes (8 colors)

Each present image carries its own palette and placement coordinates.
Coordinates may be an absolute number or the literal string "center".
"""

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile

OEM_MAGIC = 0x4F454D00  # "OEM\0"
OEM_VERSION = 2
OEM_MAX_COLORS = 32
OEM_FLASH_SIZE = 0x1D000  # 116 KB
OEM_VARIANT_SLOTS = 3
OEM_COORD_CENTER = 0xFFFF
DEPTHS = (5, 4, 3)

HEADER_FORMAT = ">IHBBI"
VARIANT_FORMAT = ">HHHH64sIII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT) + OEM_VARIANT_SLOTS * struct.calcsize(VARIANT_FORMAT) + 4

SALVADOR_PATH = os.path.join(os.path.dirname(__file__), "..", "..", "3rdparty", "salvador", "salvador")


def xor32_checksum(data):
    """XOR32 over data (must be padded to a multiple of 4 bytes)."""
    pad = len(data) % 4
    if pad:
        data = data + b"\x00" * (4 - pad)
    checksum = 0
    for i in range(0, len(data), 4):
        checksum ^= struct.unpack(">I", data[i:i + 4])[0]
    return checksum


def pad_rows(raw, byte_width, word_width, height, depth):
    """Pad each plane-row from byte_width to word_width."""
    if byte_width == word_width:
        return raw

    out = bytearray()
    for plane in range(depth):
        offset = plane * byte_width * height
        for row in range(height):
            row_start = offset + row * byte_width
            out.extend(raw[row_start:row_start + byte_width])
            out.extend(b"\x00" * (word_width - byte_width))
    return bytes(out)


def parse_coord(value):
    if value is None:
        return OEM_COORD_CENTER
    if isinstance(value, int):
        coord = value
    else:
        text = value.strip().lower()
        if text == "center":
            return OEM_COORD_CENTER
        coord = int(text, 0)
    if coord < 0 or coord > OEM_COORD_CENTER:
        raise ValueError(f"coordinate {coord} is out of range 0..65535")
    return coord


def format_coord(value):
    return "center" if value == OEM_COORD_CENTER else str(value)


def slot_for_depth(depth):
    return 5 - depth


def compress_with_salvador(raw, salvador_path):
    if not os.path.isfile(salvador_path):
        sys.exit(f"Error: salvador not found at {salvador_path}")

    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as tmp_in:
        tmp_in.write(raw)
        tmp_in_path = tmp_in.name
    tmp_out_path = tmp_in_path + ".zx0"

    try:
        subprocess.run([salvador_path, tmp_in_path, tmp_out_path],
                       check=True, capture_output=True)
        with open(tmp_out_path, "rb") as handle:
            return handle.read()
    finally:
        os.unlink(tmp_in_path)
        if os.path.exists(tmp_out_path):
            os.unlink(tmp_out_path)


def build_variant(args, depth):
    gfx = getattr(args, f"gfx{depth}")
    pal = getattr(args, f"pal{depth}")
    if not gfx and not pal:
        return None
    if not gfx or not pal:
        sys.exit(f"Error: --gfx{depth} and --pal{depth} must be provided together")

    width = getattr(args, f"width{depth}")
    if width is None:
        width = args.width
    if width is None or width <= 0:
        sys.exit(f"Error: width is required for the {depth}-bitplane image")

    height = getattr(args, f"height{depth}")
    if height is None:
        height = args.height

    x_arg = getattr(args, f"x{depth}")
    if x_arg is None:
        x_arg = args.x
    y_arg = getattr(args, f"y{depth}")
    if y_arg is None:
        y_arg = args.y

    with open(gfx, "rb") as handle:
        gfx_data = handle.read()
    with open(pal, "rb") as handle:
        pal_data = handle.read()

    byte_width = math.ceil(width / 8)
    word_width = ((width + 15) // 16) * 2

    if not height:
        height = len(gfx_data) // (byte_width * depth)
        if height <= 0:
            sys.exit(f"Error: cannot auto-detect height for {depth}-bitplane image from {len(gfx_data)} bytes")

    expected_size = byte_width * depth * height
    if len(gfx_data) != expected_size:
        sys.exit(f"Error: {gfx} is {len(gfx_data)} bytes, expected {expected_size} "
                 f"({byte_width} x {depth} x {height})")

    num_colors = 1 << depth
    if len(pal_data) < num_colors * 2:
        sys.exit(f"Error: {pal} is {len(pal_data)} bytes, need at least {num_colors * 2}")

    palette = bytearray(OEM_MAX_COLORS * 2)
    palette[:len(pal_data[:OEM_MAX_COLORS * 2])] = pal_data[:OEM_MAX_COLORS * 2]

    padded = pad_rows(gfx_data, byte_width, word_width, height, depth)
    compressed = compress_with_salvador(padded, args.salvador)

    try:
        x = parse_coord(x_arg)
        y = parse_coord(y_arg)
    except ValueError as exc:
        sys.exit(f"Error: {exc}")

    return {
        "depth": depth,
        "width": width,
        "height": height,
        "x": x,
        "y": y,
        "palette": bytes(palette),
        "compressed": compressed,
        "compressed_size": len(compressed),
        "uncompressed_size": len(padded),
    }


def main():
    parser = argparse.ArgumentParser(description="Build an OEM boot image bundle")
    parser.add_argument("--width", type=int, help="Default image width for all present variants")
    parser.add_argument("--height", type=int, default=0, help="Default image height (auto-detect if 0)")
    parser.add_argument("--x", default="center", help='Default X coordinate or "center" (default: center)')
    parser.add_argument("--y", default="50", help='Default Y coordinate or "center" (default: 50)')
    parser.add_argument("--salvador", default=SALVADOR_PATH, help="Path to salvador compressor")
    parser.add_argument("-o", "--output", default="oem.bin", help="Output blob file")

    for depth in DEPTHS:
        parser.add_argument(f"--gfx{depth}", help=f"Raw {depth}-bitplane bitmap (e.g. out{depth}.gfx)")
        parser.add_argument(f"--pal{depth}", help=f"Palette for the {depth}-bitplane bitmap (e.g. out{depth}.pal)")
        parser.add_argument(f"--width{depth}", type=int, help=f"Width override for the {depth}-bitplane bitmap")
        parser.add_argument(f"--height{depth}", type=int, help=f"Height override for the {depth}-bitplane bitmap")
        parser.add_argument(f"--x{depth}", help=f'X override for the {depth}-bitplane bitmap or "center"')
        parser.add_argument(f"--y{depth}", help=f'Y override for the {depth}-bitplane bitmap or "center"')

    args = parser.parse_args()

    variants = [None] * OEM_VARIANT_SLOTS
    payloads = []
    present_count = 0
    total_size = HEADER_SIZE

    for depth in DEPTHS:
        variant = build_variant(args, depth)
        slot = slot_for_depth(depth)
        variants[slot] = variant
        if variant is None:
            continue
        present_count += 1
        payloads.append((slot, variant))
        total_size += variant["compressed_size"]

    if present_count == 0:
        sys.exit("Error: provide at least one of --gfx5/--gfx4/--gfx3")

    if total_size > OEM_FLASH_SIZE:
        sys.exit(f"Error: blob too large ({total_size} bytes, max {OEM_FLASH_SIZE})")

    offset = HEADER_SIZE
    packed_variants = []
    for slot in range(OEM_VARIANT_SLOTS):
        variant = variants[slot]
        if variant is None:
            packed_variants.append(struct.pack(VARIANT_FORMAT,
                                               0, 0, 0, 0,
                                               bytes(OEM_MAX_COLORS * 2),
                                               0, 0, 0))
            continue

        variant["data_offset"] = offset
        offset += variant["compressed_size"]
        packed_variants.append(struct.pack(
            VARIANT_FORMAT,
            variant["width"],
            variant["height"],
            variant["x"],
            variant["y"],
            variant["palette"],
            variant["compressed_size"],
            variant["uncompressed_size"],
            variant["data_offset"],
        ))

    header_no_checksum = struct.pack(
        HEADER_FORMAT,
        OEM_MAGIC,
        OEM_VERSION,
        present_count,
        0,
        total_size,
    ) + b"".join(packed_variants)
    checksum = xor32_checksum(header_no_checksum)
    header = header_no_checksum + struct.pack(">I", checksum)

    assert len(header) == HEADER_SIZE

    with open(args.output, "wb") as handle:
        handle.write(header)
        for slot, variant in payloads:
            del slot
            handle.write(variant["compressed"])

    print(f"OEM bundle: {present_count} image(s), total {total_size} bytes")
    for depth in DEPTHS:
        variant = variants[slot_for_depth(depth)]
        if variant is None:
            continue
        ratio = variant["compressed_size"] * 100 / variant["uncompressed_size"]
        print(f"  {depth} bitplanes: {variant['width']}x{variant['height']} "
              f"x={format_coord(variant['x'])} y={format_coord(variant['y'])} "
              f"({variant['uncompressed_size']} -> {variant['compressed_size']} bytes, {ratio:.1f}%)")
    print(f"Written to {args.output}")


if __name__ == "__main__":
    main()
