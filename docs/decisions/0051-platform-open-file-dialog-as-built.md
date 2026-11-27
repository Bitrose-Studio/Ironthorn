# ADR-0051: Platform open-file dialog as built (amends 0049)

- **Status:** Accepted
- **Date:** 2026-11-27
- **Related:** amends [ADR-0049](0049-editor-content-browser-v1.md) rule 25;
  [ADR-0012](0012-windowing-and-event-loop.md) rule 8 (file dialogs delivered as events);
  [ADR-0035](0035-file-system-configuration-and-main-thread-queue.md) rule 23 (`Platform::wake`);
  [ADR-0007](0007-logging-and-profiling.md) rule 8 (state that is never destroyed)

## Context

ADR-0049 rule 25 says the dialog's callback copies the paths and posts them to the main thread
through SDL's thread-safe event queue. Building `Platform::showOpenFileDialog`
(`platform/Platform.{h,cpp}`) on SDL 3.4.20 showed what that callback really is:

- **Where it runs.** On Windows (`SDL_windowsdialog.c`) and with zenity (`SDL_zenitydialog.c`) on
  a detached SDL thread. With the xdg desktop portal (`SDL_portaldialog.c`) inside the D-Bus
  message filter, which SDL runs from its event pump (`SDL_events.c`, `SDL_DBus_PumpEvents`), so
  on the main thread, inside `waitEvents`. It may also run before `SDL_ShowOpenFileDialog`
  returns (invalid filter, no dialog available; `SDL_dialog.c`).
- **After shutdown.** A detached dialog thread outlives `SDL_Quit` and may call back while the
  process is destroying its statics. The portal's callback never arrives after `SDL_Quit`, since
  the D-Bus connection is gone.
- **The queue.** `SDL_Quit` drops queued events without looking at their user data, and
  `SDL_PushEvent` fails when the queue holds 65,535 events (`SDL_events.c`).
- **Waiting.** The portal's answer is read only when SDL pumps D-Bus. A wait blocks on the
  display connection and wakes for D-Bus only every 3 seconds (`DBUS_POLL_INTERVAL_NS`).
- **Edge cases.** Zenity reports a cancel as a list with one empty path. `default_location` is
  read as a folder or a file: on Windows a trailing separator selects the initial folder and
  anything else is a file name, which needs backslashes. The filter check reads the character
  before the end of each pattern, so an empty pattern is read out of bounds
  (`SDL_dialog_utils.c`).

The caller-visible contract of rule 25 stands: main thread, one dialog at a time
(`platform.dialog_busy`), `FileDialogFile` per path then `FileDialogClosed`. Items 1 to 5 restate
rule 25's hand-off and add the decisions on its edges; rules not listed stand.

## Options considered

How the result reaches the main thread:

- **Heap-owned strings in a user event's `data1`/`data2` (rule 25 as written).** The pump frees
  them. If `SDL_Quit` clears the queue first, or the push fails on a full queue, the strings
  leak or the result is lost and the dialog stays busy forever. Rejected.
- **A dedicated user event type carrying an index into a slot.** Nothing in the queue owns
  memory, but it needs a second registered event type and a second translation path beside the
  wake, and a full queue still loses the result unless the slot is polled anyway. Rejected: once
  the slot is polled, the extra event type adds nothing.
- **A mutex-guarded slot plus the existing wake (chosen).** The callback stores the result in a
  process-wide slot and calls `Platform::wake()`, whose event carries no data. Every pump checks
  the slot. A lost or cleared wake only delays the result to the next pump.

Teardown with a callback still pending (`Platform::create` already forbids two live Platforms):

- **Wait in `~Platform` for the callback.** The user may leave the dialog open indefinitely, and
  on the portal it never comes after `SDL_Quit`. Rejected.
- **Per-request storage with a request id in `userdata`.** Handles successive Platforms and
  overlapping dialogs, but adds allocation and lookup for a case the editor doesn't have (one
  Platform per process). Kept as the remedy if that changes.
- **Mark the dialog abandoned and keep its filter copies until the callback runs (chosen).**

## Decision

