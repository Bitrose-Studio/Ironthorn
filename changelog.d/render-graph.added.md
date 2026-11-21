- `ith/render_graph`: render graph v1 (ADR-0028). `rg::RenderGraph` takes raster, compute and
  copy passes, each a name, a setup callable that declares texture and buffer uses, and an
  execute callable that records through `rg::PassContext`. It culls passes whose writes reach no
  output, derives every barrier from the declared uses (batched per pass, out of `Undefined`
  after the previous use, as `Barrier.h` requires), begins and ends rendering with store
  operations chosen from later use, and records the passes in groups into command lists, in
  parallel on a `JobSystem`, inside pass-name markers. Transient resources come from an
  `rg::ResourcePool` that reuses equal descriptions within and across frames and frees them after
  4 unused frames; imports keep their state between frames in `rg::ImportedState`, and
  `importBackbuffer()` takes the swapchain image to `Present`. Validation in Debug and development
  builds names the graph, pass and resource; transients can be clobbered to show stale reads;
  `dump()` and `dumpGraphviz()` print the plan. See `docs/development/render-graph.md`.
- `ith_render_graph_benchmark` reports declare, compile and execute times of 100 passes.
