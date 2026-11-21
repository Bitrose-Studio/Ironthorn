- `ith_reflect`, a build tool in `tools/`, generates a module's reflection records from its
  headers annotated with `ITH_TYPE`, `ITH_FIELD` and `ITH_ENUM`, and refuses unsupported syntax
  with `file:line` errors. Modules opt in with `REFLECT` in `ith_add_module`; test libraries use
  `ith_reflect_target`.
