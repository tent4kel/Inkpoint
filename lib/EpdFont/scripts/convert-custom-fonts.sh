#!/bin/bash
# Regenerate all custom fonts (ChareInk, Newsreader, extra Bookerly/NotoSans sizes).
# Run this after updating fontconvert.py or the source TTF files.

set -e

cd "$(dirname "$0")"

SRCDIR_BOOKERLY="../builtinFonts/source/Bookerly"
SRCDIR_CHAREINK="../builtinFonts/source/ChareInk"
SRCDIR_NEWSREADER="../builtinFonts/source/Newsreader"
SRCDIR_NOTOSANS="../builtinFonts/source/NotoSans"
OUTDIR="../builtinFonts"

STYLES_MAP=("Regular" "Bold" "Italic" "BoldItalic")
LOWER_MAP=("regular" "bold" "italic" "bolditalic")

echo "=== Bookerly extra sizes ==="
for size in 10 11; do
  for i in "${!STYLES_MAP[@]}"; do
    style="${STYLES_MAP[$i]}"
    lower="${LOWER_MAP[$i]}"
    name="bookerly_${size}_${lower}"
    python3 fontconvert.py "$name" "$size" "$SRCDIR_BOOKERLY/Bookerly-${style}.ttf" \
      --2bit --compress --force-autohint > "$OUTDIR/${name}.h"
    echo "  Generated ${name}.h"
  done
done

echo "=== ChareInk all sizes ==="
for size in 12 13 14 15 16 18 19; do
  for i in "${!STYLES_MAP[@]}"; do
    style="${STYLES_MAP[$i]}"
    lower="${LOWER_MAP[$i]}"
    name="chareink_${size}_${lower}"
    python3 fontconvert.py "$name" "$size" "$SRCDIR_CHAREINK/ChareInk7SP-${style}.ttf" \
      --2bit --compress --force-autohint > "$OUTDIR/${name}.h"
    echo "  Generated ${name}.h"
  done
done

echo "=== Newsreader all sizes ==="
for size in 11 12 13 14 15 16 18 19; do
  for i in "${!STYLES_MAP[@]}"; do
    style="${STYLES_MAP[$i]}"
    lower="${LOWER_MAP[$i]}"
    name="newsreader_${size}_${lower}"
    python3 fontconvert.py "$name" "$size" "$SRCDIR_NEWSREADER/Newsreader-${style}.ttf" \
      --2bit --compress --force-autohint > "$OUTDIR/${name}.h"
    echo "  Generated ${name}.h"
  done
done

echo "=== NotoSans extra sizes ==="
for size in 10 11; do
  for i in "${!STYLES_MAP[@]}"; do
    style="${STYLES_MAP[$i]}"
    lower="${LOWER_MAP[$i]}"
    name="notosans_${size}_${lower}"
    python3 fontconvert.py "$name" "$size" "$SRCDIR_NOTOSANS/NotoSans-${style}.ttf" \
      --2bit --compress > "$OUTDIR/${name}.h"
    echo "  Generated ${name}.h"
  done
done

echo ""
echo "Done. All custom fonts regenerated."
