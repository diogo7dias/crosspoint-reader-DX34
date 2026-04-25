# Web UI Redesign Plan — Brutalist 8-bit

Tracks the rollout of the brutalist 8-bit visual direction (started in
PR #76, HomePage only) across the rest of the device's web UI.

## Goal

Bring `FilesPage`, `SettingsPage`, and `FontsPage` to visual parity with
the redesigned `HomePage`, sharing a single design system so future
pages stay consistent without per-page CSS drift.

## Current web surface

| Route | File | Size (raw) | State |
| --- | --- | --- | --- |
| `GET /` | `HomePage.html` | 4.9 KB → 16.4 KB (PR #76) | Redesigned |
| `GET /files` | `FilesPage.html` | 157 KB | Old design |
| `GET /settings` | `SettingsPage.html` | 12 KB | Old design |
| `GET /fonts` | `FontsPage.html` | 31 KB | Old design |
| `GET /js/jszip.min.js` | static asset | — | n/a |

**JSON / op endpoints (no UI):** `/api/status`, `/api/files`,
`/api/settings` (GET + POST), `/api/fonts`, `/api/fonts/delete`,
`/upload`, `/mkdir`, `/rename`, `/move`, `/delete`, `/download`,
`/preview`. These do not need redesign work.

## Constraints (must respect)

- **Heap budget:** ESP32-C3 with web mode active drops free heap from
  ~88 KB idle to ~10 KB during request serving (Min Free observed
  2452 B). Any new asset must ship gzipped from flash, not allocated on
  heap. See PR #76 follow-up — a crash was seen with weak WiFi + low
  heap; redesigns must not worsen this.
- **Flash:** every redesigned page grows. PR #76 grew HomePage 4.9 →
  4.4 KB gzip. Track gzip size per phase, not raw.
- **No new PROGMEM strings per page** for shared chrome — share via
  Phase 0 instead.
- **No external runtime deps that break offline.** Google Fonts CDN is
  fine for browsers but must always have a `ui-monospace` fallback.
- **Keep all existing functionality.** File upload, multi-select,
  rename, move, delete, font upload + delete, settings POST — JS logic
  stays; only structure + CSS change.

## Phase 0 — Shared design system  (foundation)

Extract the brutalist primitives from `HomePage.html` into a single
shared CSS file served at `GET /css/brutalist.css`, gzipped from flash
the same way HTML pages are.

**Tokens** (CSS custom properties on `:root`):
- `--bg: #0d0d10`
- `--fg: #f5f5f5`
- `--accent: #e8e8e8`
- `--grid-line: rgba(255,255,255,0.06)`
- `--border: 1px dashed #f5f5f5`
- `--font-mono: "Space Mono", ui-monospace, Menlo, monospace`

**Components** (BEM-ish class names so pages can compose):
- `.bx-grid` — 48 px grid background
- `.bx-card` — dashed-border container
- `.bx-chip` — label-over-value status chip with overflow-wrap
- `.bx-tile` — numeral tile (uptime / heap / signal style)
- `.bx-nav-btn` — 56 px tap-target button with `[N]` keyboard hint
- `.bx-masthead` — wordmark + meta layout
- `.bx-row` — list row treatment for FilesPage
- `.bx-form-row` — label+control row for SettingsPage

**Reduced-motion + responsive breakpoints** baked into the CSS so each
page just composes classes.

**Build pipeline:** `tools/embed-html.py` (or whatever already
generates `HomePageHtml.generated.h`) extended to also generate
`BrutalistCss.generated.h`. New route added to `CrossPointWebServer`:

```cpp
server->on("/css/brutalist.css", HTTP_GET, [this] { handleBrutalistCss(); });
```

with `Cache-Control: public, max-age=86400` so the browser caches it
across page nav.

**HomePage retrofit:** delete the inlined CSS from `HomePage.html` and
replace with `<link rel="stylesheet" href="/css/brutalist.css">`. Net
HomePage size goes down. This is the validation that the shared file
works.

**Phase 0 PR exit criteria:**
- `/css/brutalist.css` route serves gzipped, correct MIME
  (`text/css`), correct cache header.
- HomePage renders identically before/after.
- Total flash delta ≤ 0 (the shared CSS replaces inlined HomePage CSS;
  other pages still use their old inlined styles for now).

## Phase 1 — SettingsPage redesign  (smallest, validates design system)

Why first (after Phase 0): smallest page, mostly forms — proves the
`bx-form-row` component works, low risk.

- Apply `<link rel="stylesheet" href="/css/brutalist.css">`.
- Replace existing chrome with `bx-masthead` + `bx-card`.
- Each setting row becomes `bx-form-row` (label left, control right,
  dashed underline).
- POST `/api/settings` JSON behaviour unchanged.

**Risks:** none significant.

## Phase 2 — FontsPage redesign

- Apply shared CSS.
- Each installed font → `bx-chip` with delete button.
- Upload widget → dashed-border drop zone using `bx-card`.
- POST `/upload`, POST `/api/fonts/delete` behaviour unchanged.

**Risks:** font upload is a multipart POST; do not regress streaming
behaviour. Keep the existing JS upload logic.

## Phase 3 — FilesPage redesign  (biggest, last)

Done last because it's the largest and most JS-heavy page.

- Apply shared CSS.
- File rows → `bx-row` (icon, name, size, mtime, actions).
- Toolbar → `bx-card` with action buttons in `bx-nav-btn` style.
- Drag/drop, multi-select, context menu — JS untouched, only CSS
  classes swapped.
- Breadcrumb + path bar styled with `bx-chip`.

**Risks:** highest. Two passes:
1. CSS swap only, structure unchanged — confirm everything still
   works.
2. Structure tweaks (row layout, toolbar reflow) once CSS is proven.

## Phase 4 — Audit  (after all pages land)

- Confirm every HTML page ships gzipped from flash.
- Confirm `/css/brutalist.css` and `/js/jszip.min.js` have
  `Cache-Control` headers.
- Measure free heap with web mode + page load on weak WiFi vs.
  pre-redesign baseline. Should not regress.
- Lighthouse / browser devtools sanity check on all four pages at
  320 / 375 / 820 / 1280 px.

## Open questions

1. Inline Space Mono as a self-hosted WOFF2 in flash, or keep Google
   Fonts CDN with `ui-monospace` fallback?  CDN is smaller flash but
   relies on browser internet; self-hosted means ~30 KB extra flash.
2. Should `/css/brutalist.css` be served minified or human-readable?
   Minified saves flash but makes on-device debugging harder.
3. Can we drop `jszip.min.js`?  If FilesPage redesign removes the
   bulk-download-as-zip feature, we save a static asset.

## Order of work / PRs

| # | PR | Branch | Depends on |
| --- | --- | --- | --- |
| 0 | Shared `/css/brutalist.css` + HomePage retrofit | `feat/web-brutalist-css` | PR #76 merged |
| 1 | SettingsPage redesign | `feat/web-settings-brutalist` | Phase 0 |
| 2 | FontsPage redesign | `feat/web-fonts-brutalist` | Phase 0 |
| 3 | FilesPage redesign — CSS swap | `feat/web-files-brutalist-css` | Phase 0 |
| 4 | FilesPage redesign — structure | `feat/web-files-brutalist-layout` | Phase 3 |
| 5 | Audit + cache headers | `chore/web-cache-audit` | All above |

Each PR ships independently. Phase 0 unblocks 1–3 in parallel.
