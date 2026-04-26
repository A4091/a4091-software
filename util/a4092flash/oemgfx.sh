#!/bin/bash
set -euo pipefail

usage() {
    echo "Usage: $0 picture.png" >&2
    exit 2
}

[[ $# -eq 1 ]] || usage

PICTURE=$1
[[ -f "${PICTURE}" ]] || {
    echo "error: not a file: ${PICTURE}" >&2
    exit 1
}

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

FILE_INFO=$(file "${PICTURE}")
[[ "${FILE_INFO}" == *"PNG image data"* ]] || {
    echo "error: expected a PNG: ${FILE_INFO}" >&2
    exit 1
}

if [[ "${FILE_INFO}" =~ ([0-9]+)[[:space:]]x[[:space:]]([0-9]+) ]]; then
    INPUT_W=${BASH_REMATCH[1]}
    INPUT_H=${BASH_REMATCH[2]}
else
    echo "error: could not parse PNG dimensions: ${FILE_INFO}" >&2
    exit 1
fi

TARGET_H=90
PREVIEW_H=$(( TARGET_H * 2 ))
# PAL is 1:1.875, NTSC is 1:2.4; simplify to 1:2
RAW_W=$(( (2 * INPUT_W * TARGET_H) / INPUT_H ))
TARGET_W=$(( ((RAW_W + 7) / 8) * 8 ))
if (( TARGET_W < 8 )); then
    TARGET_W=8
fi

RESIZE="${TARGET_W}x${TARGET_H}!"
PREVIEW_RESIZE="${TARGET_W}x${PREVIEW_H}!"
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/oemgfx.XXXXXX")
trap 'rm -rf "${TMP_DIR}"' EXIT

# Input/output configuration
RESIZED_PNG="${TMP_DIR}/A4092_small.png"
#OUTPUT_DIR="/tmp"
OUTPUT_DIR="$PWD"

ABC2=(time abc2)
MAGICK=(time magick)
MKOEMBLOB=(time "${SCRIPT_DIR}/mkoemblob.py")

# Forced colors for Amiga defaults (index color)
FORCECOLORS=(
    -forcecolor 0 aaa
    -forcecolor 1 000
    -forcecolor 2 fff
    -forcecolor 3 06f
)

# Step 1: Flatten transparent pixels to the Amiga default gray, then resize.
echo "==> ${PICTURE}: ${INPUT_W}x${INPUT_H} -> ${RESIZE}"
"${MAGICK[@]}" "${PICTURE}" \
    -background "#aaaaaa" -alpha remove -alpha off \
    -resize "${RESIZE}" "${RESIZED_PNG}"
magick identify "${RESIZED_PNG}"

# Step 2: Convert to Amiga bitmap at 16 colors (4 bpc) and 32 colors (5 bpc).
for BPC in 4 5; do
    OUT_BASE="${TMP_DIR}/out${BPC}"
    TARGET_PREVIEW="${TMP_DIR}/out${BPC}-target-preview.png"
    MAC_PREVIEW="${OUTPUT_DIR}/oem-${BPC}bpp.png"

    echo "==> Converting to ${BPC} bpc ($(( 1 << BPC )) colors)..."
    "${ABC2[@]}" "${RESIZED_PNG}" \
        -bpc "${BPC}" -quantize -uninterleaved \
        "${FORCECOLORS[@]}" \
        -preview "${TARGET_PREVIEW}" \
        -b "${OUT_BASE}.gfx" \
        -p "${OUT_BASE}.pal"

    echo "==> Rendering Mac preview ${MAC_PREVIEW} at ${PREVIEW_RESIZE}..."
    "${MAGICK[@]}" "${TARGET_PREVIEW}" \
        -filter point -resize "${PREVIEW_RESIZE}" \
        "${MAC_PREVIEW}"
    magick identify "${MAC_PREVIEW}"
done

# Step 3: Build the OEM flash blob. mkoemblob.py compresses the variants.
echo "==> Building ${OUTPUT_DIR}/oem.bin..."
"${MKOEMBLOB[@]}" \
    --width "${TARGET_W}" \
    --height "${TARGET_H}" \
    --gfx4 "${TMP_DIR}/out4.gfx" \
    --pal4 "${TMP_DIR}/out4.pal" \
    --gfx5 "${TMP_DIR}/out5.gfx" \
    --pal5 "${TMP_DIR}/out5.pal" \
    -o "${OUTPUT_DIR}/oem.bin"

ls -la "${OUTPUT_DIR}/oem.bin"
