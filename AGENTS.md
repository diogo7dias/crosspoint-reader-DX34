# DX34 — Claude working notes

ESP32-C3 e-ink ebook reader firmware (PlatformIO + Arduino, `-fno-exceptions`,
~320 KB usable SRAM). This file is auto-read by Claude Code; it captures how to
build/test/flash and the in-progress memory-hardening work so any session can
continue it.

## Build / test / flash

```bash
git submodule update --init          # open-x4-sdk (required; build fails without it)
pio run -e default                   # compile firmware for esp32-c3 (CI-style verify)
pio run -t upload                    # build + flash  (MUST be on a machine with the device on USB)
pio device monitor -b 115200         # serial log
```

- **Flashing only works where the device is plugged in (the Mac), not on a
  remote build server.** A remote box can still run `pio run -e default` to
  verify compilation — always do that after firmware changes.
- Host unit tests run per environment with a matching filter (running a whole
  env runs every test under the wrong shadow headers and mislinks):
  - `pio test -e test_host -f test_memory_policy`
  - `pio test -e test_sim_zip -f test_reader_sim_zip`
  - `pio test -e test_sim_parse -f test_reader_sim_parse`
  - Some sim envs need a local `book.epub` fixture that is not in the repo;
    those runtime cases fail with "read 0 bytes" on a fresh checkout — that is
    environmental, not a regression. Compilation + the fragmentation/logic
    tests still validate.

## Toolchain gotcha (important)

The framework compiler is **GCC 8** (arduino-esp32 ~2.0.x) even though the build
uses `-std=gnu++2a`. C++20 trailing `requires` clauses and traits like
`std::is_unbounded_array_v` do **not** compile. Use C++11/14 SFINAE
(`std::enable_if` + `is_array`/`extent`/`remove_extent`) for template helpers.
Host test envs use a newer GCC, so **host builds will NOT catch this** — always
confirm template/header changes with `pio run -e default`.

## Memory-hardening conventions (follow these for any allocation)

Goal: be impenetrable against memory crashes. A failed `new`/STL grow under
`-fno-exceptions` calls `abort()` → device reset, bypassing recovery.

- **Allocation seam — `lib/Memory/Memory.h`.** All heap allocation goes through:
  - `makeUniqueNoThrow<T>()` / `makeUniqueNoThrow<T[]>(n)` for C++ objects/arrays.
  - `crosspoint::mem::tryMalloc/tryCalloc/tryRealloc` + `CMallocPtr<T>` for
    C-interop buffers (the only sanctioned `std::malloc` callers).
  - **Raw `new` (non-nothrow) and bare `malloc/calloc/realloc` are banned** in
    our code, enforced by `scripts/check_alloc_seam.py` — a ratcheting guard
    (baseline `scripts/alloc_seam_baseline.json`, currently **0**) wired as a
    `pre:` build hook. Any reintroduction fails the build. Exempt a genuinely
    necessary C-ownership site with a trailing `// alloc-ok`. The guard ignores
    comments, string literals, and vendored trees (expat/miniz/picojpeg/uzlib).
- **Activity creation is null-safe via one choke point.** `enterNewActivity`
  (main.cpp) routes a null activity into its OOM recovery ladder
  (silent-restart → OOM screen → reboot). `enterSubActivity` delegates to it.
  Create activities with `new (std::nothrow)` and pass the (possibly null)
  pointer in — do not hand-roll the OOM handling.
- **OOM new-handler — `crosspoint::mem::installOomHandler()`** (MemoryPolicy.h),
  installed at boot + re-armed each loop tick. On any `operator new`/STL grow
  failure it sheds caches once (`shedUnderPressure`) and lets operator new
  retry; complements the C-level `heap_caps_register_failed_alloc_callback`
  (`onHeapAllocFailed`) which covers raw malloc. Register droppable caches with
  `registerShedEvictor` — but ONLY SafeAnywhere (alloc-free, lock-free, safe
  from any task) ones; currently just the font cache. Page/CSS caches are
  dropped cooperatively via the MemoryPolicy recovery ladder, not the shed net.
- **Probe before a known big STL grow** with `crosspoint::mem::roomToGrow(bytes)`
  on hot layout paths.
- **Prefer heap for large buffers**, not the stack — fixed FreeRTOS task stacks
  overflow silently.

## Current state & how to continue

Done and on `main` (all on-target build-verified):
1. Allocation seam + ratcheting guard; all `new`/`malloc` migrated (baseline 0).
2. ZipFile dual-alloc leak fixed (RAII).
3. OOM new-handler net for STL/`operator new`.
4. Stack pass v1: 4 KB QR download buffer moved off-stack; loop-task stack
   high-water-mark added to the periodic `[MEM]` serial log.

**Next step (do this on the Mac, where you can flash):**
1. `pio run -t upload` then `pio device monitor -b 115200`.
2. Use the device ~1 min (open a book, turn pages, settings, QR share) and read
   the smallest `Loop stack free: X bytes` value on the `[MEM]` serial lines.
3. Decide stack pass v2 from that number:
   - `> 1500 B` → stack is healthy, no further buffer moves needed.
   - `500–1500 B` → watch.
   - `< 500 B` → move the next large stack locals off-heap, starting with
     `lib/PngToBmpConverter/PngToBmpConverter.cpp` (`readBuf[2048]` +
     `palette[256*3]`, ~2.8 KB combined), then the 1 KB locals
     (MyLibraryActivity, OtaUpdater, CrossPointWebServer, Txt.cpp).

Then optionally:
- **Widen the shed net**: let the OOM handler free more than the font cache via
  a *deferred cooperative* page-cache eviction (handler sets a flag; the loop
  task does the heavier drop in a safe context). Raises the true-exhaustion
  floor. Needs on-device testing — do it where you can flash.
- Otherwise memory-hardening is substantially complete; the remaining
  unrecoverable case is genuine total heap exhaustion (cannot be avoided
  without C++ exceptions).

## Commit conventions

Conventional Commits. End commit messages with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
Solo-dev project: no spec/plan/RFC files — chat + git history + this file are
the source of truth.
