# Link: The Faces of Evil — static recompilation for Windows

This project turns the Philips CD-i game *Link: The Faces of Evil* (1993) into a
native Windows program. The game's 68000 machine code is translated ahead of
time into C. A small runtime provides the parts of the CD-i that the code
expects: the CD-RTOS (OS-9) operating system, the CD file manager, the video
decoder and the ADPCM audio processor.

You need your own copy of the game disc as a `.bin`/`.cue` image. No game data
is included.

## Building

Requirements:

* Windows 10 or later
* Visual Studio 2022 with the C++ desktop workload (the MSVC compiler)
* CMake 3.16 or later
* Python 3.8 or later (no extra packages needed)

Steps:

1. Put the disc image in the `rom` folder, for example
   `rom/Link - The Faces of Evil (USA).bin` and its `.cue`.
2. Run `build.bat`.

`build.bat` extracts the program modules from the disc and recompiles them to C
in `gen/`. It then compiles everything with MSVC and copies `linkfoe.exe` to the
project root. A clean build takes about a minute.

## Packaging and installing

`package.bat` builds the game and creates two distribution files in `dist/`.
Neither contains the disc image.

| File | Use |
|------|-----|
| `LinkFacesOfEvil-v<version>-portable.zip` | Unzip anywhere and run. On first start the game asks for the disc image. |
| `LinkFacesOfEvil-v<version>-setup.exe` | Single-file installer. It asks for the disc image during setup and creates Start menu and desktop shortcuts. |

* `package.bat -SkipBuild` packages the current build without rebuilding.
* `package.bat -Publish` also creates a GitHub release `v<version>` with both
  files. It uses the `gh` command-line tool.
* The installer needs Inno Setup 6 (`winget install JRSoftware.InnoSetup`).
  Without it, only the zip is made.
* The version number is set in `project(... VERSION x.y.z)` in `CMakeLists.txt`.
* For an unattended install, run
  `setup.exe /VERYSILENT /DIR="D:\Games\Link" /ROM="D:\path\game.cue"`.

The installer needs no administrator rights. Its default location is
`C:\Games\Link - The Faces of Evil`. Uninstalling leaves `saves\` and
`linkfoe.ini` in place.

### Portable by design

The game writes only inside its own folder, never to AppData or the registry:

* `saves\` holds game saves (NVRAM), save states, display settings and the log.
* `linkfoe.ini` stores the disc image location and the update settings.
* Shaders are built into the executables.
* The C runtime is linked statically, so no Visual C++ redistributable is
  needed. The only DLLs used are ones included with Windows 10 and 11.

Because the game is always portable, it needs no `--portable` flag. The
shortcuts pass one anyway, and the program accepts and ignores it.

### linkfoe.ini

```ini
[game]
; the disc image; relative paths are relative to the game folder
rom=D:\CD-i\Link - The Faces of Evil (USA).cue

