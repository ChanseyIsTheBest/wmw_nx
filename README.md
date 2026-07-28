# Where's My Water? — Nintendo Switch port (loader wrapper)

This is a native wrapper / loader that runs the original ARM64 build of *Where's
My Water?* on Switch homebrew. It contains **no game code and no game assets**.
It loads the game's own native libraries (`libwmw.so`, `libfmodex.so`)

## Install & run

You need files from `com.disney.WMW` (the Premium build).

Copy the `.nro` to your SD card (e.g. `sdmc:/switch/wmw/wmw_nx.nro`), then place
your game files next to the `.nro`, in the same folder:

```
sdmc:/switch/wmw
├── wmw_nx.nro
├── libwmw.so                  <- from your APK: lib/arm64-v8a/
├── libfmodex.so               <- from your APK: lib/arm64-v8a/
└── assets/                    <- from your APK: the whole assets/ folder
    └── ... (Data, Textures, Levels, Audio, Sprites, ...)
```

`libwmw.so` and `libfmodex.so` — the 64-bit libraries from `lib/arm64-v8a/` in
your APK. (32-bit `armeabi-v7a` will not work; this wrapper is arm64.)

Optionally, drop a `cursor.png` (≤64x64, transparency supported) to replace the
on-screen cursor with your own.

## Add-on content

The Allie, Cranky and Mystery Duck packs are in-app purchases. On Android the
game asks Google Play which ones your account owns and unlocks them through its
normal restore path. There is no Play Store here, so it reads a list instead and
makes the identical call — nothing is patched and no save data is edited
Uncomment what you own officially in the included purchases.txt file.

## Controls

| Input | Action |
| --- | --- |
| **Touchscreen** | Direct, handheld only |
| **Left stick** | Move the cursor |
| **A / ZR / ZL** | Tap and hold — press, drag, release digs |
| **+** | Toggle the on-screen cursor |
| **−** | Toggle gyro pointing (tilt/turn the controller to aim) |
| **L / R** | Recentre the cursor |
| **D-pad up / down** | Adjust sensitivity of whatever is driving the cursor |

A USB mouse works in both handheld and docked.

## Building

Requires devkitPro with the `switch-dev` group plus these portlibs:

```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib
```

Then `make`. `DEBUG_LOG` in `source/config.h` writes `debug.log` next to the
`.nro`; set it to 0 for a release build, as every logged line is an SD-card
write.

## Legal

No affiliation with Disney or Creature Feep. *Where's My Water?* is © Disney.
This is an independent, non-commercial interoperability wrapper. No game program
code, level data, art or audio ships here or may be distributed with builds.

## Credits

The loader/shim infrastructure (`so_util`, `libc_shim`, `util`, `error`,
`nx_pointer`, `opensles`) derives from the open-source Switch ports of Burger
Shop and Bloons TD 5 by Andy Nguyen, fgsfds and ChanseyIsTheBest, which in turn
build on TheOfficialFloW's Vita/Switch loader lineage — all MIT-licensed. The
portrait rotation approach follows the Papers Please port, and the newlib
file-table locking follows BTD5's analysis. The WMW-specific JNI, platform
callbacks, audio, imports, asset index and main loop in this project are new.
Thanks to everyone in that lineage for making this approach possible.
