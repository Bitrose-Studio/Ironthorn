- CPU profiling with the Tracy 0.15.0 client: `ith/foundation/Profile.h` has
  `ITH_PROFILE_SCOPE`, `ITH_PROFILE_SCOPE_DYNAMIC`, `ITH_PROFILE_FUNCTION`, `ITH_PROFILE_FRAME`,
  `ITH_PROFILE_PLOT` and `ITH_PROFILE_THREAD_NAME`. They compile to nothing unless the build has
  the CMake option `IRONTHORN_PROFILING` on, which the new `windows-profile`,
  `windows-clang-profile` and `linux-clang-profile` presets set. Profiling builds collect only
  while a Tracy 0.15.0 viewer is connected and listen on localhost only.
- In profiling builds, job system workers carry their names in the profiler, every job runs in a
  zone named after the job, and the sample players and the editor mark each frame.
