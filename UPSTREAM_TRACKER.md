# Upstream Tracker

Tracks cherry-picks, skips, and review state against [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) (`upstream/master`).

**Last checked:** 2026-04-15
**Last upstream commit reviewed:** `cced777` — feat: add orientation-aware popups for reader activities (#1428)
**Upstream baseline:** everything up to and including `e6c6e72` (1.2.0 RC) has been reviewed.

---

## How to use

1. `git fetch upstream`
2. `git log --oneline upstream/master cced777..upstream/master` — shows new commits since last check
3. Evaluate each commit, add to Ported / Skipped / TODO below
4. Update "Last checked" date and "Last upstream commit reviewed" hash

---

## Ported (upstream hash → our hash)

Post-1.2.0 RC commits:

| Upstream | Ours | Description |
|----------|------|-------------|
| `9bc5111` | `4fef423` | fix: increase loadable epub size (vectors→deques) (#1638) |
| `104f391` | `a3cfdb7` | fix: two small memory leaks (#1628) |
| `83cd96b` | via `6b6e9ab` | fix: JPEGDEC wild pointer crash (#1627) |
| `5c12f2f` | `0fa3d3f` | fix: avoid skipping chapter after screenshot (#1625) |
| `9c11f3e` | `da35021` | feat: manual screen refresh on power button short press (#1626) |
| `ed0811c` | `4a2e2e1` | fix: first wifi connection attempt failing (#1521) |
| `405ce0c` | `5b1deaf` | feat: cover+custom sleep shows cover only when reading (#1256) |
| `c656673` | `bf51c91` | refactor: logPrintf and predefined log level strings (#1546) |
| `b898d53` | `6b6e9ab` | chore: drop JPEGDEC patch in favour of upstream fix (#1465) |
| `1398aeb` | via `6f6b7b7` | fix: differential rounding for inter-glyph spacing (#1413) |
| `d29b8ee` | `3b220d4` | feat: forward button at end of book goes home (#1425) |

Pre-1.2.0 RC (batch ports):

| Upstream | Ours | Description |
|----------|------|-------------|
| `526c8a5` | `3abbffc` | fix: OFW sleep routine (#1298) |
| `243ae8b` / `18b36ef` | `3abbffc` | feat: crash report to SD (#1453, #1145) |
| `ceb6acc` | `3abbffc` | fix: CSS display:none support (#1443) |
| `1df543d` | `f8bb0b0` | perf: eliminate per-pixel image rendering overheads (#1293) |
| `0c9e8b3` | via `e6c6e72` | fix: prewarm perf with many styles (#1451) |
| `7d56810` | `0db0e6a` | feat: integrated epub optimizer (#1224) |
| `5349e81` | via `e6c6e72` | feat: display file extensions in browser (#1019) |

---

## Skipped (with reason)

| Upstream | Description | Reason |
|----------|-------------|--------|
| `9b38851` | feat: initial X3 support (#875) | X3-only hardware |
| `825ef56` | feat: X3 grayscale antialiasing (#1607) | X3-only hardware |
| `6cd19f5` | fix: epub images not rendering on X3 (#1572) | X3-only hardware |
| `1bd7a1d` | refactor: deduplicate battery drawing + Lyra charging (#1437) | Lyra-specific; we have own battery code |
| `14ec53a` | feat: Slovenian translation (#1551) | Translation — grab in batch if needed |
| `cff3e12` | fix: Ukrainian translation update (#1585) | Translation |
| `fa3c7d9` | fix: Russian auto-turn translations (#1566) | Translation |
| `cc23aaa` | docs: README firmware flashing (#1654) | Docs — not applicable to DX34 |
| `11984f8` | refactor: C++20 `requires` in ActivityResult (#1420) | Risky refactor, no functional change |
| `f429f90` | refactor: default member initializers JpegContext/PngContext (#1435) | Minor refactor, low value |
| `23aad21` | refactor: removed redundant FsFile close() (#1434) | Minor cleanup, low value |
| `5ba8529` | refactor: deduplicated BMP header writing in Xtc (#1439) | Minor refactor |

---

## TODO — Not yet ported, worth considering

| Upstream | Description | Notes |
|----------|-------------|-------|
| `cced777` | feat: orientation-aware popups (#1428) | We have orientation code but not the popup positioning fix. Useful if users read in landscape. |
| `075ad7d` | fix: font metrics for combining mark positioning (#1310) | We use ChareInk only; check if diacritics render badly. Low priority unless user reports. |
| `4e9c7a7` | feat: full path bar in file browser (#1411) | Nice QoL — shows breadcrumb path. Not urgent. |
| `05f8e6e` | fix: webserver /delete backward compat (#1475) | We have `/delete` endpoint; check if clients depend on old format. |
| `fa2a3d2` | feat: OPDS search + next/prev pagination (#1462) | We have OPDS but no search. Useful feature if users use OPDS catalogs. |
| `8d6b35b` | fix: back navigation from BMPViewer (#1597) | We may not have BMPViewer at all — verify before porting. |
| `b3b43bb` | refactor: RAII ZipFile (#1433) | Our `dcc32ee` added RAII in other places; ZipFile may still lack it. Worth reviewing. |
| `1c13331` | fix: hyphenation for ISO 639-2 language codes (#1461) | Some ePubs use 3-letter lang codes. Low priority unless reported. |
