# Dungeon Hunter 3 — PS Vita · v1.0.0 "First Light"

> Paste the section below into the GitHub release description. The title above goes in the
> release-title field; suggested tag: `v1.0.0`.

---

**First public release: Dungeon Hunter 3 (Android v1.5.2c) boots, plays and saves on a real PS
Vita at 30 FPS, with every character rendering its real textures.**

Every fix below was found the same way: a console log + crash dump, the disassembly of `libDH3.so`,
and the original Java from the APK — no guessing. The full trail, phase by phase, is in
`port_progress.md`.

## Boot and runtime

- **Crash on startup in `glf::AndroidSetPaths`** — the port called `setPaths` before the engine's
  `glf::App` existed. Paths are now set by the engine itself from `getViewSettings`, as on Android.
- **Every boolean Java callback returned `false`** (`isHighPhoneGroup`, `checkFirmware`,
  `setCurrentContext`…): they were registered as `int`, and FalsoJNI only dispatches
  `CallStaticBooleanMethod` to boolean methods.
- **Missing `inet_addr` import** (fatal lazy-binding error when the game reached network code).
- **Undefined instruction `SWP`** — the `.so` uses ARMv5 atomics (boost spinlock pool) that Android
  emulates in the kernel and the Vita doesn't. All 178 sites are rewritten to `LDREX/STREX`
  trampolines at load time.
- **Crash in `_lseek_r`** — the `.so`'s libstdc++ reads Bionic's `FILE` layout (`_file` at +14).
  `std::__basic_file` and glf's disk reader are routed through stdio on the same `FILE*`.
- **`strcpy(NULL)` in `glot::TrackingManager`** — device UDID/MAC/model/firmware are now sent from
  `onResume`'s equivalent (UDID = the console's OpenPSID).
- **bionic `pthread` emulation rewritten** — the old fixed 1024-slot registry silently stopped
  working past 1024 mutexes, lazy init raced, and `pthread_once` / `sem_timedwait` were wrong.
  Mutex/cond pointers now live in the bionic word itself with atomic lazy init.
- **SceLibc's fixed `FILE` table ran out** (the engine opens the 1 GB OBB once per asset): read-only
  OBB opens are virtual handles sharing one file descriptor (`sceIoPread`).

## Graphics

- **Crash in `glLinkProgram`** — a shader that failed to compile left a NULL program. vitaGL is now
  vendored and built from source with logs; failed shaders get a stand-in and are saved for review.
- **Shader bug in the game's own GLSL** (`BlendRatio`/`Specular2` undeclared in the SP+TB variants):
  the dead statements are blanked at load.
- **Broken ETC1 textures** — vitaGL built with `SAFE_ETC1` (CPU decode, recompressed to DXT1),
  plus a `[DH3]` vitaGL fix for mip levels > 0 in that path.
- **Player character drawn as a flat "mannequin"** while enemies looked fine — vitaGL decided that
  *all* vertex attributes were interleaved by looking at the first two only. The hero is skinned on
  the CPU (`Vertex`+`Normal` in one buffer, `texcoord0` in another), so its UVs were read from the
  wrong array. Patched `[DH3]` in vitaGL to check every attribute (a global `SAFE_DRAW` also fixed it
  but cost FPS on every draw).
- **NULL mesh-buffer guards** in `createMaterialVertexAttributeMap` / `CVertexAttributeMap::set`
  for lightmap layers that don't exist in the OBB.
- **IME / on-screen keyboard never appeared** (character name screen hung) — common dialogs are
  now updated by `vglSwapBuffers` while the IME is open, with a safe default name on cancel.
- Missing GL entry points implemented (`glTexParameterfv`, `glGetTexParameter*`, `glIsBuffer`,
  `glCompressedTexSubImage2D`…).

## Asset streaming (the long one)

- **Crash in `COnDemandReader::read`** — streamed `.bdae` models are re-opened as `"/name.bdae"`;
  the pak lookup never stripped the leading slash. `CCustomPakReader::findFile` is replaced with the
  same algorithm plus that fix and a basename fallback.
- **Stack smash in `FileSystemBase::customCreateAndOpenFile`** (512-byte path buffer).
- **A chain of collada crashes** (`constructGeometry`, `constructController`, `instanciateMesh`,
  `setMaterial`) all traced back to one root cause: the streamed model's name was **shader source
  text**. `glf::FileStream::Open` copies `.obb`/`.sav` paths longer than 80 characters to the stack
  and returns without ever writing its own path buffer, which `GetPath()` returns. That buffer comes
  from an uninitialized `new`: on Android's fresh heap it was empty, on the Vita it held freed shader
  text — inherited by every pak file's full path. The path buffer is now initialized on that branch.

## Input

- **L and R did nothing** (the potion tutorial couldn't be passed): `sceCtrlPeekBufferPositiveExt2`
  reports the Vita's shoulder buttons as `L1/R1`, the keymap expected `LTRIGGER/RTRIGGER`.
- **Inverted stick**: the game reads the Xperia Play touchpad with Y up. Y is flipped by default and
  every axis is configurable in `config.txt`.
- **Tutorial stuck on "tap the center of the touchpad"**: the rear touch panel maps to the full
  Xperia touchpad (left/center/right zones), and L2/R2/L3/R3 press its center (swap weapon).

## Performance

- **15 FPS → 30 FPS**: the main loop slept a fixed 33 ms *after* every frame, on top of rendering.
  It now sleeps only what's left of the frame budget (`setting_fpsCap`, default 30 = Android's cap).
- Fewer streaming hitches: disk probes for `ux0:data/dungeonhunter3/data/…` (a folder that doesn't
  exist, everything lives in the OBB) fail immediately; per-asset OBB opens are no longer logged.

## Saves

- Saves persist across reboots (verified with a save → reboot → continue cycle).
- `rename()` now replaces an existing target like POSIX (Vita's `sceIoRename` refuses).
- Every engine delete/rename is logged with its caller, and the loader copies existing saves to
  `ux0:data/dungeonhunter3/savebackup/` at boot as a safety net.

## Video

- Intro video plays through SceAvPlayer: allocator falls back through CDRAM → PHYCONT → UNCACHE →
  vitaGL's already-mapped heap; the APK's MPEG-4 Part 2 logo must be transcoded to H.264 baseline
  (`res/raw/logo_vita.mp4`, command in `source/main.c`).

## Known issues

- Short FPS dips (~27) while monsters/effects stream in mid-level. The game runs on the "high"
  device tier (faithful to the APK's PowerVR heuristic); a "mid" tier is the next thing to try.
- Online features (Gameloft Live, store, social, tracking) are stubbed out.
- Debug builds are verbose; use Release to play.

## Installing

1. Install `kubridge.skprx` and `ur0:data/libshacccg.suprx` if you don't have them already.
2. Copy your own game data to `ux0:data/dungeonhunter3/` (layout in the README).
3. Install `dungeonhunter3.vpk` with VitaShell.
