# Native Python launcher

Version 0.3.0 uses Tkinter and Pillow, with no Electron, Node.js, Chromium or WebView2 runtime. PyInstaller bundles the Python and Tk runtimes into one Windows EXE. The existing independently written C# installer/verification code is compiled into the embedded `bridge.exe` helper.

## Build

```powershell
python -m venv .tools/launcher-python-venv
.tools/launcher-python-venv/Scripts/python.exe -m pip install -r tools/launcher-python/requirements-build.txt
powershell -NoProfile -ExecutionPolicy Bypass -File tools/launcher-python/build.ps1
```

The default output is `C:/GameDev/Macross-python-0.3.0`. `Macross30Launcher.exe` can run by itself. Build-time dependencies are not required on the user's computer. The existing first-time ISO installer still prepares its own original-data tools when needed; the launcher does not compile game code.

## User flow

Choose a play language, then press Play/Update. Available game and selected-language updates are covered by one approval. Apply game runtime first, prepare the user's original ISO data if needed, apply the selected language pack, and launch. Japanese requires no separate pack. Selecting a language never starts a download. Common and language versions are independent.

Startup, manual Update Check and five-minute refresh read public release files directly, without GitHub API authentication. The launcher reads `STATUS.json`, `BOOT-INSTALL.json` and its own `PYTHON-LAUNCHER.json` update channel. Notices and versioned history use the UI language, independently of the game language, with the configured fallback language for missing translations. Closed/unverified service status prevents new patch downloads; existing current installations remain playable. An already-running game is never patched or terminated.

Settings migrate read-only from `%LOCALAPPDATA%/Macross30Launcher/settings.json` into `python-settings.json`. Cached verified archives use the existing `.local-build/downloads/<sha256>.zip` location. Existing installed game data and saves are reused.

Self-update downloads a complete verified single EXE and reuses the existing .NET helper's atomic replacement, restart-health check and rollback. The Python entry point supports the legacy `--update-health` protocol. Building does not publish or change the existing public update channel.

## Artwork

`artwork/interstellar-v1.png` was newly generated with the built-in image tool without reference images. Prompt provenance is recorded in `artwork/provenance.json`. The build copies only that background into `assets/hero.png`. Original game PIC1 artwork is not a build input. The packaged launcher contains no game EXE, EBOOT, ISO or generated game source. This describes package contents, not legal clearance for separately downloaded game files.

## Verification

```powershell
.tools/launcher-python-venv/Scripts/python.exe -m unittest discover -s tools/launcher-python -p test_backend.py
.tools/launcher-python-venv/Scripts/python.exe tools/launcher-python/app.py --preview --smoke --report native/analysis/tk-smoke.json
C:/GameDev/Macross-python-0.3.0/Macross30Launcher.exe --preview --smoke --report C:/GameDev/Macross/native/analysis/tk-packaged-smoke.json
```

Smoke mode tests UI translations, localized notices, history filtering and language selection without downloads. Real-install integration checks replace only the final launch/select operations with stubs, preserving the user's game and active language files. Full first-time ISO installation and self-update to a future published version require separate release acceptance testing.
