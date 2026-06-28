#!/usr/bin/env python3
"""Contract tests for the CPBN serializer (cpbn.py).

The byte layout here MUST stay identical to the C++ reader in
lib/EpdFont/EpdBinFormat.h (BlobHeader, 38 bytes, little-endian) and the CRC32
must match crosspoint::binfont::crc32 / Python zlib.crc32. The shared check
vector 0xCBF43926 ("123456789") is asserted on both sides.

Run:  python3 lib/EpdFont/scripts/test_cpbn.py
"""
import struct
import zlib

import cpbn


def test_header_size_is_38():
    assert cpbn.HEADER_SIZE == 38, cpbn.HEADER_SIZE


def test_constants_match_cpp():
    assert cpbn.MAGIC == 0x4E425043  # "CPBN" LE
    assert cpbn.BLOB_FORMAT_VERSION == 2
    assert cpbn.BITS_PER_PIXEL == 2


def test_crc32_matches_zlib_check_vector():
    assert cpbn.crc32(b"123456789") == 0xCBF43926


def test_pack_blob_layout_and_crc():
    blob = b"\xDE\xAD\xBE\xEF\x01\x02\x03"
    data = cpbn.pack_blob(
        blob,
        variant=0,
        size_pt=14,
        advance_y=18,
        ascent=14,
        descent=-4,
        glyph_count=12,
        interval_count=3,
        group_count=2,
    )
    # header + blob, nothing else
    assert len(data) == cpbn.HEADER_SIZE + len(blob)
    # blob bytes follow the header verbatim
    assert data[cpbn.HEADER_SIZE:] == blob
    # header fields land where the C++ struct expects them
    magic, ver, bpp, flags, variant = struct.unpack_from("<IBBBB", data, 0)
    assert magic == cpbn.MAGIC
    assert ver == cpbn.BLOB_FORMAT_VERSION
    assert bpp == cpbn.BITS_PER_PIXEL
    assert flags == 0  # Tier 1: tables in flash
    assert variant == 0
    (glyph_count,) = struct.unpack_from("<I", data, 18)  # offsetof(glyphCount)
    assert glyph_count == 12
    (blob_size,) = struct.unpack_from("<I", data, 30)  # offsetof(bitmapBlobSize)
    assert blob_size == len(blob)
    (crc,) = struct.unpack_from("<I", data, 34)  # offsetof(blobCrc32)
    assert crc == zlib.crc32(blob) & 0xFFFFFFFF


def test_empty_blob_roundtrips():
    data = cpbn.pack_blob(b"", variant=1, size_pt=10, advance_y=12, ascent=10, descent=-3,
                          glyph_count=0, interval_count=0, group_count=0)
    assert len(data) == cpbn.HEADER_SIZE
    (blob_size,) = struct.unpack_from("<I", data, 30)
    assert blob_size == 0
    (crc,) = struct.unpack_from("<I", data, 34)
    assert crc == zlib.crc32(b"") & 0xFFFFFFFF


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print(f"{len(tests)} cpbn serializer tests passed")


if __name__ == "__main__":
    main()