1. **ADR-0049 rule 25, the hand-off.** The callback, on whichever thread SDL calls it, copies the
   paths (or SDL's error) into a process-wide, mutex-guarded slot and calls `Platform::wake()`
   (ADR-0035 rule 23), which pushes only the existing empty wake event. The next pump takes the
   slot and emits one `FileDialogFile` per path, then `FileDialogClosed`, which ends
   `dialog_busy`. The events carry the window's id as before. *Reason:* no heap strings owned by
   SDL's queue, so nothing leaks when `SDL_Quit` clears it; a full queue only delays the result;
   no second event type.
2. **ADR-0049 rule 25, the slot's lifetime (new decision).** The slot is constructed once, on
   first use, in static storage with placement new, and never destroyed: its destructor never
   runs, and nothing is allocated on the heap for it. This is the logger's pattern (ADR-0007
   rule 8). *Reason:* a detached SDL dialog thread may call back during static destruction; a
   destroyed mutex there is undefined behaviour.
3. **ADR-0049 rule 25, teardown (new decision).** `~Platform` drops an undelivered result. If the
   callback is still pending, it marks the dialog abandoned and keeps the filter and folder
   copies, which SDL may still read, until the callback runs. The callback checks for an
   abandoned dialog before it reads anything else, including `SDL_GetError`, then drops its
   result and wakes nobody; a callback with no dialog pending is ignored. A later `Platform`
   gets `platform.dialog_busy` until the callback has run. On the Linux portal it never runs
   after `SDL_Quit`, so a portal dialog left open when a Platform is destroyed keeps dialogs busy
   for every later Platform in the process. *Reason:* the editor creates one Platform per
   process; per-request storage keyed by a request id in `userdata` is the remedy if a process
   ever needs more.
4. **ADR-0049 rule 25, SDL's edge cases (new decision).**
   - An empty path from zenity's cancel is dropped, so a cancel delivers only `FileDialogClosed`.
   - `defaultFolder` gets a trailing separator, and on Windows its forward slashes become
     backslashes, before it is passed as `default_location`.
   - An empty filter pattern is a precondition failure and asserts. If execution continues,
     SDL isn't called and the dialog ends with a `FileDialogClosed` carrying the error.
   - A window SDL doesn't know asserts. If execution continues, as in release builds, the dialog
     opens without a parent and its events carry the unknown id.

   *Reason:* the contract says "folder" and "a cancel delivers no files", and SDL's
   folder-or-file reading must not leak through; SDL reads past the end of an empty pattern.
5. **ADR-0049 rule 25, waiting with a dialog open (new decision).** While a dialog is pending,
   `waitEvents` caps its timeout at 50 ms, on every backend. *Reason:* the portal's answer is
   read only by SDL's pump and the wait blocks on the display connection, so without the cap the
   result could wait up to SDL's 3-second D-Bus interval. SDL doesn't tell the engine which
   dialog backend it picked, and a per-backend cap would need an `#ifdef` or a probe for no
   measurable gain. 50 ms is imperceptible after closing a dialog and costs at most 20 idle
   wake-ups a second, only while a dialog is open.

## Consequences

- Rule 25's contract holds unchanged for callers (`UiHost::showOpenFileDialog`, the Content
  Browser's import); only its "through SDL's queue" mechanism is replaced.
- The slot's destructor never runs; it owns only what is left in it at exit.
- On Linux with the portal, destroying a Platform with a dialog open leaves dialogs busy for
  every later Platform in the process. The editor doesn't do this; tests that create several
  Platforms must not leave a dialog open.
- The idle editor wakes every 50 ms while a dialog is open.
- `platform` has internal seams for its own tests only: `detail::beginOpenFileDialog`,
  `detail::finishOpenFileDialog` and `detail::dialogFolder`. Offscreen tests drive the slot, the
  abandoned state and the edge cases through them, and `dialogFolder` is unit-tested
  (`tests/platform/DropAndFileDialogTests`). The real dialogs and drops still need manual checks
  on Windows and on Linux with both the portal and zenity.
- On acceptance, ADR-0049's README row becomes "Accepted, amended by 0050, 0051" and its header
  gets an "Amended by" line.
- **Revisit when:** a process needs more than one Platform in turn, or more than one dialog at a
  time (per-request storage); save or folder dialogs arrive (same hand-off, check their SDL edge
  cases); an SDL update changes where the callback runs, the portal's pumping, or how
  `default_location` and patterns are read; the 50 ms wake-ups show up in idle power
  measurements.
