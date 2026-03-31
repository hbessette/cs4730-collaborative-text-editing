# p2p Collaborative Text Editor

A peer-to-peer collaborative text editor built in C++.

## Dependencies

- CMake 3.16+
- A C++11-capable compiler (GCC, Clang)
- pthreads (standard on Linux/macOS)
- [FTXUI](https://github.com/ArthurSonzogni/FTXUI) — fetched automatically via CMake FetchContent

## Build

```bash
cmake -B build
cmake --build build
```

## Run

```bash
./build/p2p-editor
```

## Test

```bash
./build/tests
```

## Project Structure

```
.
├── CMakeLists.txt
├── include/       # Header files
├── src/           # Application source files
├── tests/         # Test source files
├── scripts/       # Utility scripts
└── docs/          # Documentation
```
