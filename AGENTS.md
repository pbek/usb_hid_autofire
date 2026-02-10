# AGENTS.md

## Purpose
This file captures the key technical findings about this repository and the highest-value improvement ideas. Use it as the default starting point before doing new analysis.

## Project Snapshot
- Name: `usb_hid_autofire`
- Platform: Flipper Zero external app (`.fap`)
- Primary behavior: emulate USB HID and generate repeated selected events (mouse left/right click or keyboard Enter/Space)
- Entry point: `usb_hid_autofire_app` in `usb_hid_autofire.c`
- App manifest: `application.fam`
- Current version: `0.6` (`version.h`)

## Current Module Layout
- `usb_hid_autofire.c`: app lifecycle/orchestration (resource allocation, event dispatch, centralized cleanup)
- `usb_hid_autofire_i.h`: shared state struct, constants, enums, and module API contracts
- `usb_hid_autofire_controller.c`: input/controller state transitions, delay/mode/preset logic, confirmation flow
- `usb_hid_autofire_hid.c`: timer callbacks, HID press/release engine, click phase scheduler, CPS tracking
- `usb_hid_autofire_ui.c`: canvas rendering for main/help screens
- `usb_hid_autofire_settings.c`: persistence load/save/flush with debounced write scheduling

## Critical Invariants
- Always restore the previous USB configuration before app exit.
- Never leave any fired control pressed (mouse button or keyboard key) when exiting or on failure.
- Keep `Back` key exit reliable regardless of autofire state.
- Do not block UI/input handling long enough to make stop controls feel laggy.

## Current Runtime Flow
1. App allocates message queue, viewport, one-shot click timer, periodic UI refresh timer, and one-shot settings-save debounce timer.
2. App switches USB config to HID mouse mode (unless screenshot macro is enabled).
3. App loads persisted settings from `APP_DATA_PATH(".settings")` using Flipper Format (`delay`, `mode`, `preset`, `startup_policy`, `last_active`).
4. Input callback pushes key events into queue.
5. Timer callbacks push tick/refresh/settings-save events into the same queue.
6. Main loop:
   - blocks on event queue (`FuriWaitForever`),
   - handles timer ticks by advancing click phase (`press`/`release`) and re-arming timer,
   - while active, handles periodic UI refresh ticks (`250 ms`) for real-time CPS display updates,
   - toggles autofire on `OK` short release in main screen,
   - cycles presets on `OK` long press (slow/medium/fast),
   - opens built-in Flipper modal dialog for very high-CPS preset confirmation,
   - cycles fire mode on `Up/Down` short press and hold-repeat (`Up` backward, `Down` forward),
   - adjusts delay with `Left/Right` short press and long-press acceleration (`Long`/`Repeat`),
   - opens help screen on `Back` long press,
   - closes help screen on `Back` short release (without exiting),
   - exits on `Back` short release from main screen,
   - redraws viewport only when UI-visible state changes,
   - flushes dirty settings on debounced save events.
7. On toggle off / exit, app stops timers and releases any active fired control (mouse button or keyboard key) if needed.
8. On exit it flushes pending settings, restores previous USB config, and frees resources.

## Control Model
- `OK` short (main): toggle active/inactive
- `OK` long: cycle presets (`Slow` -> `Medium` -> `Fast` -> ...)
- `OK` confirm: apply pending high-CPS preset when confirmation prompt is shown
- `Up`: cycle fired event mode backward (`Key Space` <- `Key Enter` <- `Mouse Right` <- `Mouse Left`)
- `Down`: cycle fired event mode forward (`Mouse Left` -> `Mouse Right` -> `Key Enter` -> `Key Space`)
- `Left`: decrease delay with acceleration while held (clamped at 5 ms)
- `Right`: increase delay with acceleration while held (clamped at 10000 ms)
- `Back` long: open help screen
- `Back` short on help: close help screen
- `Back` short on main: exit app

## Timing Model (Current)
- Delay variable is now bounded to `5..10000 ms`.
- Click scheduling uses two timer halves:
  - `half_delay_ms = autofire_delay / 2`
  - fallback clamp to at least `1 ms` tick remains as a defensive guard
