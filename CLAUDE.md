## Common Development Commands

### Building and Running

- `python run.py automat` - Build and run the main Automat application. Warning: it runs automat indefinitely and must be manually stopped

### Development Tools

- `python run.py "link automat"` - Build automat binary without running it (for testing compilation) (build/fast/automat)
- `python run.py "link automat" --variant=debug` - Build debug variant - for running under GDB (build/debug/automat)
- `python run.py "link automat" --variant=release` - Build release variant - fastest variant to build for testing (build/release/automat)
- `python run.py compile-commands.json` - Generate `compile_commands.json` for LSP support
- `python run.py screenshot` - Take a screenshot of running automat (saved to `build/{variant}/screenshot.png`)
- Build dashboard available at http://localhost:8000/ when running build commands
- Custom build system written in Python, located in `run_py/` directory
- Build variants: `fast` (default), `debug`, `release`, `asan`, `tsan`, `ubsan`

### Important Notes

- **To test builds**: Use `python run.py "link automat" --variant=release` to verify compilation without running
- **PipeWire build errors**: If PipeWire fails with "missing libcap.so" after system package updates, delete the stale PipeWire build directory (e.g. `rm -rf build/release/PipeWire`) and rebuild
- **Avoid running automat directly**: The `python run.py automat` command runs automat indefinitely and must be manually stopped

## Architecture Overview

Automat is a C++ application for semi-autonomous automation with a layered architecture:

### Core Architecture Layers

1. **Objects Layer** - Heart of Automat containing virtual devices that can connect to each other
   - Located in `src/library*.cc/hh`
   - Objects use typed connections defined by `Argument` class
   - Objects inherit `ReferenceCounted` (thread-safe ref-counted via `Ptr<T>`) and `ToyMakerMixin`
   - Key files: `src/object.hh`, `src/argument.hh`, `src/base.hh`

2. **Toys Layer** - UI display widgets, separated from Object logic
   - **`automat::Toy`** (`src/toy.hh`) — base for all display widgets, inherits `ui::Widget`
   - **`ObjectToy`** (`src/object.hh`) — base for Object-specific widgets (Shape, Draw, ArgStart, etc.)
   - **`ToyStore`** (`src/toy.hh`) — manages Toy lifetimes, keyed by `(WeakPtr<owner>, Atom*)`
   - **`ToyMaker`** concept — any `Part` with a `Toy` type and `MakeToy(Widget*)` method
   - Widget struct definitions typically live in .cc files; headers only declare `MakeToy`
   - `ConnectionWidget` (`src/ui_connection_widget.hh/cc`) — displays argument connections
   - `SyncConnectionWidget` (`src/sync.hh/cc`) — displays sync belts between Gear and members
   - `LocationWidget` (`src/location.cc`) — manages position/scale animation for a Location

3. **Platform Frontends** - OS-specific window management
   - Windows: `src/win32_window.cc`
   - Linux: `src/xcb_window.cc`
   - Entry point: `src/automat.cc`

### Key Components

- **Location** (`src/location.hh`) - Owns an Object and tracks its position/scale within a Board
- **Board** (`src/base.hh`) - Container for Locations; provides canvas, drop target, connection routing
- **ToyStore** (`src/toy.hh`) - Maps `(owner, atom)` keys to Toy widgets; lives on `RootWidget`
- **Argument** (`src/argument.hh`) - Typed connection between Objects; `ArgumentOf` is its ToyMaker
- **Syncable** (`src/sync.hh`) - Sync interface allowing Objects to act as one via a shared Gear
- **Custom Build System** - Python-based build system in `run_py/`
- **Extensions** - Python modules in `src/*.py` that extend the build system

### VM→UI Communication

Objects (multi-threaded) notify Toys (UI-thread) via `wake_counter`:
- Object bumps `wake_counter.fetch_add(1, relaxed)` on state change
- `ToyStore::WakeUpdatedToys()` scans each frame, comparing counters and calling `WakeAnimation()`
- Toy pulls latest state in `Tick()` by locking the Object

## Project Structure

### Source Code Organization

- `src/` - All C++ source files
- `src/library_*.cc/hh` - Object implementations for different functionality
- `src/*_test.cc` - Unit tests using Google Test
- `assets/` - Resources, textures, fonts, and SKSL shaders
- `third_party/` - External dependencies (Skia, Tesseract, LLVM, etc.)

### Build System Files

- `run.py` - Main build script entry point
- `run_py/` - Custom Python build system implementation
- `build/` - Build outputs organized by variant (`fast/`, `debug/`, `release/`)

### Key Dependencies

- **Skia** - Graphics rendering
- **Tesseract** - OCR functionality
- **LLVM** - Code generation and potentially development tools
- **Google Test** - Unit testing framework
- **RapidJSON** - JSON parsing
- **Various X11/XCB libraries** - Linux windowing

## Development Notes

### Code Style

- Modern C++ (C++26) with custom extensions
- Uses custom smart pointers (`Ptr<T>`, `WeakPtr<T>`, `NestedPtr<T>`) and containers (`Vec<T>`)
- Files use `#pragma maf` directives for build system integration
- Static linking preferred for single-binary distribution
- **Build Variant Detection**: Include `src/build_variant.hh` to access compile-time build variant constants:
  - `automat::build_variant::Debug` - true for debug builds
  - `automat::build_variant::Release` - true for release builds
  - `automat::build_variant::Fast` - true for fast builds
  - Also provides `NotDebug`, `NotRelease`, `NotFast` convenience constants
  - All constants are `constexpr` and suitable for use in `if constexpr` conditions

### Testing

- Use Google Test framework for unit tests
- Test files should be named `*_test.cc` and include `gtest.hh`
- Tests are automatically discovered and built by the build system
- Each test file becomes an individual binary that can be run separately
- Available test targets: `ptr_test`, `arcline_test`, `fmt_test`, `llvm_test`

### Build System Extensions

- Python files in `src/` can define `hook_*` functions to extend the build process
- Build variants controlled by `run_py/build_variant.py`
- Custom pragma directives in source files control compilation and linking
- Extension system allows easy integration of external libraries
