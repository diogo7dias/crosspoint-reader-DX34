# Mac-side test instructions — button detection fix

VPS work done. Two new branches pushed in **both** repos (main + submodule):

| Repo                                          | Branch                              | Purpose                                              |
|-----------------------------------------------|-------------------------------------|------------------------------------------------------|
| `diogo7dias/crosspoint-reader-DX34`           | `debug/button-detection-trace`      | Adds Probe A (ADC trace) + Probe B (cadence hist), guarded by build flags. |
| `diogo7dias/community-sdk` (submodule)        | `debug/button-detection-trace`      | Same, in `InputManager.cpp`.                         |
| `diogo7dias/crosspoint-reader-DX34`           | `fix/input-oversample-debounce`     | Submodule pointer bumped to fix branch (no other changes). |
| `diogo7dias/community-sdk` (submodule)        | `fix/input-oversample-debounce`     | 8x ADC oversample in `getState()` + `DEBOUNCE_DELAY` 5 → 15 ms. |

## Sync your Mac clone

```bash
cd ~/path/to/crosspoint-reader-DX34
git fetch --all
git submodule update --init --recursive
```

## Test flow — recommended order

### Step 1 — Baseline (current `main`)

So you have a "before" reference for miss-rate.

```bash
git checkout main
git submodule update --init --recursive
pio run -e default -t upload
```

On device, press each direction button **20 times at ~1 Hz** and count physical presses vs UI reactions. Note miss-rate per button.

### Step 2 — Apply fix #1 (ADC oversampling + 15 ms debounce)

```bash
git checkout fix/input-oversample-debounce
git submodule update --init --recursive    # important — pulls submodule fix branch
pio run -e default -t upload
```

Repeat the 20-press test on each button. Compare miss-rate to baseline.

**Expected:** miss-rate drops to near-zero on all buttons. If it does, fix #1 was the right call.

### Step 3 — (Optional) Probes for diagnostic data

If you want raw evidence (or fix #1 is incomplete), build with the probes on:

```bash
git checkout debug/button-detection-trace
git submodule update --init --recursive
pio run -e debug -t upload
pio device monitor -e debug
```

Serial output will stream:

```
[BTN] adc1=2700 adc2=4095 gpio3=1 state=0x02 commit=0x02 dt=24ms
[BTN-CADENCE] <10:0 10-20:48 20-50:152 50-100:14 100-200:2 200-500:0 500-1k:0 >1k:0
```

What to look for, per `docs/PR-PLAN-ota-wifi-buttons.md` Workstream 2:

- **Probe A** — `adc1`/`adc2` jitter while a button is held. Bucket flips during a single press → confirms hypothesis #1 (ADC noise). Now post-fix this should be flat.
- **Probe B** — cadence histogram. Mass below 50 ms = healthy. Tail in the 100–500 ms / >1 s buckets during heavy frames (EPD redraw, OTA, file transfer) → hypothesis #2 (variable loop cadence) is the next thing to fix.

### Step 4 — Report back

Tell me:
1. Baseline miss-rate (per button, out of 20).
2. Post-fix miss-rate (per button, out of 20).
3. Probe B cadence histogram during reading + during a file transfer (paste a couple of `[BTN-CADENCE]` lines).

If the fix nails it: open a PR from `fix/input-oversample-debounce` → `main` (both repos). I drafted nothing yet so you can word it however.

If miss-rate still nonzero or Probe B shows long-tail cadence: I'll cut a `fix/input-cadence` branch attacking hypothesis #2 (inject `gpio.update()` into long-running activities).

## Cleanup

When the fix is merged:

```bash
# Delete debug branch in both repos
git push origin --delete debug/button-detection-trace
cd open-x4-sdk
git push origin --delete debug/button-detection-trace
```

The fix branch only carries the bug-fix change; the debug branch is throwaway.
