- `ith/foundation/Mutex.h`: `ith::heldMutexCount()` returns how many `ith::Mutex` objects the
  calling thread holds, in builds with asserts, so code that must not block while holding a lock
  can assert it.
