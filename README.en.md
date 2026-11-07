# Macross 30 — English setup guide

**0.3.10 fixes black startup loading caused by long language-cache paths. Game: 0.3.10; launcher: 0.3.9; Korean/English base packs: 0.3.8. English corrections, Russian v5 and Spanish v3 are retained.**

**720p through 4K supported. FPS unlocking is under repair and unavailable; the 30 FPS cap remains enabled.**

**[Download the Windows launcher](https://github.com/saintiron82/macross30-releases/releases/download/v0.3.9/Macross30-launcher-0.3.9.zip)**

Game **0.3.10** · Launcher **0.3.9** · Windows x64 test release.

## Install and play

1. Download `Macross30-launcher-0.3.9.zip` and extract it to a folder.
2. Run `Macross30Launcher.exe`.
3. Choose **English** as your play language. The launcher display language and game language are separate settings.
4. Press **Play/Update** and approve the required downloads. The launcher installs the common program and the selected language pack.
5. For first setup, select the supported Japanese Macross 30 ISO dumped from your own legitimate copy, and follow the installation prompts.
6. If the ISO is encrypted, open **ISO settings** and select a `.dkey` or `.key` file you prepared that matches your image.
7. Start the game. Existing installations and settings are reused where supported. If you are asked for an installation folder or ISO, choose your own installation and original dump.

The common program package contains a precompiled Windows game executable. Required original data, including the original EBOOT, is prepared locally from the user's ISO. The launcher manages the language pack; do not extract the English pack over an existing installation manually.

## Languages and features

- **English:** the 0.3.8 base pack plus 1,980 corrections in a separate 0.3.9 text layer.
- **Russian v5 / Spanish v3:** text packs using the English image base. Translation/display review is ongoing; please report issues, including text clipping.
- **Korean:** available as a separate language pack.
- **Japanese:** uses the original game data; no Japanese language pack is required.
- Keyboard/gamepad input, remappable controls and FHD/QHD resolution options are supported. Performance depends on hardware, settings and scene.

The launcher and game have separate version numbers. A game update does not necessarily require reinstalling an unchanged language pack.

## Requirements and current limits

Windows 10/11 x64, a Direct3D 12-capable GPU, and an AVX2/SSE4.1-capable CPU are required by the current Windows runtime. First setup needs internet access, sufficient free disk space and your own supported original game dump. The first-time installer may prepare additional extraction tools when needed.

This is a test release. Problems may occur in all supported languages, including startup failures, graphics that differ from the original, rendering errors, missing or incorrect events, progression stalls, controls, saves, audio, translations or subtitles. We welcome reports of these issues. Complete first-time installation on a new PC and actual launcher replacement by a future published version remain additional acceptance checks. Do not interpret a successful build or screenshot as full-playthrough validation.

## Report problems

Join [Discord](https://discord.gg/WqaaJ6hTmY). Use **#bug-reports** for crashes, gameplay, graphics, controls or save issues, and **#translation-reports** for translation/subtitle/display problems.

Include game/launcher versions, game language, Windows/GPU details, mission or location, reproduction steps and a screenshot. Share only relevant, sanitized log excerpts. Do not upload original game files, full memory dumps or private saves.

## Original ownership and rights

**You must own a legitimate copy of Macross 30 and use a supported ISO dumped from your own copy.**

This is an unofficial, non-commercial fan project, not affiliated with or endorsed by Bandai Namco or other rights holders. All game, character, audiovisual and trademark rights remain with their respective owners. Owning the original does not automatically authorize every form of copying, modification or redistribution; comply with applicable laws and terms in your jurisdiction.

**Distribution may be discontinued at any time without prior notice**, including in response to a rights-holder request or project circumstances.


## For launcher 0.3.0 users

1. Click **Launcher update** at the bottom of your existing launcher and approve the replacement.
2. When it reopens, select your usual play language and click **Update/Play** to apply the game fixes too.
3. If automatic updating fails, download the latest launcher ZIP above, close the launcher, and replace the existing launcher files.

Keep your installed game folder and save files. If original data needs repair, select the same supported ISO dumped from your own copy; you do not need to obtain another ISO.


From launcher 0.3.2, startup automatically downloads, replaces and restarts newer launchers. The launcher stays open while the game runs and shows Game running. Existing 0.3.0/0.3.1 users must click Launcher update once to migrate. Game is 0.3.10; launcher remains 0.3.9; Korean/English base packs remain 0.3.8, with additional text packs at 0.3.10.


After the game exits, choose **Logs → Send last play logs**, review the files and send. No user login is needed; reports go to a private Discord support channel. Saves are optional and the ZIP remains in local bug-reports.

## If the ISO is reported as encrypted

In launcher 0.3.7, open **ISO settings**, check the SHA-1 identity hash, and select a matching `.dkey` or `.key` file you have prepared. Missing keys, invalid formats and mismatching keys have distinct messages. A matching key prepares a separate verified ISO while preserving the original, so additional space equal to the ISO size is needed. An already-decrypted ISO does not need a separate disc key.

## Updating to 0.3.9

If `CERTIFICATE_VERIFY_FAILED` prevents an older launcher from updating, manually download and replace it with the new launcher ZIP above. Keep your installation folder and saves. Saved launch options apply on the next game launch; while playing, use **Open game options**. HD uses existing preview textures in battles, so aircraft colors can vary.

## 0.3.10 startup hotfix

If startup remains on a black loading screen after 0.3.9, use **Play/Update** in your existing launcher to get 0.3.10. This fixes truncated language-cache paths that caused installed data to appear missing. Launcher 0.3.9 is still current; saves/settings are retained. Fix sources and hashes are preserved by the `v0.3.10` tag under `release-baselines/0.3.10`.