[update]
; ask (default), auto (install without asking) or off
mode=ask
; GitHub repository to check; the default is set at build time
repo=owner/name
; written when you choose "skip this version"
skip=v1.2.0
```

### Automatic updates

Each time the game starts, it reads the latest GitHub release of the
configured repository. If the release tag (`vX.Y.Z`) is newer than the running
version and the release has a `*-portable.zip` file, the game offers to
install it:

1. The game downloads the zip into `update\`.
2. The game closes.
3. A PowerShell script replaces the program files, keeping `saves\`, `rom\`
   and `linkfoe.ini`.
4. The script restarts the game.

When the game is offline or no release exists, it starts normally without
any message. The check gives up after a few seconds. `--no-update` skips it,
and so do headless runs.

The default repository is set by `LINKFOE_UPDATE_REPO` in `CMakeLists.txt`. To
use a different one, configure with
`cmake -DLINKFOE_UPDATE_REPO=owner/name` and rebuild.

## Playing

Run `linkfoe.exe`. It looks for the disc image in this order:

1. a path to the `.cue` or `.bin` file given as the first argument;
2. the `rom=` entry in `linkfoe.ini`;
3. a `rom` folder next to the program or the current directory.

If none of these finds the image, the game asks you to choose it and saves your
choice in `linkfoe.ini`.

| CD-i          | Keyboard                       | Xbox-style controller |
|---------------|--------------------------------|-----------------------|
| Thumbpad      | Arrow keys (Shift = faster)    | Left stick / D-pad    |
| Button 1      | Z, Space, or A                 | A                     |
| Button 2      | X, Ctrl, or S                  | B                     |
| Both buttons  | C or Enter                     | X or Start            |
| Inventory (down + button 2) | I or V           | Y or Back             |

| Other key        | Action                           |
|------------------|----------------------------------|
| F2               | Save state to the current slot   |
| F3               | Next save-state slot (1-10)      |
| F4               | Load state from the current slot |
| F5               | Cheat: toggle invincibility      |
| F6               | Cheat: +100 rubies (up to 999)   |
| F7               | Run-ahead: off, 1-6 fields (input lag reduction) |
| F8               | Smooth motion (60 fps sprites) on/off |
| F9               | Cycle aspect mode (see below)    |
| F10              | Cycle filter: sharp / smooth / CRT / nearest |
| Alt+Enter or F11 | Toggle fullscreen                |
| Tab (hold)       | Fast-forward (no frame limiting) |
| Esc              | Quit                             |

### Smooth motion (60 fps sprites)

The game updates the action 16 times a second, so on the original hardware
sprites move in jumps every 3 to 4 frames. With smooth motion (F8, on by
default in the HD edition) sprites glide at 60 fps instead. While the game
draws its sprites, the runtime records every pixel. On the frames between
updates, it shows each sprite at a position interpolated between its previous
and current place, over the correct background, and then restores the game's
memory exactly.

* Gameplay is unchanged. In testing, about 7 minutes of random-input play
  produced bit-identical audio with smooth motion on and off.
* Sprites that appear, disappear or jump (more than 40 pixels), and frames
  where the camera scrolled, are shown exactly as the game drew them.
* Interpolation means sprites trail their true position by up to one game
  update, which averages about 2-3 frames. Movement still begins on the very
  next frame. If you want to offset the extra delay, raise run-ahead (F7).
* Command line: `--smooth 0|1`. `--load-state N` starts the game directly
  from save-state slot N.

### Save states

F2 saves a snapshot of the whole machine and F4 restores it, at any point in
the game, including mid-fight and during cutscenes. There are 10 slots (F3).
They're stored as `saves/state_N.lfs`, about 2 MB each. This is separate from
the game's own save system in the Game Menu.

* A save or load happens at the next frame boundary. If the game is busy
  writing its own save file, the save state is refused with a message.
  During the Philips logo at startup, the request waits until the logo ends.
* Files carry a version check and a checksum. States from a different build,
  and damaged files, are refused without affecting the running game.
* In testing, loading a state in a fresh session reproduced the original
  session frame for frame, both with and without run-ahead.

### Input lag and run-ahead

The original game reacts to the controller 2 to 5 fields (33-83 ms) after a
press, because it updates the action only 16 times a second. Run-ahead
(F7, or `--runahead N`) hides that delay:

1. After every frame the whole machine is snapshotted.
2. The game runs N fields ahead with your current input, and that future frame
   is shown.
3. The snapshot is restored.

Sound comes only from the real timeline. In testing, a run-ahead session's
audio was bit-identical to a normal one, and every displayed frame matched the
normal run's frame N fields later. While the game reads or writes save files
in NVRAM, run-ahead pauses for about two seconds.

Other latency measures:
* At most one frame is queued for the display.
* Audio buffering is about 65 ms.
* The controller is read immediately before each frame is emulated.

### Two editions

`build.bat` produces two programs that share the same game code and save
files:

* **`linkfoe.exe`**: the original look. Its filters are sharp, smooth, CRT and
  nearest.
* **`linkfoe_hd.exe`**: the HD edition. It adds two upscaling filters ahead of
  the classic ones and starts with Cartoon HD:
  * **Cartoon HD** is an edge-directed (xBR-style) upscaler. It redraws the
    staircase edges of the flat-coloured cartoon art as smooth, anti-aliased
    contours at the full output resolution.
  * **Cartoon HD+** adds a de-dithering pass before upscaling, which smooths
    checkerboard-dithered skies and gradients. After upscaling it sharpens
    lines and slightly darkens outlines.

The HD edition keeps its display settings in `saves/settings_hd.ini`, so the
two editions don't overwrite each other's choices.

### Display: 4K and widescreen

The picture is drawn with Direct3D 11 at the full resolution of the window or
monitor, which includes 4K and beyond. Fullscreen is borderless at the
desktop's native resolution. The game itself draws a fixed 384×240 picture (a
4:3 frame), so widescreen comes from these aspect modes:

* **4:3 + blurred sides** (default): the correct picture, with soft, dimmed
  artwork filling the side bars of a 16:9 screen.
* **4:3**: the correct picture with black side bars.
* **Stretch to fill**: fills the screen and distorts the picture.
* **Zoom to fill width**: keeps proportions but crops the top and bottom.

The filters are:

* **Sharp**: crisp pixels without shimmering at any scale.
* **Smooth**: bicubic.
* **CRT**: scanlines and a phosphor mask. It looks best at 1440p or 4K.
* **Nearest**: plain nearest-pixel scaling.

Settings are saved in `saves/settings.ini`. Command-line options:
`--aspect N` and `--filter N` (0-3, in the order listed above),
`--fullscreen`, `--windowed`, `--size 3840x2160` for the window size, and
`--gdi`, which uses the old software renderer if Direct3D isn't available.

The inventory key plays the game's own "hold down, then press button 2"
combination. Invincibility stops all damage and every way Link can die,
including pits and water; after a fall you may need to walk out. The cheats
work by hooking the game functions `hero_hit` and `kill_sprite` (`HOOKS` in
`tools/gen.py`, implemented in `runtime/cheats.c`) and by writing the ruby
counter.

In the game, button 1 swings the sword and confirms menu choices. Button 2 is
the action button: it opens doors and the inventory subscreen. On the overworld
map, the thumbpad moves the cursor over the island.

Saved games are kept in the `saves` folder next to the executable, in the
`nvr_*` files that stand in for the CD-i's non-volatile RAM.

## How it works

### Recompiler (`tools/`)

* `cdi_extract.py` reads the Green Book filesystem from the raw 2352-byte
  sectors.
* `m68k.py` is a complete MC68000 instruction decoder. The CD-i's 68070 CPU is
  compatible with it.
* `analyze.py` finds all code by recursive descent from the module entry
  points. It understands:
  * the program's MPW-style A5 jump table;
  * compiler `switch` tables;
  * Microware `move.l #off,dN / jsr (pc,dN.l)` calls;
  * signal intercept routines.

  It also recovers the original function names, which are embedded as MacsBug
  symbols.
