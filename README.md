# C++ utility classes

This repository contains useful utility classes that I use in my projects
and which do not really deserve a repository of their own.
Most, if not all of these classes, are header-only,
well-documented and well-tested.
Read the documentation and tests for usage instructions.

---

### LiveValue - ungive/utility/live_value.h

`LiveValue` is a class that wraps a value of any type (even structs)
and provides thread-safe concurrent access to that value
by providing a get() method that blocks any set() calls
until all shared pointers returned by get() are destructed.
The class thereby provides efficient and thread-safe access without copying.
The return value of get() is meant to be used like an rvalue
and should not be persistently stored anywhere.

---
