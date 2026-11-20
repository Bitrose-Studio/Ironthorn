- `ith/foundation/LinearAllocator.h`: `ith::LinearAllocator`, a fixed-capacity arena with
  markers, for per-frame scratch data.
- `ith/foundation/ObjectPool.h`: `ith::ObjectPool<T>`, a chunked pool of fixed-size slots whose
  `create` returns an owning `std::unique_ptr` that gives the slot back to the pool.
