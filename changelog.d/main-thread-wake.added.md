- `ith::Platform::waitEvents(timeout)` waits for an event before pumping, for applications that
  idle, and the thread-safe `ith::Platform::wake()` ends that wait early; wakes before the next
  pump are coalesced into one internal event that pumps drop.
- `ith::JobSystem::setMainThreadWake(hook, context)` registers a hook called after every
  main-thread post and every update of an `ith::Progress` made with the JobSystem
  (`ith::Progress progress(jobs)`); connected to `Platform::wake()`, a result posted by a
  background job ends an event wait at once. `JobSystem::wakeMainThread()` calls the hook.
- The application's loop drains the main-thread queue after its event pump and before it updates
  and renders; `window_player` and `scene_player` follow this order.
