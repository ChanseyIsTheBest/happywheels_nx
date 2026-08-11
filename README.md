# Happy Wheels — Nintendo Switch port (Cocos2d-x 3.17.2 wrapper)
 
This is a native wrapper / loader that runs the original ARM64 Android build of *Happy Wheels* on Switch homebrew. It contains **no game code and no game assets** — it loads the game's own library and recreates, natively, the thin Android/JNI layer the Cocos2d-x engine expects.
 
## Install & run
 
You need files from Happy Wheels 1.1.3.
 
Put the `.nro` in any folder under `sdmc:/switch/` and place your game files next to it — the loader finds its folder at runtime, so the name is up to you:
 
```
sdmc:/switch/happywheels
├── happywheels_nx.nro
├── libMyGame.so
├── cursor.png                              <- optional
└── assets
```
Launch via title override (hold R while starting an installed game) or a forwarder. Applet mode won't work; the loader needs the full memory of a game override.
 
Optionally drop a `cursor.png` (up to 64×64, transparency respected) in the same folder to replace the on-screen cursor with your own.
 
## Controls
 
| Input | Action |
|---|---|
| Touchscreen | Direct multi-touch — the game as designed (handheld) |
| Right stick click | Toggle the on-screen cursor |
| ZR | Jump / stop — and tap at the cursor while it's up |
| Left stick ←/→ | Move left / right |
| Right stick ←/→ | Lean left / right |
| D-pad | The four pose buttons — each direction presses the one in that direction |
| B | Grab |
| L / R | Pause / eject |
| X | Retry |
| A + `+` | Toggle learn mode |
 
The cursor is off by default and works docked as well as handheld. It's modal: while it's up the gameplay bindings go silent, because the left stick can't drive both the cursor and the move controls, and a held button would otherwise press whatever sits at its HUD position in a menu. That's also why ZR can be both jump and tap — the two never overlap.
 
## Building
 
Requires devkitPro with the `switch-dev` group plus these portlibs:
 
```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 \
          switch-freetype switch-harfbuzz switch-libpng \
          switch-zlib switch-bzip2
 
export DEVKITPRO=/opt/devkitpro
make                        # -> happywheels_nx.nro
```
 

```
 
## Credits
 
The loader/shim infrastructure (`so_util`, `libc_shim`, `jni_fake`, `cocos_jni`, `cocos_text`, `opensles`, `android_native_cocos`) derives from the open-source Switch `.so`-loader lineage — Andy Nguyen and fgsfds, building on TheOfficialFloW's Vita/Switch loader tradition — reaching this project via the KINGDOM HEARTS Union χ port, with the Chrono Trigger, Geometry Dash and Fruit Ninja Classic ports as references. The malloc-free logger comes from Fruit Ninja Classic; the bionic/newlib `ETIMEDOUT` and `pthread_detach` fixes come from Chrono Trigger. The on-screen cursor is a cut-down `nx_pointer`. All MIT-licensed. Music decoding uses [minimp3](https://github.com/lieff/minimp3) (CC0).
 
Thanks to everyone in that lineage for making this approach possible.
 
## Legal
 
This repository contains only the wrapper. It ships no game code and no game assets, and does not download, bundle or link to them. Running it requires files from a copy of Happy Wheels that you own.
 
Happy Wheels is © Fancy Force. This project is not affiliated with or endorsed by Fancy Force.
 
