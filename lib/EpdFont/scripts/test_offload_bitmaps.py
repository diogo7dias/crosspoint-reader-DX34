#!/usr/bin/env python3
"""Contract tests for the bitmap-offload header rewriter (offload_bitmaps.py).

offload_bitmaps.py post-processes a fontconvert-generated builtin-font header so
the large `<name>Bitmaps[N]` array compiles out under -DCROSSPOINT_SD_FONTS_SLIM
(the bitmap then streams from an SD `.bin`), while the in-flash default build is
byte-for-byte unchanged (SLIM macro undefined). It rewrites two spots:

  1. wraps the FIRST `static const uint8_t <name>Bitmaps[N] = { ... };` array in
     `#ifndef CROSSPOINT_SD_FONTS_SLIM ... #endif`;
  2. makes the EpdFontData struct's first field conditional
     (`#ifdef CROSSPOINT_SD_FONTS_SLIM nullptr #else <name>Bitmaps #endif`).

It must be idempotent (safe to re-run) and must migrate headers that still carry
the old single-flag `CROSSPOINT_SD_FONTS` pilot guard to the split `_SLIM` macro.

Run:  python3 lib/EpdFont/scripts/test_offload_bitmaps.py
"""
import offload_bitmaps as ob

CLEAN = """\
#pragma once
#include "EpdFontData.h"

static const uint8_t foo_12_regularBitmaps[6] = {
    0x01, 0x02, 0x03,
    0x04, 0x05, 0x06,
};

static const uint16_t foo_12_regularGlyphs[] = {
    0, 1, 2,
};

static const EpdFontData foo_12_regular = {
    foo_12_regularBitmaps,
    foo_12_regularGlyphs,
    1,
    2,
    true,
};
"""

EXPECTED = """\
#pragma once
#include "EpdFontData.h"

#ifndef CROSSPOINT_SD_FONTS_SLIM
static const uint8_t foo_12_regularBitmaps[6] = {
    0x01, 0x02, 0x03,
    0x04, 0x05, 0x06,
};
#endif

static const uint16_t foo_12_regularGlyphs[] = {
    0, 1, 2,
};

static const EpdFontData foo_12_regular = {
#ifdef CROSSPOINT_SD_FONTS_SLIM
    nullptr,
#else
    foo_12_regularBitmaps,
#endif
    foo_12_regularGlyphs,
    1,
    2,
    true,
};
"""

# A header already carrying the OLD pilot guard (single CROSSPOINT_SD_FONTS flag),
# exactly as the bookerly_17_regular pilot header was hand-edited.
OLD_FLAG = """\
#pragma once
#include "EpdFontData.h"

#ifndef CROSSPOINT_SD_FONTS
static const uint8_t foo_12_regularBitmaps[6] = {
    0x01, 0x02, 0x03,
    0x04, 0x05, 0x06,
};
#endif

static const EpdFontData foo_12_regular = {
#ifdef CROSSPOINT_SD_FONTS
    nullptr,
#else
    foo_12_regularBitmaps,
#endif
    foo_12_regularGlyphs,
    1,
};
"""


def test_clean_header_gets_guards():
    assert ob.offload_header(CLEAN) == EXPECTED


def test_idempotent():
    once = ob.offload_header(CLEAN)
    twice = ob.offload_header(once)
    assert once == twice


def test_migrates_old_single_flag_to_slim():
    out = ob.offload_header(OLD_FLAG)
    assert "CROSSPOINT_SD_FONTS_SLIM" in out
    # No bare (non-_SLIM) occurrences of the old pilot flag should remain.
    assert "CROSSPOINT_SD_FONTS\n" not in out
    assert "#ifndef CROSSPOINT_SD_FONTS\n" not in out
    assert "#ifdef CROSSPOINT_SD_FONTS\n" not in out


def test_only_first_array_is_wrapped():
    out = ob.offload_header(CLEAN)
    # The Glyphs array must NOT be guarded — only the Bitmaps array moves to SD.
    assert "#ifndef CROSSPOINT_SD_FONTS_SLIM\nstatic const uint16_t foo_12_regularGlyphs" not in out
    assert out.count("#ifndef CROSSPOINT_SD_FONTS_SLIM") == 1


def test_struct_first_field_is_conditional():
    out = ob.offload_header(CLEAN)
    block = (
        "static const EpdFontData foo_12_regular = {\n"
        "#ifdef CROSSPOINT_SD_FONTS_SLIM\n"
        "    nullptr,\n"
        "#else\n"
        "    foo_12_regularBitmaps,\n"
        "#endif\n"
    )
    assert block in out


def test_unrelated_header_untouched():
    # A header with no Bitmaps array (e.g. a shared-tables file) is returned as-is.
    text = '#pragma once\nstatic const int8_t foo_regularKernValues[] = { -11, 28 };\n'
    assert ob.offload_header(text) == text


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print(f"{len(tests)} offload_bitmaps tests passed")


if __name__ == "__main__":
    main()
