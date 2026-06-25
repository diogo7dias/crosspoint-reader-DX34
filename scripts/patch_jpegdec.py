"""
PlatformIO pre-build script: patch JPEGDEC for the MCU_SKIP bugs in
JPEGDecodeMCU_P that surface when EIGHT_BIT_GRAYSCALE decodes a 3-component
progressive JPEG (each Y MCU drags two MCU_SKIP calls behind it for Cb/Cr).

Two independent defects, two fixes (mirrors upstream's 0001 + 0002 patches,
applied inline so we do not depend on the JPEGDEC libdep keeping its `.git`):

  1. Wild-pointer store-fault (CRASH).
     pMCU = &sMCUs[iMCU & 0xffffff].  When iMCU is MCU_SKIP (-8) the bitmask
     yields index 0xFFFFF8 (~33 MB past the sMCUs array); the AC decode loop
     writes through it and store-faults.
     Fix: redirect pMCU to sMCUs[0] when iMCU < 0.

  2. DC clobber (CORRUPTION, not a crash).
     With pMCU redirected to sMCUs[0], the two DC writes in JPEGDecodeMCU_P
     (`pMCU[0] |= iPositive` and `pMCU[0] = *iDCPredictor`) are still executed
     during the Cb/Cr MCU_SKIP passes and overwrite the just-decoded Y DC ->
     garbage pixels.  The *baseline* decoder JPEGDecodeMCU already guards these
     with `if (iMCU >= 0)`, but the *progressive* JPEGDecodeMCU_P does not.
     Fix: guard both progressive DC writes with `if (iMCU >= 0)`.

Without fix 2 the converter had to skip progressive JPEGs entirely (blank
covers).  With both fixes progressive decodes DC-only cleanly.

Each fix is keyed by its own marker and applied at most once — safe to run on
every build.
"""

Import("env")
import os


def patch_jpegdec(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        jpeg_inl = os.path.join(libdeps_dir, env_dir, "JPEGDEC", "src", "jpeg.inl")
        if os.path.isfile(jpeg_inl):
            _patch_file(jpeg_inl)


# (marker, old, new) — marker absence guards idempotency; old absence warns.
_PATCHES = [
    # 1. Redirect pMCU to sMCUs[0] on MCU_SKIP (wild-pointer store-fault).
    (
        "// CrossPoint patch: safe pMCU for MCU_SKIP",
        "    signed short *pMCU = &pJPEG->sMCUs[iMCU & 0xffffff];",
        "    // CrossPoint patch: safe pMCU for MCU_SKIP\n"
        "    signed short *pMCU = (iMCU < 0) ? pJPEG->sMCUs\n"
        "                                     : &pJPEG->sMCUs[iMCU & 0xffffff];",
    ),
    # 2a. Guard the successive-approximation DC write against MCU_SKIP.
    (
        "// CrossPoint patch: guard pMCU[0] DC write vs MCU_SKIP",
        "                pMCU[0] |= iPositive;",
        "                // CrossPoint patch: guard pMCU[0] DC write vs MCU_SKIP\n"
        "                if (iMCU >= 0)\n"
        "                    pMCU[0] |= iPositive;",
    ),
    # 2b. Guard the normal DC store against MCU_SKIP.  Anchored on the preceding
    #     `}` so we only hit the JPEGDecodeMCU_P copy, not the already-guarded
    #     baseline JPEGDecodeMCU copy (which is preceded by `if (iMCU >= 0)`).
    (
        "// CrossPoint patch: guard pMCU[0] DC store vs MCU_SKIP",
        "        }\n        pMCU[0] = (short)*iDCPredictor; // store in MCU[0]",
        "        }\n"
        "        // CrossPoint patch: guard pMCU[0] DC store vs MCU_SKIP\n"
        "        if (iMCU >= 0)\n"
        "            pMCU[0] = (short)*iDCPredictor; // store in MCU[0]",
    ),
]


def _patch_file(filepath):
    with open(filepath, "r") as f:
        content = f.read()

    changed = False
    for marker, old, new in _PATCHES:
        if marker in content:
            continue  # already applied
        if old not in content:
            print(
                "WARNING: JPEGDEC patch target not found in %s (marker: %s) "
                "— library may have been updated" % (filepath, marker)
            )
            continue
        content = content.replace(old, new, 1)
        changed = True
        print("Patched JPEGDEC (%s): %s" % (marker, filepath))

    if changed:
        with open(filepath, "w") as f:
            f.write(content)


# Run immediately at script import time (before compilation).
patch_jpegdec(env)
