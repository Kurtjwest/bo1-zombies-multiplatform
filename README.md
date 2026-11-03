# BO1 Zombies

Decompiled engine code for Black Ops 1 Zombies, so mods can change the engine itself, not just scripts and maps.

![](images/1.webp)

![](images/2.webp)

## Building

The engine builds for Windows (64-bit and 32-bit), macOS (Apple silicon and Intel) and Linux (x86_64, x86, arm64 and
armhf), each a native program of its platform; the sections below say how.

| Platform | Script | Output |
| --- | --- | --- |
| Windows x64 / x86 | `./build-windows.sh [--arch i686]` (clang and MinGW-w64), or the Visual Studio project (32-bit) | `build-windows-x64/BO1Zombies.exe`, `build-windows-x86/`, `build/Release/` |
| macOS Apple silicon / Intel / both | `./build-mac.sh [--arch x86_64 \| --universal]` | `build-mac/BO1Zombies.app` (`build-mac-x86_64/`, `build-mac-universal/`) |
| Linux x86_64 / x86 / arm64 / armhf | `./build-linux.sh [--arch i686 \| aarch64 \| armhf]` | `build-linux/BO1Zombies` (`build-linux-<arch>/` for another processor) |

A Release build (the scripts' default) is built as the retail game's release build was: without the engine's
assertion checks (the decompiled code has about 13,900 of them, which the retail build compiled away) and with
whole-program optimization (ThinLTO, with clang 20 or later or a recent Xcode; an older compiler builds without it,
and the configure step says so; the retail game used MSVC's LTCG). The Visual Studio project builds with its own
settings, the checks included. The scripts' other options:

- `--no-lto`: no whole-program optimization, a much quicker link after a small change (for working on the engine).
- `--asserts`: keep the assertion checks in a Release build (a failed one stops the game with its text, as in Debug
  builds, which always have them).
- `--cpu v2|v3|v4` (64-bit Intel/AMD builds): compile for a newer processor generation, which makes the game a little
  faster and stops it from starting on older processors (it says so instead of crashing): v2 SSE4.2 (Intel since
  2009, AMD since 2011), v3 AVX2 (Intel Core since 2013, AMD since 2015, every Intel Mac that runs macOS 13), v4
  AVX-512. Without it the game runs on any x86-64 processor, as before.
- `--pgo` (macOS, Linux): profile-guided optimization. The script builds the game a second time with counters in it
  (`build-<platform>-pgo-gen/`), lets its test bot play Kino der Toten and Five for two minutes each in a window
  (`tools/pgo-train.sh`; the game files must be set up), and builds the game with what that recorded, so the compiler
  lays out and inlines the code by what actually ran. It takes about three times as long as a normal build; the
  profile is kept in `pgo/`, and `--pgo-profile pgo/<file>.profdata` builds with it again without the training
  (after a change to the code, the functions it changed go without their part of the profile until the next `--pgo`;
  `--cpu` makes no difference to it). On
  Windows the training runs on Windows: `build-windows.sh --pgo-generate`, then `tools\pgo-train.bat` in that build's
  folder (after `setup.ps1 -Build build-windows-x64-pgo-gen`), then `build-windows.sh --pgo-profile
  build-windows-x64-pgo-gen`; the toolchain needs its profile runtime (llvm-mingw and MSYS2's clang have it, Debian's
  MinGW packages do not).

None of these changes how the game plays (it computes the same; the scripted test bot plays a map out identically).
Neither do the engine's own performance changes, in every build: idle job threads sleep until work comes (the retail
engine woke each one a thousand times a second to look), map loads decompress with zlib-ng in the clang builds (about
a third faster), the server skips comparing an unchanged entity's fields for every snapshot and writes snapshot bits a
byte at a time, and on Windows the server thread stays on the performance cores of a hybrid Intel CPU
(`bo1_mod_pcore 1`, the default there; `0` turns it off). In the 64-bit builds a null pointer in the engine's 32-bit
pointer fields is stored and read without a function call (millions a second). In the macOS and Linux builds the
renderer waits for the GPU instead of spinning while the engine waits for a query (the sun and coronas read their
visibility two frames later), and a big dynamic buffer that is refilled every frame (the 8 MB skinned vertex cache)
keeps its memory instead of getting new pages each frame. On the Mac, the anti-aliased back buffer, which the engine
resolves several times a frame while it keeps drawing into it, is resolved by the render pass that drew it instead of
a pass of its own that reloads and stores every sample again (at 2880x1800 with 4x MSAA, 83 MB each way), and a
resolve repeated with nothing drawn in between is skipped (`BO1_METAL_RESOLVE_IN_PASS=0` turns both off, to compare;
`BO1_METAL_CHECK_RESOLVES=1` compares their results with separate resolves once a second). `+set bo1_mod_workers N` on the command line starts 1 to 8
job threads instead of retail's two (a profile on an eight-core Mac found two plenty: the frame waits for the render
thread and the GPU, not for the jobs).

