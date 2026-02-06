## Common Development Commands

### Building and Running

- `python run.py automat` - Build and run the main Automat application. Warning: it runs automat indefinitely and must be manually stopped

### Development Tools

- `python run.py "link automat"` - Build automat binary without running it (for testing compilation) (build/fast/automat)
- `python run.py "link automat" --variant=debug` - Build debug variant - for running under GDB (build/debug/automat)
- `python run.py "link automat" --variant=release` - Build release variant - it's actually the fastest variant to build for testing (build/debug/automat)
- `python run.py compile-commands.json` - Generate `compile_commands.json` for LSP support
- `python run.py screenshot` - Take a screenshot of running automat (saved to `build/{variant}/screenshot.png`)
- Build dashboard available at http://localhost:8000/ when running build commands
- Custom build system written in Python, located in `run_py/` directory

### Important Notes

- **To test builds**: Use `python run.py "link automat" --variant=release` to verify compilation without running
- **Avoid running automat directly**: The `python run.py automat` command runs automat indefinitely and must be manually stopped

## Architecture Overview

Automat is a C++ application for semi-autonomous game automation with a layered architecture:

### Core Architecture Layers

1. **Objects Layer** - Heart of Automat containing virtual devices that can connect to each other
   - Located in `src/library*.cc/hh`
   - Objects use typed connections defined by `Argument` class
   - Key files: `src/object.hh`, `src/argument.hh`, `src/base.hh`

2. **Widgets Layer** - UI rendering using Skia graphics library
   - Files starting with `ui_*` in `src/`
   - Heavy use of Skia for cross-platform rendering

3. **Platform Frontends** - OS-specific window management
   - Windows: `src/win32_window.cc`
   - Linux: `src/xcb_window.cc`
   - Entry point: `src/automat.cc`

### Key Components

- **Location** (`src/location.hh`) - Central object storage and interaction API
- **Machine** (`src/base.hh`) - Combines multiple objects into a single unit
- **Custom Build System** - Python-based build system in `run_py/`
- **Extensions** - Python modules in `src/*.py` that extend the build system

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
- Uses custom smart pointers and containers
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
