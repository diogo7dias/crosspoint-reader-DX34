#!/usr/bin/env python3
"""Resize CrossPoint/Lector .pxc sleep wallpapers between panel ratios.

A .pxc is the device's packed sleep-wallpaper format:
  bytes 0-1  width   (uint16 LE)
  bytes 2-3  height  (uint16 LE)
  body       2 bits/pixel, MSB-first, stride = (width + 3) // 4 bytes/row
  pixel level 0..3 maps to gray {0, 85, 170, 255} (0 = black, 3 = white)

This mirrors PxcRenderer.cpp (decode) and SleepConverterPage.html encodePxc
(encode: Floyd-Steinberg dither to the 4-level palette, same thresholds).

Typical use: an X4 card (480x800) moved to an X3 (528x792) can't reuse its
.pxc wallpapers (the device size-guard rejects a mismatched panel). This
re-fits each wallpaper to the new panel so it displays natively again.

Because a .pxc is already quantised to 4 gray levels, re-fitting decodes to
gray, resamples (Lanczos), then re-dithers with the SAME Floyd-Steinberg /
palette the on-device web converter uses, so output looks native.

    python3 pxc_resize.py ~/Downloads/sleep -o ~/Downloads/sleep_x3
    python3 pxc_resize.py wall.pxc --to 528x792 --mode cover --preview
"""

import argparse
import os
import sys

import numpy as np
from PIL import Image

PALETTE_GRAY = [0, 85, 170, 255]  # 2-bit level -> gray, matches the device


def decode_pxc(path):
    """Return (gray HxW float32 in 0..255, width, height) for a .pxc file."""
    data = open(path, "rb").read()
    if len(data) < 4:
        raise ValueError("file too short for a .pxc header")
    w = data[0] | (data[1] << 8)
    h = data[2] | (data[3] << 8)
    stride = (w + 3) // 4
    expect = 4 + stride * h
    if len(data) != expect:
        raise ValueError(f"size mismatch: header {w}x{h} expects {expect} bytes, got {len(data)}")
    body = np.frombuffer(data, dtype=np.uint8, count=stride * h, offset=4).reshape(h, stride)
    # Unpack 4 pixels/byte, MSB-first: col&3==0 -> bits 7-6, ==1 -> 5-4, ...
    cols4 = stride * 4
    full = np.empty((h, cols4), dtype=np.uint8)
    for k in range(4):
        full[:, k::4] = (body >> (6 - k * 2)) & 0x03
    levels = full[:, :w]
    gray = levels.astype(np.float32) * 85.0
    return gray, w, h


