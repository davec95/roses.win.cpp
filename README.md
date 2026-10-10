# roses.win.cpp

Standalone **Windows** port of
[Roses.cpp](https://github.com/davec95/Roses.cpp) /
[Roses.py](https://github.com/davec95/Roses.py) — animated
8-particle rose attractor with CRT phosphor trails.

**One .exe, no installer, no Python, no extra DLLs.** Native Win32
window + GDI rendering, statically linked. The only DLL imports
are GDI32 / KERNEL32 / USER32 / msvcrt — every one ships with
Windows. Double-click `roses.exe` and it runs.

## Files

| File | What |
|---|---|
| `roses.exe` | prebuilt x86-64 Windows binary (commit this and grab it) |
| `roses.win.cpp` | Win32 shell: window, input, threads, GDI blit |
| `roses_core.h` | shared compute core (attractor, planes, tone-map, history) — pure stdlib, same math as the Linux core |
| `test_core.cpp` | native Linux harness that exercises `roses_core.h` with the same threading shape |
| `Makefile.win` | cross-compile from Linux (mingw-w64) or build in MSYS2 |

## Controls (identical to roses.py)

- **Space** next rose (replays forward through history; a new
  random rose only when the cursor is already at the newest)
- **Backspace** previous rose
- **p** phosphor trails on/off (off = solid dots)
- **- / =** phosphor shorter / longer persistence
- **Enter** toggle fullscreen
- **Escape** leave fullscreen
- **q** or the close box quit

## Build

From Linux (cross-compile):

```sh
sudo apt install mingw-w64
make -f Makefile.win        # -> roses.exe
```

On Windows (MSYS2/MinGW):

```sh
g++ -O2 -std=c++20 -static -mwindows -DUNICODE -D_UNICODE \
    -o roses.exe roses.win.cpp -lgdi32 -luser32
```

`-static` folds libstdc++/libgcc/CRT into the exe; the win32
thread model means no winpthread DLL.

## Threading model

Same as Roses.cpp R2: one worker thread per attractor. Worker j
owns particle j and density plane j (plane-major, disjoint
memory) — the hot loop has no mutexes and no atomics. Engine
thread + 8 workers sync twice per tick with auto-reset kernel32
events; all 9 threads then tone-map disjoint pixel slices. The
engine posts the finished frame to the UI thread, which blits a
32-bit DIB and acks before the next tick starts.

## Testing note

The shared core (`roses_core.h`) is fully tested natively on
Linux: `g++ -O2 -std=c++20 -pthread test_core.cpp` runs 30 ticks
with scripted commands (next/back/replay, phosphor toggle,
persistence, resize) and 6 assertions, and a `-fsanitize=thread`
build reports zero data races. The Win32 shell itself compiles
cleanly with `-Wall -Wextra` but has not been executed here
(no wine on the build box) — first real run happens on Windows.

## Known behavioral difference from roses.py

Pressing Space (new rose) keeps the old phosphor trails and lets
them decay, instead of clearing the screen as roses.py does.
This matches Roses.cpp R1/R2 (which David already approved);
say the word and a `create_accumulator` call in `next_rose`
restores the Python behavior.
