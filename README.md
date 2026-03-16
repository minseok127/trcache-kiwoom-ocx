# trcache-kiwoom-ocx

Shared-memory bridge between 32-bit Kiwoom OCX and 64-bit trcache engine for real-time tick data capture.

## Architecture

```
[32-bit OCX] --push--> [Shared Memory (SPSC Queue)] --pop--> [64-bit Engine] --feed--> [trcache] --flush--> [disk]
```

## Build (Windows, MSVC 64-bit)

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build/engine/Release/kob_engine.exe`

## Usage

1. Start the producer. It must call `kob_create()` to create the shared memory.
2. Start the engine (after the producer has created the shared memory):
   ```powershell
   kob_engine.exe <output_dir>
   ```
3. The producer pushes ticks via `kob_push()`. The engine pops and writes to disk.

Tick data is written to `<output_dir>/<YYYYMMDD>/<symbol>.bin` as raw binary (entry bytes as-is from the queue).

## Entry Layout Protocol

The only constraint on the tick entry is:

- **The first `KOB_CODE_SIZE` (8) bytes must be the null-terminated stock code** (e.g. `"005930\0\0"`).

The engine uses this to route entries to per-symbol streams. It does not interpret the rest of the entry — raw bytes are fed into trcache and flushed to disk as-is.

## Integrating with Producer

```c
#include "shared/kiwoom_ocx_bridge.h"

// Define your own tick structure.
// First KOB_CODE_SIZE bytes must be the stock code.
struct my_tick {
    char    code[KOB_CODE_SIZE];  // must be first
    int32_t time;
    int32_t price;
    /* ... other fields ... */
};

// At startup
struct kob_handle shm = {0};
kob_create(&shm, KOB_DEFAULT_CAPACITY, sizeof(struct my_tick));

// On each tick
struct my_tick entry = {0};
strncpy(entry.code, "005930", KOB_CODE_SIZE);
entry.time  = 93000;
entry.price = 72000;
kob_push(&shm, &entry);
```

## Error Handling

If the SPSC queue becomes full, the error flag is set and all subsequent pushes
fail. The engine detects this and shuts down. A full queue means tick data was
lost — that day's data should be replaced.

## Project Structure

```
shared/              Header-only SPSC queue (32/64-bit compatible)
engine/              64-bit consumer (pop -> trcache -> disk)
trcache/             Submodule: real-time trade data engine
```
