# C++ utility classes

This repository contains useful utility classes that I use in my projects
and which do not really deserve a repository of their own.
Most, if not all of these classes, are header-only,
well-documented and well-tested.
Read the documentation and tests for usage instructions.

---

### Atomic <sup>`C++11`</sup> [ungive/utility/atomic.hpp](./include/ungive/utility/atomic.hpp)

`Atomic` is a class that wraps a value of any type (even structs)
and provides thread-safe concurrent access to that value
by providing a get() method that blocks any set() calls
until all shared pointers returned by any amount of get() calls are destructed.
The class thereby provides efficient and thread-safe access
to possibly large amounts of data without copying.
The return value of get() is meant to be used like an rvalue
and should not be persistently stored anywhere.

```cpp
struct Data { int x = 1; int y = 2; };
Atomic<Data> value;

// all of the following statements are thread-safe:
value.watch([](Data const& data) {
    // this is called with each successful set() call
    // the respective set call returns when this returns
    data.x; // yields 3 with by last set() call
});
// get() blocks any set calls until the return value is destructed
value.get()->x; // yields 1 (default value)
// assume the following set() call blocks until the set() call after it:
value.set({ 2 }); // attempts to set and blocks
// another set call is made while the previous one is blocking
// the value will be set to the newer one passed by the more recent call
value.set({ 3 }); // returns true, the previous set call returns false
value.get()->x; // yields 3

// only do this when the struct has related values that must be read together
// Reading x and y with two separate get() calls may yield unrelated values!
std::shared_ptr<const Data> ptr = value.get();
auto point = std::make_pair(ptr->x, ptr->y);
ptr = nullptr; // destroy the reference

// Absolutely do not do this! The pointer is destructed, which allows set()
// calls to modify the data. The following references are not thread-safe.
// Always copy by value when the it is needed beyond the pointer's lifetime.
Data const& ref = *value.get(); // danger!
int& x_ref = value.get()->x; // danger!
int x_copy = value.get()->x; // ok
```

---

## Copyright

Copyright (c) 2025 Jonas van den Berg  
All rights reserved.
