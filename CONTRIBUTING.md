# Contributing to Ironthorn

Thank you for your interest. Ironthorn is in early development, so the architecture still moves
quickly. Please **open an issue before starting a large change**, so we can agree on the
approach first.

## Before you start

- Read the [architecture overview](docs/architecture.md), the [ADRs](docs/decisions/README.md)
  and the [coding standards](docs/development/coding-standards.md).
- Follow the design rule: **the simplest solution that gives the same result wins**. Proposals
  that add abstraction without a present need will be asked to simplify.

## Making a change

1. Fork the repository and create a branch.
2. Build and test it ([building](docs/development/building.md)). Your change must:
   - pass `./scripts/dev.ps1 verify` on Windows, or the Linux presets including `linux-asan`;
   - include tests for normal cases, edge cases and failure paths;
   - document every new public declaration (its contract, not a restatement of the code);
   - add a changelog fragment for anything a user of the engine would notice
     ([changelog.d/README.md](changelog.d/README.md));
   - add an ADR for architecture changes ([template](docs/decisions/0000-template.md)).
3. Write commit messages as `<Area>: <what is true now>`, e.g.
   `Foundation: an Expected holding an error asserts when its value is read`.
4. Open a pull request. Explain what the change does and how you tested it. CI must pass.

## Rules that are never waived

- **Clean room.** Don't contribute code, shaders or documentation copied, translated or closely
  paraphrased from other engines or their plugins, or from any source whose license doesn't
  allow it ([ADR-0003](docs/decisions/0003-clean-room.md)).
- **Dependencies.** New third-party code needs a permissive license and a pinned version, and is
  listed in [third-party.md](docs/development/third-party.md).
- **No secrets, credentials or machine-local files** in commits.

## License of contributions

Ironthorn is licensed under the [Apache License 2.0](LICENSE). Unless you state otherwise, any
contribution you submit is licensed under the same terms (Apache-2.0 §5). By submitting it, you
confirm that you have the right to do so.
