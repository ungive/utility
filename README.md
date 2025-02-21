# C++ utility classes

This repository contains useful utility classes that I use in my projects
and which do not really deserve a repository of their own.
Most, if not all of these classes, are header-only,
well-documented and well-tested.
Read the documentation and tests for usage instructions.

---

### Atomic - [ungive/utility/atomic.h](./include/ungive/utility/atomic.h)

`Atomic` is a class that wraps a value of any type (even structs)
and provides thread-safe concurrent access to that value
by providing a get() method that blocks any set() calls
until all shared pointers returned by any amount of get() calls are destructed.
The class thereby provides efficient and thread-safe access
to possibly large amounts of data without copying.
The return value of get() is meant to be used like an rvalue
and should not be persistently stored anywhere.

```cpp
struct Data { int x = 1; };
Atomic<Data> value;
// all of the following statements are thread-safe:
value.watch([](Data const& data) {
    // called during each set() call
    data.x; // returns 2
});
value.get()->x; // returns 1 (blocks any set calls)
value.set({ 2 }); // sets x to 2 (blocking)
value.get()->x; // returns 2
// auto ref = value.get(); // do not do this
```

---
