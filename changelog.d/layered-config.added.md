- A `config` module (`ith/config/Config.h`) reads layered configuration: `ith::Config` over an
  ordered list of layer directories (engine, project, platform, user), with settings files in the
  `ith.config` envelope merged per file by JSON Merge Patch (RFC 7396): objects merge, arrays and
  other values replace, `null` removes. `config.get(ith::ConfigSection<T>{"Editor", "preferences"})`
  reads a reflected struct over its defaults and warns about unknown fields and wrong types;
  `config.set(section, value)` writes only what differs from the layers below, durably and in a
  stable key order. Whole files (layouts, input bindings) are found and written per layer, and
  `generation()` and `reload()` let consumers poll for changes. Broken files are skipped with a
  warning.
- The build copies `engine/config/` (`Editor.json`, `Keymap.json` with the default editor keys, and
  `Layouts/Default.json`) to `Config/` next to the executables.
