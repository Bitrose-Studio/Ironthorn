- `World::add` returns a const pointer for every component type (`const T*` typed, `const void*`
  by record): a write after `add` would skip the hooks that already saw the value. Set the fields
  before adding, or write through `get` afterwards.
