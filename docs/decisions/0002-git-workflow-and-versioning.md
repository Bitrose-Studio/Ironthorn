# ADR-0002: Git workflow and versioning

- **Status:** Accepted
- **Date:** 2026-11-20

## Context

Ironthorn is maintained by a small core team and is open to outside contributions. History has
to stay readable, releases need a clear meaning, and parallel changes shouldn't conflict over
the changelog.

## Decision

### Git workflow

1. **Maintainers work on `main`.** They use a short branch only for risky or long work, and
   never force-push `main`.
2. **Contributors** open pull requests from a fork or a branch ([CONTRIBUTING](../../CONTRIBUTING.md)).
3. **One item per commit.** Each commit builds, passes the tests, and doesn't mix unrelated
   changes.
4. **Review before committing.** The change must:
   - follow the [coding standards](../development/coding-standards.md) and the ADRs;
   - have tests for normal, edge and failure cases;
   - pass `scripts/dev.ps1 verify` on Windows. CI covers Linux.
5. **Commit messages:** `<Area>: <what is true now>`, in plain words. For example:
   `Foundation: an Expected holding an error asserts when its value is read`.
   - The area is the module or topic (`Foundation`, `RHI`, `Build`, `Docs`, `CI`).
   - A body explains why, when that isn't obvious.
6. **No secrets and no machine-local files.** Binary assets go through Git LFS
   (`.gitattributes`).

### Versioning

7. **Semantic Versioning 2.0.0**, one version for the whole engine. It is set in `project()` in
   the root `CMakeLists.txt` and exposed as `ith::kEngineVersion` / `ith::kEngineVersionString`.
8. **`main` carries the next version with `-dev`** (`IRONTHORN_VERSION_SUFFIX`, now `0.1.0-dev`).
   A release commit removes the suffix and is tagged. The next commit sets the following `-dev`
   version.
9. **Before 1.0**, a minor release (`0.x.0`) may break APIs and a patch release (`0.x.y`) only
   fixes. **Releases follow milestones**: 0.1.0 comes when phase P1 of the
   [roadmap](../architecture.md#roadmap) is complete.
10. **Tags:** an annotated tag `vX.Y.Z` on `main`, with a GitHub Release whose notes are the
    matching `CHANGELOG.md` section.
11. **Changelog fragments.** A user-visible change adds `changelog.d/<slug>.<kind>.md`.
    - `<kind>` is one of `added`, `changed`, `deprecated`, `removed`, `fixed`, `security`.
    - The file holds bullet points.
    - The `changelog_fragments` test validates them.
    - At release time the fragments are gathered into `CHANGELOG.md` and deleted.
    - `CHANGELOG.md` is never edited by hand between releases.
12. **Versioned separately**, each with its own ADR when it appears: asset, scene and project
    file formats, the C ABI and the plugin API.

## Consequences

- History stays linear and easy to read.
- `main` has no review gate for maintainers. CI runs on every push, and a red build is fixed
  before anything else. Branch protection will be added when the team grows.
