# ADR-0038: File system: Windows read retry while a replace is in flight (amends 0036)

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** amends [ADR-0036](0036-file-system-windows-atomic-replace-corrected.md) rule 4
  (readers on Windows); [ADR-0035](0035-file-system-configuration-and-main-thread-queue.md) rule 2
  (`readFile`), [file system guide](../development/file-system.md)

## Context

ADR-0036 rule 4 has `readFile` on Windows retry `ERROR_FILE_NOT_FOUND` up to 3 times, yielding
with `SwitchToThread`, to cover the instant in which the POSIX rename of a concurrent
`writeFileAtomic` leaves the name empty. That was not enough:

- on CI (windows-2025), readers racing a replacement failed with `fs.access_denied`;
- in a local stress run under load, `fs.not_found` lasted longer than three yields.

The likely cause follows from documented Win32 behaviour; it was not measured on CI. The POSIX
rename first marks the old file delete-pending. Opening a delete-pending file fails with
`STATUS_DELETE_PENDING`, which Win32 reports as `ERROR_ACCESS_DENIED`. The name is then briefly
missing until the new link appears. If the writer is descheduled between these steps, the window
lasts as long as the writer is off the CPU, which a fixed number of yields can't cover.

Throughout this window the writer's temporary file `<name>.tmp-<uuid>` exists next to the target.

## Options considered

- **A larger fixed retry count.** It still has no bound tied to the writer, and it makes every
  real permission error and every truly missing file pay the full count. Rejected.
- **Different share modes or open flags.** No Windows rename is atomic for opens by name while it
  replaces an open file, so no combination of flags removes the window. Rejected.
- **Retry only while a replacement is visibly in progress** (chosen): the temporary file tells
  the reader whether waiting can help.

## Decision

1. **Replaces ADR-0036 rule 4.** When the open in `readFile` on Windows fails with
   `ERROR_FILE_NOT_FOUND`, `ERROR_ACCESS_DENIED` or `ERROR_SHARING_VIOLATION`, it is retried only
   while a file `"<name>.tmp-*"` exists next to the target, checked with one `FindFirstFileExW`
   before each retry.
2. **Timing:** sleeps of 0, 1, 3 and 7 ms, then 10 ms each, for up to 1 s in total. After that
   the last error is reported.
3. **No temporary file:** the error is reported at once, after one extra directory lookup, so a
   real permission error or a missing file stays fast.
4. **Tests:** the existing stress test (readers against repeated replacements, never a partial
   file, no spurious errors), and a new test that a missing file is reported quickly and, on
   Windows, that a missing file next to a temporary file is reported only after the wait.

POSIX is unchanged: `rename` replaces the name in one step there.

## Consequences

- Readers racing a save on Windows see the old file or the new one instead of `fs.access_denied`
  or `fs.not_found`, even when the writer is descheduled mid-rename.
- A missing file or a real access error costs one `FindFirstFileExW` more than before; there is
  no fixed retry any more.
- A temporary file left by a crash makes reads of a missing target wait the full second. Scans
  already report such leftovers (`isTemporaryFileName`).
- **Revisit when:**
  - the one-second wait shows up in profiles or as a user-visible stall (for example, many
    leftover temporary files);
  - CI shows reader errors during a replacement that the temporary-file check doesn't explain;
  - Windows documents a rename that is atomic for opens by name.
