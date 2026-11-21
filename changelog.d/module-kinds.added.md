- Modules declare a kind with `ith_add_module(... KIND runtime|client|tools|editor)` (default
  `runtime`). The `module_layering` test checks which kinds may depend on which, and that each
  kind sits in the right layers. `shader_compiler` is a `tools` module.
- `ith_add_application(<name> KIND editor|player|server|tools ...)` declares an executable; the
  `module_layering` test checks that everything it links is allowed for its target. Editor and
  tools applications are not part of the Shipping (`Release`) build.
- The CMake options `IRONTHORN_BUILD_CLIENT` and `IRONTHORN_BUILD_EDITOR` (on by default) build
  the client and editor modules, next to `IRONTHORN_BUILD_TOOLS`.
- The sample applications `minimal_player` and `minimal_server`, and the presets
  `linux-shipping-player` and `linux-shipping-server`, which build them in Shipping without the
  modules they must not link.
