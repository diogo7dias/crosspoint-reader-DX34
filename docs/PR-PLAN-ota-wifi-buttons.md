# PR Plan — Upstream OTA/WiFi Port + Button Detection Investigation

## Scope

Two related but independent workstreams bundled into one tracking doc:

1. **Upstream port** (branch `port/upstream-ota-wifi`, already pushed) — three high-value upstream commits adapted for DX34. Ready for review/flash once smoke-tested.
2. **Button detection flakiness investigation** — root-cause investigation for intermittent button miss-detection. Evidence-gathering phase, no code change yet.

---

## Workstream 1 — `port/upstream-ota-wifi`

### Branch
`port/upstream-ota-wifi` → 2 commits ahead of `main`.

### Commits ported

#### 1. `25157e4f` — `fix: Read GH release JSON as stream in OTA updater (#1810)` (upstream `aa7a31b3`)

**Problem upstream solved:** `/releases/latest` JSON ballooned past ~30 KB once release notes grew. Single-buffer ArduinoJson parse on ESP32-C3 ran out of heap → OTA check failure.

**What landed:**
- New `lib/JsonParser/StreamingJsonParser.{h,cpp}` — SAX-style incremental parser.
- New `lib/JsonParser/ReleaseJsonParser.{h,cpp}` — release-JSON-specific extractor (tag, firmware URL, size).
- `OtaUpdater::checkForUpdate()` rewritten to feed HTTP chunks into the streaming parser, drop ArduinoJson dependency, drop `local_buf` cleanup struct.
- Host unit tests for both parsers under `test/streaming_json_parser/` and `test/release_json_parser/` plus `run_*.sh` runners.

**DX34 adaptations preserved:**
- DX34 release URL (`diogo7dias/crosspoint-reader-DX34`).
- `CrossPoint-Mod-DX34-ESP32-` user-agent.
- `findSemverStart()` for `DX34-x.y.z` tag parsing.
- HTTP status-code check (`403/429 → RATE_LIMITED`, non-200 → `HTTP_ERROR`).
- `extern "C" esp_crt_bundle_attach` arduino-platform workaround retained (upstream switched to `<esp_crt_bundle.h>` include; DX34 toolchain may be older).
- `installUpdate(std::function<void()>)` callback kept (DX34's prior cleanup vs upstream's `void(*)(void*) + ctx`).

#### 2. `82e69c54` — `feat: self-heal from transient WiFi loss, add dBm indicator during WebServerActivity (#1780)` (upstream `adcd7961`)

**Problem upstream solved:** Brief router/STA blips kicked the file-transfer activity into `SHUTTING_DOWN` and left it hung. No on-device signal-strength readout — required serial.

**What landed:**
- WiFi state machine in `CrossPointWebServerActivity::loop()`: `consecutiveDisconnects` counter + `firstDisconnectAt` timestamp; activity stays alive while driver retries; only abandons (returns to network selection) after `WIFI_ABANDON_MS = 5 min`.
- 4-bar dBm indicator drawn top-right via `renderWifiIndicator(int subHeaderTop)`. Hysteresis (`barsForRssi`) avoids rapid screen redraw at threshold boundaries.
- Periodic RSSI logging when `< -75 dBm`.

