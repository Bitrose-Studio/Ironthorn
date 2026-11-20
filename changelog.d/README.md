# Changelog fragments

Every change a user of the engine would notice adds one file here
([ADR-0002](../docs/decisions/0002-git-workflow-and-versioning.md)):

    changelog.d/<slug>.<kind>.md

- `<slug>`: lower-case words joined by hyphens, e.g. `expected-type`.
- `<kind>`: one of `added`, `changed`, `deprecated`, `removed`, `fixed`, `security`.
- Content: one or more Markdown bullet points (`- ...`), written for engine users.

At release time the fragments are gathered into `CHANGELOG.md` under the version's heading
and deleted. Don't edit `CHANGELOG.md` directly. The `changelog_fragments` test validates
the files.
