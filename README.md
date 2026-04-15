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

### Starting the first peer

The first peer starts the document. Use `--first` to skip the initial state sync:

```bash
./build/p2p-editor --first
```

You can optionally specify a port (default is 5000):

```bash
./build/p2p-editor --first --port 5000
```

### Joining as a subsequent peer

Additional peers connect by providing the address of any existing peer via `--peer`:

```bash
./build/p2p-editor --peer 127.0.0.1:5000
```

Use `--port` if you need a non-default port (required when running multiple peers on the same machine):

```bash
./build/p2p-editor --port 5010 --peer 127.0.0.1:5000
```

Multiple `--peer` flags can be provided:

```bash
./build/p2p-editor --port 5020 --peer 127.0.0.1:5000 --peer 127.0.0.1:5010
```

### Port layout

Each instance occupies four consecutive ports starting from `--port P`:

| Port  | Purpose              |
|-------|----------------------|
| P     | CRDT operations (UDP)|
| P+1   | Heartbeat / peer discovery (UDP) |
| P+2   | State sync (TCP)     |
| P+3   | Cursor sync (UDP)    |

### Keyboard shortcuts

| Key                     | Action                          |
|-------------------------|---------------------------------|
| Printable chars / Enter | Insert at cursor                |
| Backspace / Delete      | Delete character                |
| Arrow keys              | Move cursor                     |
| Home / End              | Jump to line start / end        |
| Escape                  | Quit (broadcasts LEAVE to peers)|

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
