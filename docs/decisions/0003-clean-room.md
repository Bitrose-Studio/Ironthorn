# ADR-0003: Clean-room implementation of ideas from other engines

- **Status:** Accepted
- **Date:** 2026-11-20

## Context

Ironthorn borrows proven *ideas* from other engines. Examples:

- an abilities/attributes/effects system;
- delta-replicated arrays;
- input actions with mapping contexts;
- motion matching.

The source code of commercial engines and of marketplace plugins is licensed under terms that
don't allow it to be reused in another engine. Ideas, concepts and observable behaviour are
not covered by those licenses.

## Decision

1. **No code from other engines or their plugins** enters this repository unless its license
   explicitly allows it, and then only as a pinned third-party dependency. That covers code,
   shaders, comments, data, documentation text and sample content, including translations,
   ports and close paraphrases.
2. **Clean-room process** for systems inspired by another engine:
   - A *spec author* studies public documentation, talks, papers and observable behaviour. They
     write a behavioural spec under `docs/specs/`: what the system does, its rules and edge
     cases. The spec has no code, pseudo-code or class layouts from the original.
   - The *implementer* works from the spec, Ironthorn's architecture and public papers, and
     doesn't consult the original source code.
   - Reviewers reject code that mirrors another engine's internals. Each system's
     documentation lists its sources: specs, papers and talks.
3. **Our own names and API shapes**, designed for Ironthorn's ECS. No other product's names or
   trademarks in module, plugin or type names.
4. **Patents.** Before an algorithm known to be patented in some variants ships, it gets a
   review, and the implementation sticks to published, unencumbered methods.

## Consequences

- Building such a system takes longer than porting would. In exchange, the result is clean to
  license under Apache-2.0 and fits Ironthorn's design.
- Contributors must confirm their contributions follow this process ([CONTRIBUTING](../../CONTRIBUTING.md)).
