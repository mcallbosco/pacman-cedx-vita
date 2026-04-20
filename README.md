# PAC-MAN CE DX — PS Vita Port

### → To install, go to **[pacmancedx.mcallbos.co](https://pacmancedx.mcallbos.co/)** ←

Drop in your APK + OBB in the browser and it'll hand you back a single `pacmancedx.zip` containing the ready-to-install VPK and a `pacmancedx/` data folder. Nothing leaves your machine. This repo is the source code behind that site and the loader VPK it builds.

---

A homebrew `.so`-loader port of **PAC-MAN Championship Edition DX** (Android) to the PlayStation Vita, plus a browser-based installer that turns your user-supplied APK + OBB into a Vita-ready VPK.

> **This repository contains only the loader and the website. It does not contain, distribute, or link to PAC-MAN CE DX itself.** The game, its assets (`assets/…`), and the binaries `libPacmanCE.so` / `libfmod.so` are © Bandai Namco Entertainment. To play, you must legally own the Android version and supply the `.apk` and `.obb` from your own copy.

## What's in here

| Path | What it is |
| --- | --- |
| `source/` | Vita-side loader: relocates the Android `.so`, stubs Android/Java APIs, bridges OpenGL ES, etc. |
| `lib/vitaGL/` | Custom fork of [vitaGL](https://github.com/Rinnegatamante/vitaGL) with patches needed by this game (softfp ABI for `.so` interop, plus targeted shader/draw fixes). |
| `lib/vitaGL_clean/` | Pristine vitaGL build (hardfp) used by the standalone settings configurator. |
| `lib/{falso_jni,fios,kubridge,libc_bridge,sha1,so_util}/` | Standard `.so`-loader support libraries. |
| `extras/livearea/` | Vita LiveArea theme (icon, background, template). |
| `docs/` | The website. Drop in your APK + OBB → get a VPK + data zip in your browser. Nothing leaves your machine. |
| `.github/workflows/` | CI that builds the loader VPK and publishes the site to GitHub Pages on every push to `main`. |
| `build.sh` | Convenience wrapper: `./build.sh [extra cmake -D flags…]` |

## For end users

Don't build from source — just visit **[pacmancedx.mcallbos.co](https://pacmancedx.mcallbos.co/)** and drop in your APK + OBB. You'll get back a single `pacmancedx.zip`. Unzip it and you'll have:
- `pacmancedx.vpk` — install via VitaShell
- `pacmancedx/` — FTP this folder to `ux0:data/`

## For developers — building the loader

### Build flags

#### Rendering

| Flag | Values | Default | Effect |
| --- | --- | --- | --- |
| `VITA_MSAA_MODE` | `OFF`, `2X`, `4X` | `OFF` | vitaGL multisample antialiasing. `OFF` is the default for the released VPK to maximize GPU headroom; users can opt into `2X` or `4X` from the in-game configurator. |
| `SHADER_FORMAT` | `GLSL`, `CG`, `GXP` | `GLSL` | Source format for the game's shaders. `GXP` skips runtime compilation (fastest cold start) but requires pre-compiled `.gxp` artifacts in `data/`. |
| `DUMP_COMPILED_SHADERS` | `ON`, `OFF` | `OFF` | When using `GLSL` or `CG`, persist compiled shaders to disk so subsequent launches skip compilation. Ignored for `GXP`. |

#### Runtime / IO

| Flag | Values | Default | Effect |
| --- | --- | --- | --- |
| `USE_SCELIBC_IO` | `ON`, `OFF` | `ON` | Route file/string IO through `SceLibcBridge`. |

#### Diagnostics (all default `OFF`)

| Flag | Effect |
| --- | --- |
| `ENABLE_RUNTIME_LOGS` | Verbose `.so`-loader log to `ux0:data/pacmancedx/debug.log`. |
| `ENABLE_GL_DEBUG_HOOKS` | Heavy OpenGL call instrumentation. |
| `ENABLE_FALSOJNI_VERBOSE` | Sets `FALSOJNI_DEBUGLEVEL=0` so every JNI call and lookup is logged. |
| `ENABLE_AUDIO_LOGS` | FMOD / BGM / audio diagnostics. |
| `ENABLE_IO_PROFILING` | Per-call IO profiling. |


## Credits

- **Rinnegatamante** — vitaGL, the `.so`-loader pattern this port follows, countless reference ports
- **TheFloW** — original `.so`-loader research on Vita
- **VitaSDK contributors** — toolchain + system bindings
- **Mcall** — this port
