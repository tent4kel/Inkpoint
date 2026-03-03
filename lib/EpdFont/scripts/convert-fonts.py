#!/usr/bin/env python3
"""
Font generation driver — reads fonts.json and invokes fontconvert.py for each
(font, style, size) combination.

Usage (from the scripts/ directory):
  python3 convert-fonts.py [--dry-run] [--font NAME] [--size SIZE]

Options:
  --dry-run    Print commands without running them.
  --font NAME  Limit to a specific font name (e.g. bookerly).
  --size SIZE  Limit to a specific size (e.g. 11.5).
"""

import json
import os
import subprocess
import sys
import argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "fonts.json")
FONTCONVERT = os.path.join(SCRIPT_DIR, "fontconvert.py")

STYLE_ORDER = ["regular", "bold", "italic", "bolditalic"]


def size_to_name(s):
    """11 → '11', 11.5 → '11_5'"""
    if s == int(s):
        return str(int(s))
    return f"{s:.1f}".replace(".", "_")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="Print commands without executing.")
    ap.add_argument("--font", metavar="NAME", help="Only generate this font family.")
    ap.add_argument("--size", metavar="SIZE", type=float, help="Only generate this size.")
    args = ap.parse_args()

    with open(CONFIG_PATH) as f:
        config = json.load(f)

    output_dir = os.path.normpath(os.path.join(SCRIPT_DIR, config["outputDir"]))
    fonts = config["fonts"]

    total = 0
    errors = 0

    for font in fonts:
        name = font["name"]
        if args.font and name != args.font:
            continue

        compress = font.get("compress", False)
        bit2 = font.get("2bit", False)
        sources = font["sources"]

        for size_entry in font["sizes"]:
            size = size_entry["size"]
            if args.size is not None and size != args.size:
                continue

            force_autohint = size_entry.get("force_autohint", False)
            sname = size_to_name(size)

            for style in STYLE_ORDER:
                if style not in sources:
                    continue

                ttf = os.path.normpath(os.path.join(SCRIPT_DIR, sources[style]))
                font_id_name = f"{name}_{sname}_{style}"
                out_path = os.path.join(output_dir, f"{font_id_name}.h")

                cmd = [sys.executable, FONTCONVERT, font_id_name, str(size), ttf]
                if bit2:
                    cmd.append("--2bit")
                if compress:
                    cmd.append("--compress")
                if force_autohint:
                    cmd.append("--force-autohint")

                print(f"  {font_id_name}.h", end="", flush=True)
                if args.dry_run:
                    print(f"  [dry-run] {' '.join(cmd)}")
                    continue

                try:
                    result = subprocess.run(cmd, capture_output=True, text=True, check=True)
                    with open(out_path, "w") as out:
                        out.write(result.stdout)
                    print(" OK")
                    total += 1
                except subprocess.CalledProcessError as e:
                    print(" FAILED")
                    print(e.stderr, file=sys.stderr)
                    errors += 1

    if not args.dry_run:
        print(f"\nDone: {total} generated, {errors} failed.")
    if errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
