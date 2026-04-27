#!/usr/bin/env python3
"""Post-process fontconvert.py output to share byte-identical arrays across sizes.

For each (family, weight) pair in the reader font collection, the arrays
`Intervals`, `KernLeftClasses`, `KernRightClasses`, and `LigaturePairs` are
driven by the source TTF's codepoint coverage and OpenType tables, not by
the rasterization ppem.  Across the 6 sizes of a given (family, weight) they
are typically byte-identical, so we extract them once into a shared header
and rewrite each per-size header to reference the shared symbols.

This runs as a second pass after `convert-builtin-fonts.sh` and is
idempotent: invoking it on already-deduped headers is a no-op because the
array bodies no longer exist in the per-size files.

Running `convert-builtin-fonts.sh` after edits overwrites the per-size
headers with full (un-deduped) versions, then this script is expected to be
invoked again.
"""

import glob
import os
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
FONTS_DIR = SCRIPT_DIR.parent / "builtinFonts"
SHARED_DIR = FONTS_DIR / "shared"

FAMILIES = [
    ("bookerly", ["regular", "bold", "italic"], [10, 12, 13, 14, 15, 16, 17]),
    ("chareink", ["regular", "bold", "italic"], [10, 12, 13, 14, 15, 16, 17]),
    ("vollkorn", ["regular", "bold", "italic"], [10, 12, 13, 14, 15, 16, 17]),
    ("bitter", ["regular", "bold", "italic"], [10, 12, 14, 16]),
    # Galmuri is Regular-only (italic/bold synthesized at draw time) and
    # covers the small pixel-font sizes.
    ("galmuri", ["regular"], [10, 11, 12, 14]),
]

# Array-name suffix -> (C type, shared symbol suffix).  The first entry of each
# tuple is the substring that follows the per-size symbol prefix in the source.
SHAREABLE_ARRAYS = [
    ("Intervals", "EpdUnicodeInterval"),
    ("KernLeftClasses", "EpdKernClassEntry"),
    ("KernRightClasses", "EpdKernClassEntry"),
    ("LigaturePairs", "EpdLigaturePair"),
]


def extract_array_block(content: str, symbol: str):
    """Return (full_match_text, body_text) for a `static const ... symbol[N] = { ... };` block.

    Returns (None, None) if the symbol is not defined in this content."""
    pat = re.compile(
        r"(static\s+const\s+\S+\s+"
        + re.escape(symbol)
        + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\n\};\s*)",
        re.DOTALL,
    )
    m = pat.search(content)
    if not m:
        return None, None
    return m.group(1), m.group(2)


def rewrite_per_size(path: Path, family: str, weight: str, shared_array_names: list):
    """Rewrite a per-size header to reference shared symbols and drop the shared array bodies."""
    content = path.read_text()
    modified = False
    per_size_prefix = f"{family}_{path.stem.split('_')[1]}_{weight}"  # e.g., bookerly_14_regular
    shared_prefix = f"{family}_{weight}"  # e.g., bookerly_regular
    shared_include = f'#include "shared/{shared_prefix}_tables.h"'

    # Remove each shared array definition from the body.
    for arr_suffix in shared_array_names:
        sym = f"{per_size_prefix}{arr_suffix}"
        full, _ = extract_array_block(content, sym)
        if full is not None:
            content = content.replace(full, "")
            modified = True

    if not modified:
        return False  # already deduped or no shareable arrays present

    # Replace references in the EpdFontData struct.
    for arr_suffix in shared_array_names:
        content = content.replace(
            f"{per_size_prefix}{arr_suffix}",
            f"{shared_prefix}{arr_suffix}",
        )

    # Insert the include once, right after the EpdFontData.h include.
    if shared_include not in content:
        content = content.replace(
            '#include "EpdFontData.h"',
            '#include "EpdFontData.h"\n' + shared_include,
            1,
        )

    # Collapse accidental triple blank lines into doubles for cleanliness.
    content = re.sub(r"\n{3,}", "\n\n", content)

    path.write_text(content)
    return True


def process_family_weight(family: str, weight: str, sizes: list):
    size_contents = {}
    for size in sizes:
        path = FONTS_DIR / f"{family}_{size}_{weight}.h"
        if not path.exists():
            print(f"{family} {weight}: missing {path.name}; skipping family", file=sys.stderr)
            return 0
        size_contents[size] = (path, path.read_text())

    shared_arrays = []  # (arr_suffix, full_definition_text, c_type)
    per_size_drop_names = []  # list of suffixes to drop from every size header

    for arr_suffix, c_type in SHAREABLE_ARRAYS:
        bodies = {}
        full_defs = {}
        for size, (path, content) in size_contents.items():
            sym = f"{family}_{size}_{weight}{arr_suffix}"
            full, body = extract_array_block(content, sym)
            if body is None:
                bodies[size] = None
            else:
                bodies[size] = body
                full_defs[size] = full

        # Skip if any size lacks the array (e.g., no kerning/ligatures).
        if any(b is None for b in bodies.values()):
            continue

        unique = {b for b in bodies.values()}
        if len(unique) != 1:
            print(
                f"  skip {arr_suffix}: differs across sizes ({len(unique)} unique); "
                f"keeping per-size",
                file=sys.stderr,
            )
            continue

        # Rename the symbol from `{family}_{size}_{weight}<suffix>` -> `{family}_{weight}<suffix>`.
        ref_size = next(iter(bodies))
        full_def = full_defs[ref_size]
        ref_sym = f"{family}_{ref_size}_{weight}{arr_suffix}"
        shared_sym = f"{family}_{weight}{arr_suffix}"
        shared_def = full_def.replace(ref_sym, shared_sym, 1)
        shared_arrays.append((arr_suffix, shared_def, c_type))
        per_size_drop_names.append(arr_suffix)

    if not shared_arrays:
        print(f"{family} {weight}: no shareable arrays; skipping", file=sys.stderr)
        return 0

    # Write the shared header.
    SHARED_DIR.mkdir(exist_ok=True)
    shared_path = SHARED_DIR / f"{family}_{weight}_tables.h"
    parts = [
        "/**",
        f" * Shared arrays across all sizes of {family} {weight}.",
        " * Generated by dedup-shared-tables.py; do not hand-edit.",
        " */",
        "#pragma once",
        '#include "../../EpdFontData.h"',
        "",
    ]
    for arr_suffix, shared_def, _c_type in shared_arrays:
        parts.append(shared_def.rstrip())
        parts.append("")
    shared_path.write_text("\n".join(parts).rstrip() + "\n")

    # Rewrite each per-size header.
    rewritten = 0
    for size, (path, _) in size_contents.items():
        if rewrite_per_size(path, family, weight, per_size_drop_names):
            rewritten += 1

    total_shared_bytes = 0
    for arr_suffix, shared_def, _ in shared_arrays:
        total_shared_bytes += len(shared_def)
    print(
        f"{family} {weight}: shared {len(shared_arrays)} arrays "
        f"({', '.join(n for n, _, _ in shared_arrays)}), "
        f"rewrote {rewritten}/{len(size_contents)} per-size headers",
        file=sys.stderr,
    )
    return rewritten


def main():
    for family, weights, sizes in FAMILIES:
        for weight in weights:
            process_family_weight(family, weight, sizes)
    print("\ndedup-shared-tables: done.", file=sys.stderr)


if __name__ == "__main__":
    main()