* `gen.py` turns every function into a C function. Each one starts at a given
  68k address and returns the address where execution continues. It uses
  whole-program flag-liveness analysis, so condition codes are only computed
  when something reads them.

Two modules are recompiled:

* `cdi_link`, the game itself, about 39,000 instructions;
* `cdi_bumper`, the Philips logo animation that the game runs as a child
  process.

### Runtime (`runtime/`)

* `cpu.c` holds the dispatcher. It maps 68k addresses to recompiled functions.
  Native C calls are used for direct calls, with a safe fallback through the
  dispatcher for returns, indirect jumps and signal delivery.
* `interp.c` is a 68000 interpreter. The game loads "compiled sprite" routines
  from disc into RAM at run time, so those run in the interpreter.
* `os9.c` emulates the CD-RTOS kernel at a high level:
  * memory allocation and process creation;
  * a fork/wait/exit model for running child processes;
  * signals and intercept routines;
  * alarms;
  * path I/O and the configuration descriptor (`/nvr/csd`).
* `cdfm.c` covers the CD file manager. Real-time `SS_Play` follows Play Control
  Blocks and Lists at 75 sectors per second, with channel selection, EOR,
  trigger and EOF handling.
* `video.c` covers the UCM display-control calls (FCT/LCT) and an MCD212-style
  renderer. It supports:
  * CLUT4, CLUT7, CLUT8, RL3, RL7, DYUV and RGB555 images;
  * transparency and mattes;
  * image contribution factors, mixing and mosaic;
  * the hardware cursor.

  The pointer device is handled here too.
* `audio.c` provides the ADPCM decoder (levels A, B and C), sound maps with
  gapless `SM_Out` chaining, and attenuation.
* `host_win32.c` handles the window, GDI output, keyboard and XInput, waveOut
  audio and frame pacing at 60 Hz (NTSC).

Emulated time advances with the number of executed instructions. Video fields,
disc sectors, alarms and the end of each sound map are scheduled on that clock,
and the clock is synchronized to real time once per field.

### Tests

* `python tools/gen_tests.py` emits each distinct instruction of the game as a
  standalone recompiled function. The `insn_test` target then compares each one
  against the interpreter on randomized machine states:

  ```
  cmake --build build\cmake --config Release --target insn_test
  build\cmake\Release\insn_test.exe rom\<image>.bin 200
  ```

* `python test/gen_alu_vectors.py` builds vectors from an independent bit-serial
  model of shifts, rotates and BCD. The `alu_test` target checks the runtime's
  helpers against them.

### Debug options

* `--headless` runs without a window or audio, as fast as possible.
* `--frames N` exits after N video fields.
* `--dump N` writes every Nth frame as a BMP to `saves/`.
* `--wav file.wav` records the audio output.
* `--script "field:key:dur,..."` scripts input. Keys are `1`, `2`, `L`, `R`,
  `U` and `D`.
* `--fuzz SEED` drives the game with random input.
* `--trace` and `--trace3` log system calls, signals and disc activity to
  `saves/recomp.log`.

## Reference

The CD-i Full Functional Specification (the Green Book, May 1994) was the
reference for the CD-RTOS calls, the play control structures and the video and
audio formats.
