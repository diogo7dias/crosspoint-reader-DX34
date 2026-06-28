#!/usr/bin/env python3
"""CPBN (CrossPoint BiNary) SD-backed font blob serializer.

Pure stdlib. Emits the on-disk format consumed by the C++ reader in
lib/EpdFont/EpdBinFormat.h. The byte layout MUST stay byte-for-byte identical to
the packed `BlobHeader` struct there (38 bytes, little-endian) and the CRC32 must
match crosspoint::binfont::crc32 (zlib polynomial). See test_cpbn.py for the
contract checks pinning both sides to the 0xCBF43926 check vector.

Tier 1 file layout:
    [BlobHeader (38 B, packed, little-endian)] [bitmapBlob]

Tier 1 keeps the glyph/interval/group/kerning tables compiled into flash; only
the concatenated DEFLATE bitmap blob lives in this file. The header carries the
expected table counts so the firmware can reject a stale SD card (counts that no
longer match the flash tables) and a CRC32 over the blob to catch corruption.
"""
import struct
import zlib

MAGIC = 0x4E425043  # "CPBN" little-endian
BLOB_FORMAT_VERSION = 2
BITS_PER_PIXEL = 2

FLAG_TABLES_IN_FILE = 0x01  # reserved for Tier 2; 0 in Tier 1

# Mirrors the packed C++ BlobHeader field-for-field:
#   uint32 magic, uint8 version, uint8 bitsPerPixel, uint8 flags, uint8 variant,
#   uint16 sizePt, uint16 advanceY, int16 ascent, int16 descent, uint16 reserved0,
#   uint32 glyphCount, intervalCount, groupCount, bitmapBlobSize, blobCrc32
HEADER_FORMAT = "<IBBBBHHhhHIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
assert HEADER_SIZE == 38, HEADER_SIZE


def crc32(data: bytes) -> int:
    """CRC32 (zlib polynomial), matching crosspoint::binfont::crc32."""
    return zlib.crc32(data) & 0xFFFFFFFF


def pack_blob(blob: bytes, *, variant: int, size_pt: int, advance_y: int, ascent: int, descent: int,
              glyph_count: int, interval_count: int, group_count: int, flags: int = 0) -> bytes:
    """Serialize a Tier 1 CPBN file (header + bitmap blob) to bytes."""
    header = struct.pack(
        HEADER_FORMAT,
        MAGIC,
        BLOB_FORMAT_VERSION,
        BITS_PER_PIXEL,
        flags,
        variant,
        size_pt,
        advance_y,
        ascent,
        descent,
        0,  # reserved0
        glyph_count,
        interval_count,
        group_count,
        len(blob),
        crc32(blob),
    )
    return header + blob


def write_blob_file(path: str, blob: bytes, **kwargs) -> int:
    """Write a Tier 1 CPBN file to `path`. Returns the total byte count."""
    data = pack_blob(blob, **kwargs)
    with open(path, "wb") as f:
        f.write(data)
    return len(data)
