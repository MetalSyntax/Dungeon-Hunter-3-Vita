<p align="center"><img src="extras/livearea/pic0.png" alt="Dungeon Hunter 3" width="640"></p>

# Dungeon Hunter 3 — PS Vita Port

A native port of Gameloft's **Dungeon Hunter 3** (Android, v1.5.2c) to the PlayStation Vita, built on
[TheFloW's so-loader](https://github.com/TheOfficialFloW) technique: the original Android `libDH3.so`
is relocated and run directly on the Vita, with a
[FalsoJNI](https://github.com/v-atamanenko/FalsoJNI)-based bridge standing in for the parts of the
Android runtime the game calls into (Java callbacks, file system, OBB, input, audio, video, IME),
rendering through a patched [vitaGL](https://github.com/Rinnegatamante/vitaGL) built from source.

**This repo is loader/bridge source code only.** It does not contain, and will never contain,
Gameloft's copyrighted game assets, APK, OBB or `.so` binaries — see
[Getting the game data](#getting-the-game-data) for what you need to supply yourself.

## Status: playable — help and testing welcome

Verified on real hardware: intro video, menus, character creation (with the Vita's on-screen
keyboard), tutorial, dungeon combat, all four classes rendering with their real textures, physical
controls, and saves that survive a reboot. The game holds **30 FPS** (the original Android frame
cap) in normal play.

Known open issues:

1. **Short FPS dips (~27 fps)** while the game streams monsters/effects mid-level. Mitigated (disk
   probes for non-existent paths are short-circuited, per-asset logging removed); the next lever is
   reporting a "mid" device tier instead of "high" (see `RELEASE_NOTES.md`).
2. **Debug builds log a lot** (`DEBUG_SOLOADER`): every file open, texture upload and a few
   bounded shader diagnostics. Use a Release build for playing.
3. **Online features** (Gameloft Live, IGP store, social, tracking) are stubbed: the servers are
   gone and none of it is needed to play.

`port_progress.md` is the full paper trail: every crash, the log/dump that exposed it, the
disassembly that explained it, and the fix — one bug at a time. `RELEASE_NOTES.md` summarizes it.

## Controls

The port presents the Vita to the game as an **Xperia Play with its gamepad slid open**, so the game
uses its own native gamepad scheme and on-screen prompts.

| Vita input | Xperia Play equivalent | In game |
|---|---|---|
| Left stick | Left touchpad | Move |
| Right stick | Right touchpad | Aim / ranged attack |
| D-Pad | D-Pad | Menu navigation |
| Cross / Square / Triangle / Circle | Cross / Square / Triangle / Circle | Attack, skills, back |
| L / R | L1 / R1 | R = health potion |
| START / SELECT | Start / Select | Pause / menus |
| Rear touch (left / center / right) | Touchpad zones | Move / **swap weapon** / aim |
| L2 / R2 / L3 / R3 (PSTV) | Touchpad center | Swap weapon |
| Front touch | Touch screen | Everything the HUD exposes |

Stick axes and the frame cap can be changed in `ux0:data/dungeonhunter3/config.txt` (created on first
boot, missing keys are added automatically):

```
setting_invertLeftX 0
setting_invertLeftY 1
setting_invertRightX 0
setting_invertRightY 1
setting_fpsCap 30
```

## Building

Requirements:
- [VitaSDK](https://vitasdk.org/). `kubridge` stubs, FalsoJNI, so_util, libc_bridge and **vitaGL**
  are vendored under `lib/` (vitaGL is built from source with local `[DH3]` patches — see
  `lib/vitagl/VENDORED.md` and `README VITAGL.md`).
- On the Vita: [kubridge](https://github.com/bythos14/kubridge) (`kubridge.skprx` in taiHEN's
  `*KERNEL`) and Sony's shader compiler `libshacccg.suprx` in `ur0:data/` (not redistributed here).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

VitaSDK's toolchain does not cope with spaces in paths: build from a space-free directory. The
psvita-port-toolkit CLI (`psvita-toolkit build` / `deploy` / `analyze`) used
during development does that automatically and also handles FTP deploys and crash-dump triage.
`Debug` builds define `DEBUG_SOLOADER` (verbose logs to `ux0:data/dungeonhunter3/logs/`).

## Getting the game data

You need to legally own **Dungeon Hunter 3** for Android (v1.5.2c, `com.gameloft.android.ANMP.GloftD3HM`)
with its OBB. Nothing of it is in this repo; `.gitignore` excludes it by design.

Copy to the Vita:

```
ux0:data/dungeonhunter3/
├── libDH3.so                                    # from the APK: lib/armeabi-v7a/libDH3.so
├── assets/                                      # the APK's assets/ folder
├── res/                                         # the APK's res/ folder
└── com.gameloft.android.ANMP.GloftD3HM/
    └── main.1501.com.gameloft.android.ANMP.GloftD3HM.obb
```

The OBB is also found directly in `ux0:data/dungeonhunter3/` or under `obb/<package>/`.
Optional intro: the APK's `res/raw/logo.mp4` is MPEG-4 Part 2, which SceAvPlayer can't decode;
transcode it to `res/raw/logo_vita.mp4` with the `ffmpeg` command documented in `source/main.c`.
Without it the intro is simply skipped.

Then install the `.vpk` (title ID `PSVDH0003`) with VitaShell.

## Credits

- [SoLoBoP](https://github.com/v-atamanenko) (Andy Nguyen, Rinnegatamante, Volodymyr Atamanenko) —
  the so-loader boilerplate this project is built on, MIT licensed (see `LICENSE`).
- [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) — the JNI bridge implementation.
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) — the GLES driver, built from source.
- [kubridge](https://github.com/bythos14/kubridge) — kernel memory bridge.
- **Dungeon-Hunter-2-vita** — sibling port on the same Gameloft *glitch* engine; its vendored vitaGL
  and loader fixes were the starting point here.
- Dungeon Hunter 3 is © Gameloft. This project is an unofficial, non-commercial fan port and is not
  affiliated with or endorsed by Gameloft.

## License

The loader/bridge source code in this repository is MIT licensed — see `LICENSE`. vitaGL is under
its own license (`lib/vitagl/COPYING*`). These licenses cover this project's own code only, not
Dungeon Hunter 3 itself.
