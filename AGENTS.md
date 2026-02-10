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
1. App allocates message queue, viewport, and one-shot click timer.
2. App switches USB config to HID mouse mode (unless screenshot macro is enabled).
3. Input callback pushes key events into queue.
4. Timer callback pushes tick events into the same queue.
5. Main loop:
   - polls event queue (`50 ms` timeout),
   - handles timer ticks by advancing click phase (`press`/`release`) and re-arming timer,
   - toggles autofire on `OK` release,
   - adjusts delay with `Left/Right` release,
   - exits on `Back`.
6. On toggle off / exit, app stops timer and releases left mouse button if needed.
7. On exit it restores previous USB config and frees resources.

## Control Model
- `OK`: toggle active/inactive
- `Left`: decrease delay by 10 ms (down to 0)
- `Right`: increase delay by 10 ms (unbounded)
- `Back`: exit app

## Timing Model (Current)
- Delay variable is shown as milliseconds and applied as two timer halves:
  - `half_delay_ms = autofire_delay / 2`
  - each half is clamped to at least `1 ms` tick
- Total cycle is approximately `2 * max(1, autofire_delay / 2)` ms.
- `delay=0` no longer creates an effectively unthrottled busy loop; max practical rate is now bounded by timer tick granularity.

## Failure Modes To Design For
- USB HID config switch can fail; current code uses `furi_check(...)` and abort-like behavior.
- Early returns or future error branches can skip cleanup if not centralized.
- Any crash while pressed can leave host with a perceived held button unless release-on-exit is enforced.

## Numeric Safety Notes
- `autofire_delay` is `uint32_t` and currently increases without an upper bound.
- Direct microsecond multiply overflow risk was removed with timer-based scheduling.
- Future changes should define and enforce explicit bounds, then perform checked/saturated math.

## Findings: Risks / Technical Debt
- Delay-based busy-wait was removed; timer callback now drives click phases via queued tick events.
- `autofire_delay` can still reach `0`; internal scheduling prevents unthrottled spin but UX guardrails are still missing.
- No upper bound for delay.
- No persisted settings across app launches.
- `tools.c` contains `strrev` that is currently unused.
- UI communicates basics but lacks richer status and guardrails for extreme rates.

## Priority Improvements (Critical First)

### P0 (Stability + Performance)
1. [Done] Replace delay-based busy loop with timer-driven state machine.
2. Clamp delay to safe range (for example `5..1000 ms`).
3. Ensure all exit/error paths restore USB config and release resources.
4. Reduce unnecessary redraw frequency (update only on state change or low-rate refresh tick).
5. Centralize cleanup in one path (`goto cleanup` pattern) to prevent leak/regression branches.

### P1 (User Experience)
1. Show clearer status: `ACTIVE/PAUSED`, effective CPS, delay, and selected mode.
2. Support long-press acceleration for delay changes.
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
  - `Left/Right` adjust delay and value is reflected on-screen.
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
1. Clamp delay to bounded range and update UI text to reflect actual behavior.
2. Centralize cleanup for all setup/exit/error paths.
3. Reduce redraw frequency (state-change driven or low-rate refresh).
4. Integrate richer status UI + CPS display.
5. Add persistent settings.
6. Refactor into modules and delete unused code.

## Notes for Future Agents
- Prefer preserving current user-facing controls unless explicitly changing UX.
- If timing behavior is changed, update on-screen labels to avoid unit ambiguity.
- Keep USB mode restoration as a non-negotiable requirement.
- Before major refactor, capture baseline behavior with a short manual test checklist.