`tools/benchmark.sh` (macOS, Linux) measures a build: the test bot plays a map in a window for three minutes (`--map`,
`--seconds`), with no frame limit and no vsync for the run (your settings stay as they are), and the game prints its
frame rate, frame time percentiles, slow frames, and how long the render thread waits for the GPU each frame (and on
Metal how busy the GPU is), which says whether the GPU or the CPU side sets the frame rate, into
`benchmarks/<date>-<map>/summary.txt`; `--profile` also records where the time goes (macOS `sample`, Linux `perf`),
and `--app` runs another build, so two builds compare on one computer. On Windows the same run is `BO1Zombies.exe`
with the arguments `tools\pgo-train.bat` uses.

## Windows (64-bit and 32-bit)

The engine is a Windows program, and runs on Windows' own APIs there: Win32, Direct3D 9, DirectSound, XInput and
Winsock. Two builds make it:

- `build-windows.sh` builds the 64-bit (x64) game, and with `--arch i686` the 32-bit (x86) one, with clang and
  MinGW-w64. It runs on Windows in an MSYS2 or Git Bash shell, and on Linux and macOS as a cross build. It needs clang
  and lld with MinGW-w64 ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw) on any of them, with its `bin` folder on
  `PATH`; Debian/Ubuntu: `sudo apt install clang lld mingw-w64`; MSYS2: `pacman -S mingw-w64-clang-x86_64-toolchain`),
  CMake, Ninja, git and make. The first build downloads and compiles FFmpeg and libvpx.
- The Visual Studio project (`cmake -A Win32`, CMakeLists.txt) builds the original 32-bit game with MSVC and the
  June 2010 DirectX SDK (`-DDXSDK_DIR=...` for an extracted copy).

```sh
./build-windows.sh                    # build-windows-x64/BO1Zombies.exe
./build-windows.sh --arch i686        # build-windows-x86/BO1Zombies.exe
```

The game runs in its build folder, with the original game's files next to it, as the Visual Studio build does: on
Windows `powershell -NoProfile -ExecutionPolicy Bypass -File setup.ps1 -Build build-windows-x64 [-GameDir "D:\...\Call of Duty Black Ops"]`
copies them there once (`zone`, `main`, `localization.txt` and the install's `mods`, if it has one; and D3DX9's
`d3dx9_43.dll` out of the game's `Redist` folder), then `Play.bat` (or the
executable with its own arguments) starts it. The build puts the repository's `mods/` and `main/` (`data/main/`) next to
the executable.

What the clang/MinGW-w64 build uses instead of the DirectX SDK and the 32-bit-only libraries of the Visual Studio one:
XAudio2 is the software XAudio2 of the macOS and Linux builds (`src/platform/common/xaudio2_soft.cpp`), played through
WASAPI (`src/platform/windows`), so no XAudio2 DLL is needed; Bink movies play with `binkw32.dll` on x86 and, as RAD
made no 64-bit Bink 1, with FFmpeg on x64; Steam is `steam_api.dll` on x86, and on x64 a stand-in `steam_api64.dll`
without Steam (Valve's own `steam_api64.dll` from the Steamworks SDK can replace it); NVAPI is absent (the renderer
takes its non-NVIDIA paths); XInput is Windows' own `xinput1_4.dll`. The executables need no runtime DLLs of their own.
In the 64-bit game the engine's 32-bit pointer fields hold handles into a 3.25 GB arena, as in the 64-bit macOS and
Linux builds (`src/universal/ptr32.h`). As in the other builds, a controller moves through every menu and the main
menu offers Zombies, Options and Quit (see the macOS section).

## macOS (Apple silicon and Intel)

