# Setup and build guide

This project ships both the modern `mbxhost` client/REPL and the DOS-side `MBXSRV.EXE` server that talk through a shared directory. The instructions below walk through compiling each piece and how to get the mailbox bridge running.

## 1. Host client (`mbxhost`)

### Requirements

- Any POSIX host (macOS, Linux, Cygwin/WSL) with a working C++17 compiler (`clang++`, `g++`, etc.).
- Optional: CMake + Ninja if you prefer the provided `CMakeLists.txt`.

### Quick build (single command)

```bash
c++ -std=c++17 -O2 -o mbxhost host-os.cpp
```

### CMake (recommended for tooling)

```bash
cmake -S . -B build
cmake --build build
```

The `build/` directory contains the Ninja-generated files and the `mbxhost` executable. Run `./mbxhost shared-dir` or use the `--cmd`/`--timeout` options described in `README.me`.

## 2. DOS mailbox server (`MBXSRV.EXE`)

This DOS executable runs inside DOSBox-X and polls the shared folder for incoming commands. The project is compatible with several DOS toolchains; pick the one that best matches your toolchain already in DOSBox-X.

### Recommended: DJGPP (cross-build from the host)

- DJGPP produces 32-bit protected-mode executables suitable for FreeDOS/DOSBox.
- Assuming you installed DJGPP in `~/.dos/djgpp`, build with:

```bash
~/.dos/djgpp/bin/i586-pc-msdosdjgpp-gcc -Os -s -o build/MBXSRV.EXE dosbox-client.c
```

- Copy `build/MBXSRV.EXE` into the DOSBox shared folder and run it inside DOSBox-X.

### Alternative: OpenWatcom

- OpenWatcom is a native DOS compiler that excels at building low-level utilities.
- From a DOS/Windows command prompt with `wcl` on `PATH`:

```bat
wcl -bt=dos -os -s -zq dosbox-client.c
```

- Move the resulting `dosbox-client.exe`/`MBXSRV.EXE` into the shared folder; DOSBox-X can use it immediately.

### Alternative: Borland C++ 3.x/5.x

- For legacy Borland environments that have `bcc.exe` and `tlink.exe`, compile as:

```bat
bcc -mc -0 -W -eMBXSRV.EXE dosbox-client.c
```

(Adjust flags according to your Borland version. The goal is a console-mode DOS binary named `MBXSRV.EXE`.)

### Shared folder expectations

- The DOS side expects the shared directory to contain:

  - `MBXSRV.EXE` running
  - `MBXJOB.BAT`, `CMD.*`, `OUT.*`, `RC.*`, `STA.TXT`, `LOG.TXT`

- Mount the same shared folder inside DOSBox-X (e.g., `mount z /path/to/shared`).

## 3. Running the bridge

1. Inside DOSBox-X: `Z:` then `MBXSRV` to start the server loop.
2. On the host: `./mbxhost /path/to/shared` for the REPL or `./mbxhost /path/to/shared --cmd "dir" --timeout 8000` for one-shot commands.

The mailbox protocol (CMD.NEW → CMD.TXT → CMD.RUN, etc.) is handled for you; see `README.me` for more details on status/log files.

## 4. Tips

- Keep the shared folder on a **local drive** to avoid timestamp issues.
- If you need stderr capture from DOS commands, set the `MBX_STDERR=1` environment variable before starting `MBXSRV.EXE`.
- Use `mbxhost --cmd "quit-guest"` from the host to tell DOSBox-X to exit cleanly.
