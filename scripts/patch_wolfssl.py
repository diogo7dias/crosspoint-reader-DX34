from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
#undef FP_MAX_BITS
#define FP_MAX_BITS 16384
"""


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")

    # espressif32 6.x (IDF 4.4 / Arduino core 2.x) injects -DHAVE_CONFIG_H into
    # every source build, which makes wolfSSL's autoconf-style sources try to
    # include a config.h that doesn't exist in the Arduino packaging. Drop an
    # empty stub next to user_settings.h (already on the include path) so the
    # include resolves; all real configuration comes from user_settings.h.
    # (Upstream builds on pioarduino/core 3.x, which doesn't define it.)
    config_stub = path.parent / "config.h"
    if not config_stub.exists():
        config_stub.write_text("/* empty stub: real config lives in user_settings.h (see patch_wolfssl.py) */\n")
        print(f"Stubbed wolfSSL config.h: {config_stub.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)
