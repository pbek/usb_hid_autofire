# AGENTS.md

## Purpose
This file captures the key technical findings about this repository and the highest-value improvement ideas. Use it as the default starting point before doing new analysis.

## Project Snapshot
- Name: `usb_hid_autofire`
- Platform: Flipper Zero external app (`.fap`)
- Primary behavior: emulate USB HID mouse and generate repeated left-click events
- Entry point: `usb_hid_autofire_app` in `usb_hid_autofire.c`
- App manifest: `application.fam`
- Current version: `0.6` (`version.h`)

## Critical Invariants
- Always restore the previous USB configuration before app exit.
- Never leave mouse left button pressed when exiting or on failure.
- Keep `Back` key exit reliable regardless of autofire state.
- Do not block UI/input handling long enough to make stop controls feel laggy.

## Current Runtime Flow
1. App allocates message queue, viewport, one-shot click timer, and periodic UI refresh timer.
2. App switches USB config to HID mouse mode (unless screenshot macro is enabled).
3. Input callback pushes key events into queue.
4. Timer callback pushes tick events into the same queue.
5. Main loop:
   - blocks on event queue (`FuriWaitForever`),
   - handles timer ticks by advancing click phase (`press`/`release`) and re-arming timer,
   - while active, handles periodic UI refresh ticks (`250 ms`) for real-time CPS display updates,
   - toggles autofire on `OK` release,
   - adjusts delay with `Left/Right` short press and long-press acceleration (`Long`/`Repeat`),
   - exits on `Back`,
   - redraws viewport only when UI-visible state changes.
6. On toggle off / exit, app stops timers and releases left mouse button if needed.
7. On exit it restores previous USB config and frees resources.

## Control Model
- `OK`: toggle active/inactive
- `Left`: decrease delay with acceleration while held (clamped at 5 ms)
- `Right`: increase delay with acceleration while held (clamped at 1000 ms)
- `Back`: exit app

## Timing Model (Current)
- Delay variable is now bounded to `5..1000 ms`.
- Click scheduling uses two timer halves:
  - `half_delay_ms = autofire_delay / 2`
  - fallback clamp to at least `1 ms` tick remains as a defensive guard
- Total cycle is approximately `autofire_delay` ms (integer rounding by half-split applies).
- UI shows real-time CPS derived from observed click-release timing, so runtime lag is reflected in displayed rate.

## Failure Modes To Design For
- USB HID config switch can fail; current code now routes setup failures through centralized cleanup.
- Cleanup is centralized for setup failures and normal exit, reducing leak/regression risk from early returns.
- Any crash while pressed can leave host with a perceived held button unless release-on-exit is enforced.

## Numeric Safety Notes
- `autofire_delay` is `uint32_t` and is clamped to `5..1000 ms`.
- Direct microsecond multiply overflow risk was removed with timer-based scheduling.
- Delay adjustments now use saturating logic for both decrement and increment paths.

## Findings: Risks / Technical Debt
- Delay-based busy-wait was removed; timer callback now drives click phases via queued tick events.
- Delay is now clamped to `5..1000 ms`; runaway/unbounded delay behavior is removed.
- Unconditional per-loop redraw was removed; rendering is state-change driven.
- No persisted settings across app launches.
- `tools.c` contains `strrev` that is currently unused.
- UI communicates basics but lacks richer status and guardrails for extreme rates.

## Priority Improvements (Critical First)

### P0 (Stability + Performance)
1. [Done] Replace delay-based busy loop with timer-driven state machine.
2. [Done] Clamp delay to safe range (`5..1000 ms`).
3. [Done] Ensure all exit/error paths restore USB config and release resources.
4. [Done] Reduce unnecessary redraw frequency (update only on state change or low-rate refresh tick).
5. [Done] Centralize cleanup in one path (`goto cleanup` pattern) to prevent leak/regression branches.

### P1 (User Experience)
1. [Done] Show clearer status: `ACTIVE/PAUSED`, real-time CPS, delay, and selected mode.
2. [Done] Support long-press acceleration for delay changes.
3. Add presets (for example slow/medium/fast) and optional safety confirmation for very high CPS.
4. Persist user settings (`delay`, mode, last active state policy).

### P2 (Maintainability)
1. Split monolithic file into modules:
   - input/controller state
   - HID sender/timer
   - UI rendering
   - settings persistence
2. Remove dead code (`tools.c/.h`) unless reused.
3. Add internal comments only for non-obvious timing/state logic.

### P3 (Quality + Release Process)
1. Add tests for state transitions and timing math.
2. Validate behavior against multiple Flipper firmware versions in CI.
3. Expand docs with troubleshooting and host OS behavior notes.

## Build And Quick Verification
- Build:
  - `./fbt fap_usb_hid_autofire`
- Build and launch:
  - `./fbt launch_app APPSRC=usb_hid_autofire`
- Manual smoke checklist:
  - App opens and renders status/version.
  - `OK` toggles active/inactive.
  - `Left/Right` short press adjusts delay by one step, hold accelerates, and values clamp at `5..1000 ms`.
  - While active, host receives repeated left clicks.
  - `Back` exits immediately and USB behavior returns to pre-app mode.
  - After exit, no stuck mouse button state on host.

## Compatibility Notes
- Changelog records compatibility fix for Flipper firmware `0.74.2`.
- Keep API usage aligned with current Flipper firmware headers and re-test on firmware updates.

## Definition Of Done For Major Refactors
- Click timing remains stable with acceptable jitter under UI interaction.
- UI remains responsive while autofire is active.
- Exit path is deterministic and always restores USB config.
- No stuck pressed-button behavior after any normal exit path.
- Existing control contract (`OK`, `Left`, `Right`, `Back`) is preserved unless intentionally changed and documented.

## Suggested Implementation Order
1. Add presets and optional high-CPS safety confirmation.
2. Add persistent settings.
3. Refactor into modules and delete unused code.

## Notes for Future Agents
- Prefer preserving current user-facing controls unless explicitly changing UX.
- If timing behavior is changed, update on-screen labels to avoid unit ambiguity.
- Keep USB mode restoration as a non-negotiable requirement.
- Before major refactor, capture baseline behavior with a short manual test checklist.
