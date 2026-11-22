# Ironthorn

[![CI](https://github.com/Bitrose-Studio/Ironthorn/actions/workflows/ci.yml/badge.svg)](https://github.com/Bitrose-Studio/Ironthorn/actions/workflows/ci.yml)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

Ironthorn is a modular, 3D-first game engine that also does 2D, written in C++20. It aims for:

- **High-end image quality.**
- **Graphics that scale without breaking the look.** Lower settings reduce how precisely things
  are computed, not whether they exist, and presets are built from measured cost and measured
  visual loss.
- **Game UI that lives outside the world.** Widget assets are owned by the player and the HUD,
  never placed in the scene hierarchy.

> **Status: early development.** Phase P1, the engine's foundation, is complete and released as
> [0.1.0](CHANGELOG.md): platform layer, jobs, the RHI on Direct3D 12 and Vulkan, render graph,
> reflection, serialization, ECS, asset database and input. There is no editor, renderer or
> gameplay yet. APIs will change without notice until 1.0.

## Documentation

| Document | Purpose |
|---|---|
| [Architecture](docs/architecture.md) | Goals, layers, rendering, scalability, UI, roadmap |
| [Decisions (ADRs)](docs/decisions/README.md) | Architecture decisions, and the ones planned |
| [Building and testing](docs/development/building.md) | Toolchains, presets, the pre-commit gate |
| [Coding standards](docs/development/coding-standards.md) | Design principles, naming, documentation, tests |
| [Third-party code](docs/development/third-party.md) | Every dependency, its version and license |
| [Contributing](CONTRIBUTING.md) | How to propose changes |
| [Security](SECURITY.md) | How to report vulnerabilities |
| [Changelog](CHANGELOG.md) | Released changes; unreleased ones are in `changelog.d/` |

## Quick start

Windows (Visual Studio 2022 with the C++ workload and the C++ Clang tools):

```powershell
./scripts/bootstrap.ps1
./scripts/dev.ps1 build
./scripts/dev.ps1 test
```

Linux (Clang 18+ or GCC 13+, CMake 3.28+, Ninja):

```sh
cmake --preset linux-clang
cmake --build --preset linux-clang-dev
ctest --preset linux-clang-dev
```

## License

Ironthorn is licensed under the [Apache License 2.0](LICENSE). Copyright 2026 Bitrose Studio.
"Ironthorn" is a name of Bitrose Studio; the license does not grant rights to use it (Apache-2.0
§6).