- Total cycle is approximately `autofire_delay` ms (integer rounding by half-split applies).
- UI shows real-time CPS derived from observed click-release timing, so runtime lag is reflected in displayed rate.
- Presets:
  - `Slow`: `250 ms` (~4.0 CPS)
  - `Medium`: `120 ms` (~8.3 CPS)
  - `Fast`: `70 ms` (~14.3 CPS, requires confirmation)
- Settings persistence uses debounced writes (`500 ms`) to reduce write frequency during hold-repeat changes.
- Startup policy is currently fixed to `PausedOnLaunch` (no user-facing toggle), while startup-state handling logic remains in code.

## Failure Modes To Design For
- USB HID config switch can fail; current code now routes setup failures through centralized cleanup.
- Cleanup is centralized for setup failures and normal exit, reducing leak/regression risk from early returns.
- Any crash while pressed can leave host with a perceived held button unless release-on-exit is enforced.

## Numeric Safety Notes
- `autofire_delay` is `uint32_t` and is clamped to `5..10000 ms`.
- Direct microsecond multiply overflow risk was removed with timer-based scheduling.
- Delay adjustments now use saturating logic for both decrement and increment paths.

## Findings: Risks / Technical Debt
- Delay-based busy-wait was removed; timer callback now drives click phases via queued tick events.
- Delay is now clamped to `5..10000 ms`; runaway/unbounded delay behavior is removed.
- Unconditional per-loop redraw was removed; rendering is state-change driven.
- `tools.c` contains `strrev` that is currently unused.
- High-CPS preset confirmation uses built-in modal dialogs.
- Modal confirmation path now uses bounded input-queue draining to avoid long-press repeat deadlock before dialog show.
- Main view is compact; help is now a dedicated screen opened by `Back` long press.
- UI control hints now use rendered button glyph icons (Up/Down/Left/Right/OK/Back) from app-local icon assets for FAP-safe symbol resolution.
- Fire engine now supports both mouse and keyboard events with release-on-stop safety.
- Persisted settings currently include: delay, mode, preset, startup policy (internally fixed to paused), and last active state.
- Monolithic implementation has been split into focused modules with a shared internal header contract.

## Priority Improvements (Critical First)

### P0 (Stability + Performance)
1. [Done] Replace delay-based busy loop with timer-driven state machine.
2. [Done] Clamp delay to safe range (`5..10000 ms`).
3. [Done] Ensure all exit/error paths restore USB config and release resources.
4. [Done] Reduce unnecessary redraw frequency (update only on state change or low-rate refresh tick).
5. [Done] Centralize cleanup in one path (`goto cleanup` pattern) to prevent leak/regression branches.

### P1 (User Experience)
1. [Done] Show clearer status: `ACTIVE/PAUSED`, real-time CPS, delay, and selected mode.
2. [Done] Support long-press acceleration for delay changes.
3. [Done] Add presets (slow/medium/fast) and optional safety confirmation for very high CPS.
4. [Done] Persist user settings (`delay`, mode, last active state policy).

### P2 (Maintainability)
1. [Done] Split monolithic file into modules:
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
  - `Up/Down` cycle fired event mode (including hold repeat) and selected mode is shown on screen.
  - `Left/Right` short press adjusts delay by one step, hold accelerates, and values clamp at `5..10000 ms`.
  - `OK` long cycles presets and `Fast` preset requires explicit confirmation.
  - `Back` long opens help screen; `Back` short in help closes help; `Back` short in main exits app.
  - While active, host receives repeated events for selected mode (`Mouse Left/Right`, `Enter`, `Space`).
  - `Back` short in main exits immediately and USB behavior returns to pre-app mode.
  - After exit, no stuck mouse button or keyboard key state on host.
  - After restart, persisted settings are restored; app starts paused by policy.

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
1. Remove dead code (`tools.c/.h`) unless reused.
2. Add internal comments only for non-obvious timing/state logic.

## Notes for Future Agents
- Prefer preserving current user-facing controls unless explicitly changing UX.
- If timing behavior is changed, update on-screen labels to avoid unit ambiguity.
- Keep USB mode restoration as a non-negotiable requirement.
- Before major refactor, capture baseline behavior with a short manual test checklist.