def _resize_gray(gray, tw, th, mode, pad, resample):
    """Resample a gray HxW array to (th, tw) using the chosen fit mode."""
    src = Image.fromarray(np.clip(gray, 0, 255).astype(np.uint8), "L")
    sw, sh = src.width, src.height
    bg = 255 if pad == "white" else 0
    if mode == "pad":
        # No scaling at all: place the source 1:1, centred. Pad the axis that is
        # smaller than the panel, centre-crop the axis that is larger. For X4->X3
        # this is +24px side bars and a 4px top/bottom trim - pixel-identical
        # content, so it keeps the exact crispness of the original.
        canvas = Image.new("L", (tw, th), bg)
        left, top = (tw - sw) // 2, (th - sh) // 2
        cl, ct = max(0, -left), max(0, -top)
        cropped = src.crop((cl, ct, cl + min(sw, tw), ct + min(sh, th)))
        canvas.paste(cropped, (max(0, left), max(0, top)))
        return canvas
    if mode == "stretch":
        return src.resize((tw, th), resample)
    if mode == "cover":
        scale = max(tw / sw, th / sh)
        nw, nh = max(1, round(sw * scale)), max(1, round(sh * scale))
        r = src.resize((nw, nh), resample)
        left = (nw - tw) // 2
        top = (nh - th) // 2
        return r.crop((left, top, left + tw, top + th))
    # fit / contain: scale to fit, pad the remainder
    scale = min(tw / sw, th / sh)
    nw, nh = max(1, round(sw * scale)), max(1, round(sh * scale))
    r = src.resize((nw, nh), resample)
    bg = 255 if pad == "white" else 0
    canvas = Image.new("L", (tw, th), bg)
    canvas.paste(r, ((tw - nw) // 2, (th - nh) // 2))
    return canvas


def _quantize_none(g):
    """Threshold-quantise gray -> level index 0..3, matching the device encoder
    (SleepConverterPage.html quantPalette: >=212->3, >=127->2, >=42->1, else 0).
    Lossless for input already snapped to {0,85,170,255}."""
    idx = np.zeros(g.shape, dtype=np.uint8)
    idx[g >= 42] = 1
    idx[g >= 127] = 2
    idx[g >= 212] = 3
    return idx


def _dither_to_levels(limg, dither):
    """Quantise an 'L' image to level indices 0..3 (device palette 0/85/170/255).

    Uses explicit device-matching thresholds rather than PIL's palette quantiser:
    a padded 256-entry palette makes PIL map black pixels ambiguously and
    corrupts line art. Threshold quantisation is exact and lossless for input
    already at the 4 levels (the no-resample / nearest paths)."""
    g = np.asarray(limg, dtype=np.int16)
    if dither == Image.Dither.NONE:
        return _quantize_none(g)
    # Floyd-Steinberg error diffusion to the 4-level palette (encoder weights
    # 7/3/5/1). Row-sequential; only used by --resample smooth.
    buf = g.astype(np.float32)
    h, w = buf.shape
    idx = np.zeros((h, w), dtype=np.uint8)
    levels = np.array([0.0, 85.0, 170.0, 255.0], dtype=np.float32)
    for y in range(h):
        row = buf[y]
        for x in range(w):
            old = row[x]
            q = 3 if old >= 212 else 2 if old >= 127 else 1 if old >= 42 else 0
            idx[y, x] = q
            err = old - levels[q]
            if x + 1 < w:
                row[x + 1] += err * 7 / 16
            if y + 1 < h:
                nxt = buf[y + 1]
                if x > 0:
                    nxt[x - 1] += err * 3 / 16
                nxt[x] += err * 5 / 16
                if x + 1 < w:
                    nxt[x + 1] += err * 1 / 16
    return idx


def encode_pxc(gray, tw, th, mode, pad, resample, dither, invert=False):
    """Return .pxc bytes for a gray HxW array re-fitted to tw x th.

    invert=True flips the 2-bit level (level' = 3 - level) so the wallpaper
    displays correctly on a panel whose grayscale polarity is inverted (the
    X3 UC81xx controller vs the X4 SSD1677 the format was authored for).
    """
    limg = _resize_gray(gray, tw, th, mode, pad, resample)
    idx = _dither_to_levels(limg, dither)  # (th, tw) indices 0..3
    if invert:
        idx = 3 - idx
    stride = (tw + 3) // 4
    padw = stride * 4
    if padw > tw:
        idx = np.pad(idx, ((0, 0), (0, padw - tw)))
    packed = (idx[:, 0::4] << 6) | (idx[:, 1::4] << 4) | (idx[:, 2::4] << 2) | idx[:, 3::4]
    out = bytearray(4 + stride * th)
    out[0] = tw & 0xFF
    out[1] = (tw >> 8) & 0xFF
    out[2] = th & 0xFF
    out[3] = (th >> 8) & 0xFF
    out[4:] = packed.astype(np.uint8).tobytes()
    return bytes(out), limg, idx


def iter_pxc(inputs, recursive):
    for inp in inputs:
        if os.path.isfile(inp):
            if inp.lower().endswith(".pxc"):
                yield inp
        elif os.path.isdir(inp):
            if recursive:
                for root, _, files in os.walk(inp):
                    for f in sorted(files):
                        if f.lower().endswith(".pxc"):
                            yield os.path.join(root, f)
            else:
                for f in sorted(os.listdir(inp)):
                    if f.lower().endswith(".pxc"):
                        yield os.path.join(inp, f)


def main():
    ap = argparse.ArgumentParser(description="Re-fit .pxc sleep wallpapers to a new panel ratio.")
    ap.add_argument("inputs", nargs="+", help="one or more .pxc files or directories")
    ap.add_argument("-o", "--out", help="output directory (default: <input>_x3 for a dir)")
    ap.add_argument("--to", default="528x792", help="target WxH (default 528x792 = X3)")
    ap.add_argument("--mode", default="cover", choices=["cover", "fit", "stretch", "pad"],
                    help="cover=fill+crop (default), fit=letterbox, stretch=distort, "
                         "pad=1:1 no-resample centre (crispest, keeps original detail)")
    ap.add_argument("--invert", action="store_true",
                    help="flip black/white (level 3-n) for panels with inverted grayscale "
                         "polarity, e.g. the X3 UC81xx controller")
    ap.add_argument("--pad", default="white", choices=["white", "black"], help="fit-mode pad color")
    ap.add_argument("--resample", default="nearest", choices=["nearest", "smooth"],
                    help="nearest=keep original dither, clean whites (default, best for the "
                         "already-dithered .pxc art); smooth=Lanczos+re-dither (better on photos, "
                         "muddies flat/line art)")
    ap.add_argument("--recursive", action="store_true", help="recurse into directories")
    ap.add_argument("--preview", action="store_true",
                    help="also write a <name>.preview.png (source vs converted) next to output")
    ap.add_argument("--overwrite", action="store_true", help="overwrite existing outputs")
    args = ap.parse_args()

    tw, th = (int(x) for x in args.to.lower().split("x"))
    if args.resample == "nearest":
        resample, dither = Image.NEAREST, Image.Dither.NONE
    else:
        resample, dither = Image.LANCZOS, Image.Dither.FLOYDSTEINBERG

    default_out = None
    if not args.out:
        first = args.inputs[0]
        if os.path.isdir(first):
            default_out = os.path.normpath(first) + "_x3"

    n_ok = n_skip = n_err = 0
    for path in iter_pxc(args.inputs, args.recursive):
        try:
            gray, sw, sh = decode_pxc(path)
        except Exception as e:  # noqa: BLE001 - report and continue the batch
            print(f"  ERR  {os.path.basename(path)}: {e}", file=sys.stderr)
            n_err += 1
            continue
        if (sw, sh) == (tw, th):
            print(f"  skip {os.path.basename(path)}: already {tw}x{th}")
            n_skip += 1
            continue

        out_bytes, limg, idx = encode_pxc(gray, tw, th, args.mode, args.pad, resample, dither, args.invert)

        outdir = args.out or default_out or os.path.dirname(path)
        os.makedirs(outdir, exist_ok=True)
        outpath = os.path.join(outdir, os.path.basename(path))
        if os.path.exists(outpath) and not args.overwrite:
            print(f"  skip {os.path.basename(path)}: exists (use --overwrite)")
            n_skip += 1
            continue
        with open(outpath, "wb") as f:
            f.write(out_bytes)

        if args.preview:
            src_prev = Image.fromarray(np.clip(gray, 0, 255).astype(np.uint8), "L").resize(
                (sw * th // sh, th), Image.NEAREST)
            conv_prev = Image.fromarray((idx * 85).astype(np.uint8), "L")
            gap = 12
            combo = Image.new("L", (src_prev.width + gap + conv_prev.width, th), 128)
            combo.paste(src_prev, (0, 0))
            combo.paste(conv_prev, (src_prev.width + gap, (th - conv_prev.height) // 2))
            combo.save(os.path.join(outdir, os.path.splitext(os.path.basename(path))[0] + ".preview.png"))

        n_ok += 1
        print(f"   ok  {os.path.basename(path)}  {sw}x{sh} -> {tw}x{th} ({args.mode})")

    print(f"\ndone: {n_ok} converted, {n_skip} skipped, {n_err} errors -> "
          f"{args.out or default_out or '(alongside sources)'}")


if __name__ == "__main__":
    main()
