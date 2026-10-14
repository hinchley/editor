#!/usr/bin/env python3
# generates src/unicodedata.c from the unicode character database.
#
# usage: tools/unicodedata.py <ucd-dir> > src/unicodedata.c
#
# <ucd-dir> must contain these files from https://www.unicode.org/Public/<version>/ucd/:
#   UnicodeData.txt, EastAsianWidth.txt, CaseFolding.txt,
#   auxiliary/GraphemeBreakProperty.txt, emoji/emoji-data.txt (all placed flat in <ucd-dir>)

import os
import re
import sys

LINE_WIDTH = 95

GRAPHEME_BREAKS = {
    "CR": "GraphemeBreakCr",
    "LF": "GraphemeBreakLf",
    "Control": "GraphemeBreakControl",
    "Extend": "GraphemeBreakExtend",
    "ZWJ": "GraphemeBreakZwj",
    "Regional_Indicator": "GraphemeBreakRegionalIndicator",
    "Prepend": "GraphemeBreakPrepend",
    "SpacingMark": "GraphemeBreakSpacingMark",
    "L": "GraphemeBreakL",
    "V": "GraphemeBreakV",
    "T": "GraphemeBreakT",
    "LV": "GraphemeBreakLv",
    "LVT": "GraphemeBreakLvt",
}


def read_lines(directory, name):
    with open(os.path.join(directory, name), encoding="utf-8") as file:
        for line in file:
            line = line.split("#", 1)[0].strip()
            if line:
                yield [field.strip() for field in line.split(";")]


def parse_range(text):
    if ".." in text:
        first, last = text.split("..")
        return int(first, 16), int(last, 16)
    return int(text, 16), int(text, 16)


def read_version(directory):
    with open(os.path.join(directory, "EastAsianWidth.txt"), encoding="utf-8") as file:
        match = re.search(r"EastAsianWidth-(\d+\.\d+\.\d+)", file.readline())
    if not match:
        sys.exit("cannot find the unicode version in EastAsianWidth.txt")
    return match.group(1)


def read_categories(directory):
    # returns {code point: general category}, expanding <..., First>/<..., Last> pairs.
    categories = {}
    first = None
    for fields in read_lines(directory, "UnicodeData.txt"):
        point, name, category = int(fields[0], 16), fields[1], fields[2]
        if name.endswith(", First>"):
            first = point
            continue
        if name.endswith(", Last>"):
            for code in range(first, point + 1):
                categories[code] = category
            continue
        categories[point] = category
    return categories


def build_widths(directory, categories):
    widths = {}
    for fields in read_lines(directory, "EastAsianWidth.txt"):
        if fields[1] in ("W", "F"):
            first, last = parse_range(fields[0])
            for code in range(first, last + 1):
                widths[code] = 2

    # zero width wins over wide (for example combining ideographic tone marks).
    zero = [code for code, category in categories.items() if category in ("Mn", "Me", "Cf")]
    zero += list(range(0x1160, 0x1200)) + list(range(0xD7B0, 0xD800)) + [0x200B]
    for code in zero:
        widths[code] = 0

    # the soft hyphen is visible in terminals.
    widths.pop(0x00AD, None)
    return widths


def build_property(directory, name, values):
    table = {}
    for fields in read_lines(directory, name):
        if fields[1] in values:
            first, last = parse_range(fields[0])
            for code in range(first, last + 1):
                table[code] = values[fields[1]]
    return table


def build_folds(directory):
    folds = []
    for fields in read_lines(directory, "CaseFolding.txt"):
        if fields[1] in ("C", "S"):
            folds.append((int(fields[0], 16), int(fields[2], 16)))
    return sorted(folds)


def to_ranges(table):
    # merges consecutive code points with equal values into inclusive ranges.
    ranges = []
    for code in sorted(table):
        value = table[code]
        if ranges and ranges[-1][1] == code - 1 and ranges[-1][2] == value:
            ranges[-1][1] = code
        else:
            ranges.append([code, code, value])
    return ranges


def emit_entries(entries):
    # packs entries onto lines no wider than LINE_WIDTH, without a trailing comma.
    lines = []
    current = " "
    for index, entry in enumerate(entries):
        piece = " " + entry + ("," if index < len(entries) - 1 else "")
        if len(current) + len(piece) > LINE_WIDTH:
            lines.append(current)
            current = " "
        current += piece
    lines.append(current)
    return "\n".join(lines)


def emit_ranges(name, count_name, ranges):
    entries = ["{0x%04X, 0x%04X, %s}" % (first, last, value) for first, last, value in ranges]
    return (
        f"const UnicodeRange {name}[] = {{\n{emit_entries(entries)}\n}};\n\n"
        f"const u32 {count_name} = {len(ranges)};\n"
    )


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: tools/unicodedata.py <ucd-dir> > src/unicodedata.c")

    directory = sys.argv[1]
    version = read_version(directory)
    categories = read_categories(directory)
    widths = to_ranges(build_widths(directory, categories))
    breaks = to_ranges(build_property(directory, "GraphemeBreakProperty.txt", GRAPHEME_BREAKS))
    pictographics = build_property(directory, "emoji-data.txt", {"Extended_Pictographic": 1})
    pictographics = to_ranges(pictographics)
    folds = build_folds(directory)
    fold_entries = ["{0x%04X, 0x%04X}" % fold for fold in folds]

    sys.stdout.write(
        "// generated by tools/unicodedata.py.\n"
        "// do not edit by hand.\n"
        '#include "unicodedata.h"\n\n'
        f'const char unicodeVersion[] = "{version}";\n\n'
        + emit_ranges("unicodeWidths", "unicodeWidthCount", widths)
        + "\n"
        + emit_ranges("unicodeGraphemeBreaks", "unicodeGraphemeBreakCount", breaks)
        + "\n"
        + emit_ranges("unicodePictographics", "unicodePictographicCount", pictographics)
        + "\n"
        + f"const UnicodeFold unicodeFolds[] = {{\n{emit_entries(fold_entries)}\n}};\n\n"
        + f"const u32 unicodeFoldCount = {len(folds)};\n"
    )


main()
