# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**trcache-kiwoom-ocx** is a shared-memory bridge between a 32-bit Kiwoom OCX producer and a 64-bit trcache consumer for real-time tick data capture on Windows.

The 32-bit producer (separate repo, not here) pushes tick entries into a lock-free SPSC queue over Windows shared memory. The 64-bit engine in this repo pops entries and feeds them into trcache, which flushes raw tick data to per-symbol binary files.

## Build

Windows only (MSVC 64-bit):

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build/engine/Release/kob_engine.exe`

## Architecture

```
[32-bit Producer] --kob_push--> [Shared Memory SPSC Queue] --kob_pop--> [64-bit Engine] --feed--> [trcache] --flush--> [disk]
```

### Key components

- **`shared/kiwoom_ocx_bridge.h`** — Header-only SPSC queue over Windows shared memory (`CreateFileMapping`/`MapViewOfFile`). Entry-type-agnostic: entry size is set at creation time. The only protocol constraint is that the first `KOB_CODE_SIZE` (8) bytes of each entry must be the null-terminated stock code. Uses compiler barriers (not atomics) — sufficient for SPSC on x86.

- **`engine/main.c`** — 64-bit consumer process. Strips the stock code prefix, feeds the remaining opaque bytes into trcache via `trcache_feed_trade_data()`. trcache's `trade_flush_ops` callback writes raw bytes to `<output_dir>/<YYYYMMDD>/<symbol>.bin`. Candle config is a required dummy (trcache needs at least one); candles never close since `dummy_candle_update` always returns true.

- **`trcache/`** — Git submodule. See `trcache/CLAUDE.md` for its internals. This repo uses it as a library: init with dummy candle config + `trade_flush_ops` for raw tick persistence.

### Data flow in the engine

1. `kob_pop()` → raw entry bytes into `entry_buf`
2. First 8 bytes → `trcache_register_symbol()` / `trcache_lookup_symbol_id()`
3. `entry_buf + KOB_CODE_SIZE` → `trcache_feed_trade_data()`
4. trcache worker threads call `trade_flush()` when internal blocks fill up
5. `trade_flush()` writes to per-symbol `.bin` file (fd cached per symbol)

### Error handling

- Queue full → error flag set by producer, engine detects and `abort()`s with diagnostics
- Symbol table full / trcache feed failure → `abort()` with diagnostics
- Ctrl+C → graceful shutdown via `SetConsoleCtrlHandler`, calls `trcache_destroy()` and closes all file handles

## Code Style

- C11, tab indentation
- snake_case for all identifiers (`kob_` prefix for bridge API, `g_` prefix for globals)
- ASCII box-comment section headers
- Return 0 on success, -1 on error
- Windows-only: no `#ifdef _WIN32` fallbacks, `#error` on non-Windows

## Important Constraints

- The shared header must compile in both 32-bit (producer) and 64-bit (engine) — use fixed-width types only, no pointers in shared memory structures
- The engine must not depend on the tick entry layout beyond the first 8 bytes (stock code)
- trcache requires at least one candle config; the dummy config satisfies this without doing meaningful work
- `trcache/CMakeLists.txt` uses `CMAKE_SOURCE_DIR` for include paths, which breaks when used as a subdirectory — the top-level CMakeLists overrides `INTERFACE_INCLUDE_DIRECTORIES` to fix this