The engine also builds as a native Mac app, for Apple silicon (arm64) and Intel (x86_64) Macs. Nothing is emulated or translated at run time: the Win32 API
calls the engine makes go to `src/platform/win32compat` and `src/platform/macos` (POSIX, Mach and Cocoa),
Direct3D 9 runs on Metal (`src/platform/d3d9mtl`, with the game's shaders translated to Metal by MojoShader),
XAudio2 on Core Audio, XInput on Game Controller, and Bink videos and xWMA sounds are decoded with FFmpeg.

Needs macOS 13 or later (Apple silicon, or an Intel Mac with a Metal GPU), the Xcode command line tools
(`xcode-select --install`), CMake and Ninja (`brew install cmake ninja`), and the game files of a Call of Duty: Black Ops
PC install (Steam or retail), copied from Windows.

```sh
./build-mac.sh                                        # builds build-mac/BO1Zombies.app for this Mac's processor
./setup-mac.sh "/path/to/Call of Duty Black Ops"      # one time: copies zone/, main/, ... to the game folder
open build-mac/BO1Zombies.app                         # or double-click it
```

`./build-mac.sh --arch x86_64` builds the Intel app on any Mac (in `build-mac-x86_64`; `--arch arm64` the Apple
silicon one), and `./build-mac.sh --universal` both, joined into one app that runs natively on either
(`build-mac-universal/BO1Zombies.app`). On an Apple silicon Mac the Intel app runs under Rosetta
(`arch -x86_64 build-mac-x86_64/BO1Zombies.app/Contents/MacOS/BO1Zombies`).

The first build downloads and compiles FFmpeg and libvpx (for offline builds pass
`-DBO1_FFMPEG_SOURCE_DIR=...` / `-DBO1_LIBVPX_SOURCE_DIR=...` to CMake). Started without arguments the app opens
the zombies menu the way `Play.bat` does on Windows; engine arguments work as on Windows
(`build-mac/BO1Zombies.app/Contents/MacOS/BO1Zombies +set bo1_zombies 1 +set r_fullscreen 1 ...`).

The game folder is `~/Library/Application Support/BO1Zombies`. The game reads the original game's files there and
writes everything there (settings and saves in `players/`, logs, screenshots, the shader cache), and nowhere else, so
the app runs from any folder with nothing next to it (drag it to /Applications, say). The app holds only the game's
code: the repository's own files (its mods, the folders of `mods/` that git tracks, and `data/main/`) are installed in
the game folder by every build, and by `./setup-mac.sh` (`tools/install-game-data.sh`): change them in the repository
and rebuild (the files that differ are copied; the ones the repository no longer has are removed unless you changed
them; links in the game folder are left alone). Mods of your own, a map mod say, go only in the game folder's `mods/`:
anything else in the repository's `mods/` is left out. `-DBO1_INSTALL_GAME_DATA=OFF` turns the install off for a build,
and a build cross-compiled on Linux leaves it to `./setup-mac.sh` on the Mac.
Of the install, setup takes only what the game reads: `zone/`, `main/` (the `.iwd` files and the videos),
`localization.txt` and its `mods/` if it has one; its Windows programs and DLLs, `Redist`, `players/` (the Windows
game's settings), Steam's, the uninstaller's and other launchers' files stay out.
`./setup-mac.sh --move "/path/to/Call of Duty Black Ops"` moves the game files instead of copying them (instant on the
same disk). Earlier versions of `setup-mac.sh` put the game files next to the app, in `build-mac`, where the game also
kept its settings and saves: `./setup-mac.sh` on its own moves all of that to the game folder (and leaves the build,
with its old copy of `mods/`). Every run also removes from the game folder what earlier versions put there that the game
has no use for: a build's own files (zlib-ng, its source, the ThinLTO cache) and the parts of an install now left out.
`--gamedir <path>` runs the game on another folder (for testing), which then holds what the game writes.

Controls are the Windows ones. The menus drawn on the front end's TV screens have no cursor, as on Windows: the mouse
looks around the room, the scroll wheel (or a two-finger swipe on a trackpad, one item per short swipe) or W/S moves
through the items, a click or D chooses, a right-click (a two-finger click on a trackpad) or A goes back. A
controller moves through every menu with the d-pad or the left stick (held, they scroll on), chooses with A and goes
back with B. The main menu offers Zombies, Options and Quit, in every build: Campaign and Multiplayer are the retail
game's own programs (BlackOps.exe, BlackOpsMP.exe), which this engine does not replace.

The game starts in macOS full screen (a Space of its own; the game's own full-screen setting, `r_fullscreen`). The
display's mode never changes: the game draws at the resolution chosen in its graphics options (`r_mode`), anything
from 640x480 up to the display's full Retina resolution, and the screen shows it scaled. The default is the screen's
size in points (1440x900 on a 13" MacBook Pro); a higher one is sharper and costs the GPU more. With full screen off
the game is in a window of that size in points (as large as the screen at most); a 720x480 window shows a 4:3 picture,
the way a standard-definition TV does, unless the aspect ratio option says otherwise. The green button and Ctrl-Cmd-F
(Window > Enter Full Screen) take the game window in and out of full screen at any time.

Metal compiles the game's shaders when they are first drawn. Instead of stalling the frame meanwhile, the game draws
the frames without what is not compiled yet (a new place or effect shows a moment later the first time), and keeps a
record of what it compiled (`shadercache/` in the game folder): the next time a level loads,
that is compiled in the background before it is needed.

The log (with the `BO1_*` settings below) goes to the terminal when the app is started from one:
`BO1_PERF_LOG=1 build-mac/BO1Zombies.app/Contents/MacOS/BO1Zombies`.

Settings for the Mac build:

| Environment variable | Effect |
| --- | --- |
| `BO1_HIDPI=1` | window sizes (and the screen size the game sees) in pixels instead of points: a window at the display's full Retina sharpness, half as large |
| `BO1_METAL_FRONT_CCW=1` | flip the triangle winding Metal treats as front-facing (if geometry renders inside out) |
| `BO1_DUMP_FAILED_SHADERS=1` | write shaders Metal rejects to `shaderdumps/` in the game folder (with the error in the log) |
| `BO1_SYNC_SHADERS=1` | draws wait for their shaders to compile instead of being skipped (no pop-in, but stalls) |
| `BO1_SHADER_LOG=1` | log what the shader compiler does in every frame that compiled something |
| `BO1_METAL_INTEL_GPU_PATH=1` | use an Apple GPU the way the Intel and AMD GPUs of Intel Macs are used (textures in private memory, written through the GPU; no 16-bit 5:6:5 format): to test that path on Apple silicon |
| `BO1_METAL_RESOLVE_IN_PASS=0` | resolve the anti-aliased back buffer with a render pass of its own, as before, instead of at the end of the pass that drew it (to compare) |
| `BO1_METAL_CHECK_RESOLVES=1` | once a second, compare each resolve made at the end of a pass (and each skipped repeat) with a separate resolve of the same buffer, and print the count of identical and different ones (slow while it compares) |
| `BO1_PERF_LOG=1` | log once a second where the frames' time went: frame rate, GPU time (Metal: the time the GPU was busy), waits for the GPU and for the display (v-sync), draws and render passes per frame |

Cross-compiling from Linux (clang, lld and a macOS SDK) works too: see `cmake/toolchain-macos.cmake` (`toolchain-macos-arm64.cmake`, `toolchain-macos-x86_64.cmake`).

## Linux (x86_64, x86, arm64 and armhf)

The same engine builds as a native Linux executable for 64-bit and 32-bit x86 and ARM. As on macOS nothing is emulated:
the Win32 calls go to `src/platform/win32compat` (POSIX), the window, keyboard, mouse, controllers and sound to SDL2
(`src/platform/linux`), and Direct3D 9 runs on OpenGL 4.5 (`src/platform/d3d9mtl` over `src/platform/opengl`, with
the game's shaders translated to GLSL by MojoShader). Bink videos and xWMA sounds are decoded with FFmpeg.

Needs clang 14 or later and its lld (the linker of the same LLVM version, for whole-program optimization; without it
the build links without), CMake 3.16+, Ninja, pkg-config and the SDL2 development files, git and make (Debian/Ubuntu:
`sudo apt install clang lld cmake ninja-build pkg-config libsdl2-dev git make`; Fedora:
`sudo dnf install clang lld cmake ninja-build pkgconf SDL2-devel git make`; Arch: `sudo pacman -S clang lld cmake ninja
pkgconf sdl2 git make`), an OpenGL 4.5 driver to run (Mesa 20 or later for Intel, AMD and the software renderer, or
NVIDIA's), and the game files of a Call of Duty: Black Ops PC install (Steam or retail).

```sh
./build-linux.sh                                       # builds build-linux/BO1Zombies for this computer's processor
./setup-linux.sh "/path/to/Call of Duty Black Ops"     # one time: copies zone/, main/, ... to the game folder
build-linux/BO1Zombies
```

`./build-linux.sh --arch i686` (or `x86_64`, `aarch64`, `armhf`) cross-compiles for another processor into
`build-linux-<arch>`, with clang and that processor's SDL2 development files: on Debian and Ubuntu its multiarch
packages (`sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install libsdl2-dev:i386 libstdc++-14-dev:i386`
for x86, `arm64` / `armhf` likewise), or a sysroot named by `BO1_SYSROOT` (see `cmake/toolchain-linux.cmake`). On a
Raspberry Pi or another ARM computer `./build-linux.sh` builds for it directly. To run there, the GPU's driver has to
offer OpenGL 4.5 too: Mesa's freedreno (Adreno 6xx) and asahi (Apple M1/M2) do, the Raspberry Pi's v3d and Mali's
Panfrost don't, and without one the game runs on Mesa's software renderer (llvmpipe), slowly.

The game folder is `~/.local/share/BO1Zombies` (`$XDG_DATA_HOME/BO1Zombies`). As on macOS the game reads the original
game's files and writes everything there (settings, saves, logs, screenshots, the shader cache), and the repository's
`mods/` and `data/main/`, which the build puts in `gamedata/` next to the executable, are installed into it when the
game starts. Keep `gamedata/` and `libsteam_api.so` next to `BO1Zombies` when moving it. `setup-linux.sh` takes from the
install only what the game reads, as on macOS (`--move` moves it instead), and removes the parts of an install that
earlier versions copied and the game has no use for. `--gamedir <path>` runs the game on another folder. Names in the
game files are found whatever their case, so a copy from a Windows disk works as it is. Started without arguments the game opens the zombies menu; engine arguments work as on Windows
(`build-linux/BO1Zombies +set r_fullscreen 1 ...`).

The game runs in a window or full screen (`r_fullscreen`, or the graphics options) on X11 and Wayland; the display's mode never
changes, the game draws at the resolution of its graphics options (`r_mode`, 640x480 and up, 720x480 included) and the
picture is scaled to the window.
OpenGL compiles the game's shaders on background threads the way Metal does on the Mac (see above, `shadercache/`).

Settings for the Linux build, besides `BO1_DUMP_FAILED_SHADERS`, `BO1_SYNC_SHADERS`, `BO1_SHADER_LOG` and
`BO1_PERF_LOG` (see the macOS table):

| Environment variable | Effect |
| --- | --- |
| `BO1_GL_DEBUG=1` | an OpenGL debug context; the driver's errors and warnings go to the log (`2`: its performance hints too) |
| `BO1_GL_LOG=1` | log what the renderer's start-up self-test measured (clip space and winding conventions) |
| `SDL_VIDEODRIVER=wayland` / `x11` | the window system SDL uses (SDL's own setting) |
| `SDL_AUDIODRIVER=pipewire` / `pulseaudio` / `alsa` | the sound system SDL uses (SDL's own setting) |

## Co-op

Two to four players, each on their own computer, play a zombies map together, as in the original game's co-op, on a
local network or over the internet. The computers can be any mix of the Windows, Mac and Linux builds of this engine,
as long as each has the map and all run the same version of the game (and the same mods). One player hosts: the game
runs on the host's computer, and the others join it.

It is all in the Zombies menu's **Online** choice (**Solo** is a game alone, as before):

1. The host picks **Host Game**, then the map (Solo's mission select). The level waits at its start, on a black
   screen, for up to four players: the screen says how many have joined and shows the address the others join (the
   host computer's IP address on the local network). It starts by itself once four are in, or when the host presses
   the Use key (the screen says which: "Press F to start").
2. The others pick **Join Game**, type that address and press Enter, once the host is on that waiting screen. An
   address without a port (`192.168.1.20`) joins port 3074, the game's; one with a port (`192.168.1.20:3075`) joins
   that one.

Each player's points show at the bottom right, in that player's colour (white, blue, yellow, green), yours larger, as
in the original game; the Tab scoreboard lists everyone.

If no game answers at the address, joining stops after about half a minute with "Lost connection to host", and the
zombies menu comes back when the message is closed (Esc, or its Exit).

Four is the most: the levels' scripts give each of four players a character (model, arms, voice), a start position
and a place in tables such as Kino's teleporter destinations, so a fifth player would have no body and break them.

The game uses UDP port 3074 (the next free one, 3075 and up, if it is taken, as by a second copy of the game on the
same computer: the host's waiting screen shows the address with its port then). Hosting or joining opens it to the
network, where it otherwise answers only on the computer itself, so the first time the system may ask whether the
game may accept incoming connections: allow it. Over the internet the others join the host's public IP address (a
site such as whatismyip.com shows it), and the host's router has to forward UDP port 3074 to the host's computer (its
"port forwarding" settings); a virtual network that puts the computers on one network (ZeroTier, Tailscale) works
too, with the address it gives the host. As in the original game the level runs on the host's computer at the host's
frame rate, so the fastest computer, with the best connection, should host.

In a game, the console (the `~` key, below Esc; its commands start with a `/`, what doesn't is sent as a chat
message) has the rest: `/disconnect` leaves a game, and joining again comes back into it (with no points, as anyone
joining a game in progress); when the host leaves, the game ends for everyone. `/name Kurt` sets the name the others
see over your head. The console can also host and join without the menus: `/set bo1_coop_players 4` then
`/map zombie_theater` hosts (a smaller number starts the level once that many are in; the number lasts until the
game is closed, and Solo sets it back to 0), and `/connect 192.168.1.20` joins.

A computer can also host without playing, as a dedicated server:
`BO1Zombies +set dedicated 1 +set bo1_coop_players 2 +map zombie_theater` (it waits for one player unless told more).
It starts the map again when the game is over or the last player leaves.

A joining player compiles the host's level scripts too, to build the same animation trees; if the two differ (another
version of the game or of a mod), joining stops with a message saying so.

To try co-op on one computer, `./tools/coop-test.sh` (macOS and Linux) runs two copies of the game side by side, each
in a window and on a game folder of its own (made the first time in `~/BO1Zombies-coop`, with links to your game
files, so your own game folder and settings stay as they are), the left one on port 3074 and the right one on 3075.
Host in the left window (Online, Host Game, a map), join from the right one (Online, Join Game, `127.0.0.1`), then
press F in the left one to start. Cmd-Tab (Alt-Tab) switches between them: only the window in front gets the keyboard
and mouse.

## Mods

The main menu's **Mods** (between Zombies and Settings) lists the mods in the game folder's `mods` folder: each row
says whether that mod is on. Left and right (arrow keys, d-pad, left stick) turn the selected mod off and on; Enter, a
click or the gamepad's A opens its page: what it does, the maps it plays on, ON / OFF, and its settings (Enter, A,
left and right change the selected one). The choices are saved with your settings, and every game you start from the
menus (Solo, Host Game, Join Game) gets the mods that are on: they are stacked (`fs_mods`) in their order when the map
loads. A mod made for some maps only is left out of a game on another map (the console says so). Back on the main
menu the game is the original again.

| mod | what it does | maps |
|---|---|---|
| Horde | rounds don't wait for the horde; how many zombies may be alive at once (24 to 1000), start round, spawning from every window | Five, Kino der Toten |
| Nightmare | every special enemy from the first round (Nightmare or Hell); needs Shangri-La, Call of the Dead and Moon installed (and Ascension for Kino) | Five, Kino der Toten |
| Zombie counter | holding Tab shows the zombies alive and left this round | every map |
| No perks | no perk machines | every map |
| Sandbox | Five as a test area: everything open and pads that give perks, power-ups and weapons | Five |

Some settings need memory or pools that are set aside when the game starts: Nightmare's extra maps, Horde with more
than 100 zombies. The menu then says "Restart the game to finish turning on ...", and saves what they need in
`players/mods_startup.cfg`, which the next start reads. Until the restart a game leaves such a mod out, or for a
setting uses the largest value the running game has room for (Horde: 100 zombies).

In co-op the host's mods run the game, but every player should turn on the same ones: Nightmare's extra maps load on
each computer.

A command line that picks mods itself (`+set fs_mods ...` or `+set fs_game ...`, as `tools\mod-launcher.hta` does)
wins: the menu's choices are not used then (the Mods screen says so). A command line that starts a map (`+map`,
`+devmap`) or a headless run doesn't read `players/mods_startup.cfg` either. In the console, `/bo1_mods` lists the
mods and their settings, and `/bo1_mods zombie_theater` also shows what a game on that map would get.

### Map mods

A custom zombies map made with the Black Ops mod tools, such as Perish, comes as a folder: the map's zone
(`zombie_perish.ff`), usually `mod.ff` (its own scripts, weapons and sounds), `.iwd` files, `video/<map>_load.bik` and
`mod.json`. Put the folder as it comes in the game folder's `mods` (on Linux the repository's `mods` works too: the
build puts it in `gamedata/`, and the game copies it from there once). It needs no `mod.txt`: the Mods menu lists it
as MAP, with the name and description from `mod.json`, and its page has **PLAY SOLO** and **HOST GAME** (co-op as in
Co-op above: the others pick Join Game, and each needs the map in their own `mods` folder).

A map needs more memory than the game sets aside for the original maps (the menu works it out from the sizes of its
zones). The first time the menu sees it, it saves that in `players/mods_startup.cfg` and asks for one restart; after
that every start sets it aside. When the map loads, its folder is stacked with the other mods that are on and play on
every map (Zombie counter, No perks); mods made for other maps are left out. Its zones, `.iwd` files and videos come
from its folder; its `mod.ff` loads with the map, just before it, and is freed with it (as with retail's `fs_game`, its
scripts and assets win over the game's patch zones). Its own loading screen picture isn't shown (it is in `mod.ff`,
which loads after the loading screen is set up).

From the console: `/map zombie_perish` (once the main menu has loaded). On the command line, retail's way to play a map
mod: `+set bo1_zombies 1 +set fs_game mods/Perish +set bo1_mod_memory 384 +devmap zombie_perish` (without the menu,
`bo1_mod_memory` is the memory in MB to set aside; `/bo1_mods` shows what the menu works out). A map is made against the
final, patched game (its scripts call functions the patch zones added) and can use images, sounds and models of the
original maps and DLC, so the whole game, updated, has to be installed.

### Adding a mod

A mod is a folder in `mods` (the repository's, which the build installs in the game folder, on macOS once git tracks a
file of it, or the game folder's own)
whose scripts hook into the game through `maps/<folder>/_hooks.gsc` (`mods/_stack/maps/_modstack.gsc` explains the
hooks), plus a `mod.txt` that puts it in the menu. The folder's name is the mod's name in `fs_mods`: letters, digits
and `_`. The smallest `mod.txt` (mods/zinfo's):

```
name        "Zombie counter"
description "While you hold Tab (the scoreboard), ..."
order       30
set zinfo 1
```

The words of a `mod.txt` (`//` and `/* */` are comments; text with spaces goes in quotes):

- `name "..."`, `description "..."` (three lines of the page at most).
- `order N`: its place in the menu and in the stack (lower first; the mods' hooks run in that order). Horde 10,
  Nightmare 20, Zombie counter 30, No perks 40, Sandbox 50; 100 if not given.
- `map <map> "<label>"`, once per map it plays on (none: every map). `needs <zone> "<label>" [map <map>]`: a zone file
  that has to be installed (on that map only, if named); without it the mod can't be turned on.
- `set <dvar> <value> [map <map>]`: set when a game starts (on that map only, if named), back to its earlier value
  when a game starts without it. Not for saved settings (`seta`). When two mods set the same dvar, the larger number
  wins, or, for words, every word of each (a zone list).
- `startup <dvar> <value>`: needed when the game starts (memory, pools); the menu asks for a restart if the running
  game started with less. `startup bo1_mod_zones "<zones>"` sets aside memory for that many extra zones.
- `clear <dvar>` or `clear <start>*`: dvars the mod's scripts set (SetDvar) that should be empty again at the start of
  each game, so a choice made in one game doesn't stay for the next.
- `option <dvar> { ... }`: a row of its page, saved in that dvar (the one its scripts read):
  `label "..."`, `help "..."` (shown under the rows), `type toggle | choice | range`, `default <value>`, and its
  values: `value <value> "<label>"` each, optionally followed by `{ set ... startup ... needs ... }` that apply when
  that value is the chosen one (a value whose needs aren't installed, or that needs a restart, plays with the settings
  of the nearest value before it in the list that can); `toggle` without values is OFF 0 / ON 1; `range` with
  `min`, `max` and `step` makes the values (at most 32).

The menu reads the folder again each time the main menu loads. A `mod.txt` with a mistake is left out, and the console
(and `console_mp.log`) says which line.
