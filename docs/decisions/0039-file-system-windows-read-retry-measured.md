# ADR-0039: File system: Windows read retry after a replace, measured (amends 0038)

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** amends [ADR-0038](0038-file-system-windows-read-retry-during-replace.md) rules 1–3;
  [ADR-0036](0036-file-system-windows-atomic-replace-corrected.md),
  [file system guide](../development/file-system.md)

## Context

ADR-0038 has `readFile` on Windows retry a failed open only while the writer's temporary file
`<name>.tmp-<uuid>` exists next to the target, on the assumption that the window in which the name
is delete-pending or missing ends before the temporary file is gone. CI (windows-2025) kept
failing the readers-against-replacements stress test with reader errors, so the assumption was
checked.

## Measured

- Locally, 1 stress-test run in 33 failed under ADR-0038's rule. A temporary diagnostic (reading the
  NT status with `RtlGetLastNtStatus`, removed afterwards) showed that the open failed
  with `STATUS_OBJECT_NAME_NOT_FOUND` about 0.3 ms after the writer's rename had returned, and
  the temporary file was already gone. The window outlasts both the rename call and the
  temporary file, so ADR-0038's check reported the error at once.

## Not measured

- The NT status behind the access denials seen on CI. Windows reports a delete-pending file as
  `ERROR_ACCESS_DENIED` (`STATUS_DELETE_PENDING`), and a scanner briefly holding the new file can
  also deny the open; which one CI hit is not known.

## Decision

The POSIX rename used by `writeFileAtomic` is not atomic for opens by name. For a moment after
the writer's call returns, an open of the target can fail in two ways:

- with not found: measured as `STATUS_OBJECT_NAME_NOT_FOUND` about 0.3 ms after the rename
  returned, with the temporary file already gone;
- with access denied: seen on CI. Windows reports a delete-pending file this way, and a scanner
  may also hold the new file briefly.

So `readFile` on Windows treats not found, access denied and sharing violations as possibly
transient (**replaces ADR-0038 rules 1–3**):

1. It always opens once more, at once.
2. It then keeps trying, with sleeps of 1, 3, 7 and then 10 ms, for up to one second while a
   `<name>.tmp-<uuid>` file exists next to the name.
3. Without a temporary file, it keeps trying the same way for up to 50 ms after an access denial
   or sharing violation.
4. Otherwise it reports the error.

Costs: a missing file costs one extra open and one directory lookup; a real permission error
costs at most about 50 ms; a missing target next to a temporary file left by a crash costs one
second. POSIX is unchanged.

## Rejected

- **Detecting the cause from the NT status** (retry only on `STATUS_DELETE_PENDING` or a missing
  name during a rename). It needs `RtlGetLastNtStatus`, which is undocumented, so the engine does
  not depend on it.

## Tests

- The stress test (readers against repeated replacements, never a partial file, no spurious
  errors) keeps permanent diagnostics in its failure message: for each failed read, how long after
  the last write returned it happened and whether a temporary file existed.
- The missing-file test from ADR-0038 is unchanged: a missing file is reported quickly, and on
  Windows only after the wait when a temporary file is next to it.

## Revisit when

- the stress test fails again on Windows, locally or on CI;
- the NT status of the CI access denials is measured, and the 50 ms denial wait can be narrowed
  or must grow;
- the 50 ms wait on real permission errors, or the one-second wait next to leftover temporary
  files, shows up in profiles or as a user-visible stall;
- Windows documents a rename that is atomic for opens by name, or a documented way to read the
  NT status of a failed open.
