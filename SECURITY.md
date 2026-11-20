# Security policy

## Reporting a vulnerability

Please **don't open a public issue** for security problems. Report them privately through
GitHub: **Security → Report a vulnerability** on this repository. Include:

- what is affected (module, file, version or commit);
- how to reproduce it;
- what an attacker could do with it.

We aim to confirm the report within 7 days and to agree on a disclosure date with you.

## Supported versions

Ironthorn has not been released yet. Until 1.0, only the latest commit on `main` receives
security fixes.

## Scope

The most security-relevant parts of a game engine are the ones that read untrusted data:

- asset and scene loading;
- save games;
- networking and replication;
- scripting.

These are fuzzed and run under sanitizers in CI as they are built
([ADR-0001](docs/decisions/0001-cpp-language-and-safety-rules.md)).