**DX34 adaptations:**
- DX34 has no `UITheme::getInstance().getMetrics()` struct → `renderWifiIndicator` rewritten with hardcoded layout constants (`CONTENT_SIDE_PADDING = 12`, anchored to title row at `y=15`).
- DX34 has `onGoBack` (not upstream's `onGoHome`) → switched.
- DX34's existing "Reconnecting to Wi-Fi…" banner kept; condition switched from old `wifiDropped` flag to `consecutiveDisconnects > 0`.
- DX34's STA-only `WiFi.setAutoReconnect(true)` + heap diag log preserved in `CrossPointWebServer.cpp` (upstream made it unconditional and dropped the diag).

#### 3. `ba4a361d` — `fix: OTA on x3 and progress bar on x4 and x3 (#1805)` — **SKIPPED**

Reason: X3-specific (`skip_efuse_blk_check.c` + `bootloader_common_check_efuse_blk_validity` linker wrap). Remaining changes refactor the OTA progress callback API in the opposite direction from DX34's existing `std::function` migration. Net value for DX34: a small UX win (3-second hold on the "Update complete" screen before shutdown). Document as a candidate for separate later port if users report flashing-completion UX.

#### 4. `5717374e` — `feat(update): SD-card firmware update + X3 bootloader compatibility (#1786)` — **PARKED**

Too large for this PR (928 insertions, 13 files, new `SdFirmwareUpdateActivity`, `FirmwareFlasher`, `OtaBootSwitch`). Strip X3 bootloader compatibility, keep SD update path. Track separately.

### Risk surface

| Area | Likelihood of regression | Mitigation |
|---|---|---|
| OTA check (release JSON) | Low — new parser has unit tests, DX34 release shape matches assumption (`tag_name`, `assets[].name == "firmware.bin"`, `browser_download_url`, `size`) | Smoke test: trigger update check on device, confirm tag + URL parsed correctly |
| OTA install (binary download) | None — `installUpdate()` untouched | n/a |
| WiFi self-heal | Medium — new state machine; abandon timeout 5 min may surprise users on flaky networks | Smoke test: connect, kill router for 30 s, restore — should auto-recover; kill router for 6 min — should return to network selection |
| dBm indicator | Low — pure render addition, hardcoded layout may collide with existing top-right elements on STA mode | Visual check on real screen for both AP and STA modes |
| Heap diag log retained | None | n/a |

### Smoke-test plan (pre-merge)

| Test | Action | Pass criterion |
|---|---|---|
| Build | `pio run` | Compiles, links, no new warnings |
| OTA check warm | Settings → check for update | Toast shows current vs latest; no crash; serial shows `OTA: Found update: tag=… size=…` |
| OTA check rate-limited | Hammer endpoint to provoke 403/429 | Returns `RATE_LIMITED`, no crash |
| WiFi blip recovery | Start file-transfer, power-cycle router for ≤30 s | Banner appears, clears on reconnect, IP refreshed |
| WiFi sustained loss | Start file-transfer, kill router for ≥6 min | Activity exits cleanly to network selection |
| dBm indicator | Connect at varying distances | Bars track signal, no flicker at threshold |
| Memory | Free heap before/after OTA check | No regression vs current main |

### Merge strategy

Rebase + FF onto `main` after smoke tests pass (matches existing `(#NN)` linear history).

---

## Workstream 2 — Button detection flakiness

### Symptom
User reports buttons "not always detected." Intermittent miss-detection. Suspect either firmware or device.

### Verdict so far: **mostly firmware, with hardware contributing**

### Architecture (relevant facts)
- 6 directional buttons share **2 ADC pins** (GPIO1, GPIO2) via resistor ladder. Each button presents a distinct voltage.
- Power button on GPIO3 — digital, active LOW, **also a strapping pin** (boot-mode select; affects boot only, not runtime).
- All input handling lives in submodule `open-x4-sdk` (fork: `diogo7dias/community-sdk`), `libs/hardware/InputManager/src/InputManager.cpp`. Unmodified from upstream-baseline.
- `InputManager::getState()` takes **one** `analogRead()` per pin per call.
- `InputManager::update()` invoked once per `loop()` iteration in `src/main.cpp:967`.
- `DEBOUNCE_DELAY = 5 ms` (`InputManager.h:112`) — must remain stable for >5 ms before commit.
- ADC quiescent threshold `ADC_NO_BUTTON = 3800`; ranges have generous margins vs recorded values. Boundary collision unlikely under nominal noise.

### Suspect ranking

| # | Hypothesis | Likelihood | Why |
|---|---|---|---|
| 1 | No ADC oversampling + 5 ms debounce too short for ESP32-C3 noise | **High** | ESP32-C3 ADC has ±50-100 LSB jitter; single sample at edges of resistor-ladder window can land in wrong bucket; flickering reads keep resetting `lastDebounceTime` so press never commits |
| 2 | Variable loop cadence | **Medium-High** | `gpio.update()` ticks once per main loop. Heavy frames (EPD redraw, file I/O, OTA, large JSON) push iteration to 100-300 ms. Tap shorter than that is lost outright. Existing commit `4a210823 "Manually trigger GPIO update in File Browser mode"` is prior evidence this category has bitten DX34 before |
| 3 | Power button strapping-pin interference (GPIO3) | Low | Boot-time only; does not explain runtime flakiness |
| 4 | Mechanical contact wear / device defect | Low | Cannot rule out without per-button stats; but same SDK serves many devices, so widespread firmware issue more likely than per-device hardware |

### Pre-fix evidence to gather (Phase 1)

Goal: distinguish hypothesis 1 vs 2 vs 4 before changing anything. Three artifacts to add temporarily, then revert after capture.

#### Probe A — Raw ADC trace
Add to `InputManager::update()` (open-x4-sdk fork):
```cpp
#if BUTTON_DEBUG_TRACE
static unsigned long lastTrace = 0;
const unsigned long now = millis();
if (now - lastTrace >= 20) {  // 50 Hz max
  Serial.printf("[BTN] adc1=%4d adc2=%4d gpio3=%d state=0x%02X commit=0x%02X dt=%lums\n",
                adcValue1, adcValue2, digitalRead(POWER_BUTTON_PIN),
                state, currentState, now - lastTrace);
  lastTrace = now;
}
#endif
```
Build with `-DBUTTON_DEBUG_TRACE=1`. Run device, attach serial, press each button 5×, capture log.

**Reads from log:**
- Spread of `adc1`/`adc2` while button held → ADC noise envelope.
- Bucket flips during a single press → confirms hypothesis 1.
- `dt` consistently > 50 ms with missed presses → confirms hypothesis 2.
- Clean stable reads but `state` wrong → device-side ladder voltage off (hypothesis 4).

#### Probe B — Update cadence histogram
Counters in `InputManager::update()`:
```cpp
#if BUTTON_DEBUG_CADENCE
static uint16_t buckets[8] = {0};  // <10ms, 10-20, 20-50, 50-100, 100-200, 200-500, 500-1000, >1000
static unsigned long lastUpdate = 0;
static unsigned long lastReport = 0;
const unsigned long dt = millis() - lastUpdate;
lastUpdate = millis();
int b = (dt < 10) ? 0 : (dt < 20) ? 1 : (dt < 50) ? 2 : (dt < 100) ? 3
      : (dt < 200) ? 4 : (dt < 500) ? 5 : (dt < 1000) ? 6 : 7;
buckets[b]++;
if (millis() - lastReport > 5000) {
  Serial.printf("[BTN-CADENCE] <10:%u 10-20:%u 20-50:%u 50-100:%u 100-200:%u 200-500:%u 500-1k:%u >1k:%u\n",
                buckets[0],buckets[1],buckets[2],buckets[3],buckets[4],buckets[5],buckets[6],buckets[7]);
  lastReport = millis();
}
#endif
```

**Reads from log:**
- Mass below 50 ms → cadence is fine, hypothesis 2 demoted.
- Tail in 100-500 ms during reading/transfer → confirms hypothesis 2.

#### Probe C — Press counter (user-driven)
Hold one button (say BACK) and press exactly **20 times** at ~1 Hz. Log `wasPressed(BACK)` count. If <20, miss rate quantified per session. Repeat for each button.

### Decision tree after evidence

| Evidence pattern | Hypothesis confirmed | Fix |
|---|---|---|
| Probe A shows bucket flips during press | #1 (ADC noise) | Add 8× oversample (mean) + raise `DEBOUNCE_DELAY` to 15 ms |
| Probe B histogram has fat 100-500 ms tail during heavy frames | #2 (cadence) | Add `gpio.update()` calls inside long-running activities; consider FreeRTOS task for input polling |
| Probe A clean, Probe C miss rate > 0 only on specific button | #4 (hardware) | Per-button calibration / replace device |
| Probe A shows quiescent `adc1/adc2` drift toward thresholds | sub-#1 (component drift) | Recalibrate `ADC_RANGES_*` |

Fixes #1 and #2 are independent and complementary. Likely both are partially true.

### Out of scope for this PR
Actual fixes are deferred until after evidence capture. This PR is the **port** PR. Button work tracked separately:
- Branch (planned): `debug/button-detection-trace` for Probes A+B+C
- Branch (planned): `fix/input-oversample-debounce` after evidence

### Rollback
Both port commits are isolated to OTA + WebServer activity files. Revert is a single `git revert` per commit if a regression surfaces post-merge.

---

## Open questions for reviewer

1. Confirm DX34 release-asset name is exactly `firmware.bin` (ReleaseJsonParser only matches that string).
2. Confirm `WIFI_ABANDON_MS = 5 min` is the desired UX (vs e.g. 2 min) before sending file-transfer back to network selection.
3. Approve adding the `BUTTON_DEBUG_TRACE` build flag temporarily, or prefer a separate `debug/` branch with the probes always-on.
