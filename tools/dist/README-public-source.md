# Reviewed public source snapshots

Keep the private development branch private. The public release commit must have
the reviewed public commit `7da9b9469ac42d719e416bd7b1764a03ba8446bd` as its parent,
never the private worktree's `HEAD`.

`public_source_snapshot.py` reads the public base and a fixed allowlist of current
launcher, installation and runtime source files. It excludes complete game
executables, EBOOT, generated PPU/SPU source, original guard-byte arrays and
unrelated localization tools/data. English caption loader and native UI source
are independent code; the actual translated asset data remains in its separate
language patch artifact.

The active release EXE uses the current native UI source with the language
dropdown and English caption bridge. `native/analysis/rsx-clean-owned.c` predates
those changes, so it must not replace the current UI source in this release's
snapshot.

Freeze the relevant source before running this command:

```powershell
python -X utf8 tools/dist/public_source_snapshot.py `
  --output native/analysis/public-source-snapshot-0.3.0 `
  --readme native/analysis/public-source-snapshot-0.3.0/README.proposed.md `
  --original-elf analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf
```

Review `public-source-reviewed.zip` and `public-source-review.json`. The report
contains the base commit, explicit overlays and SHA-256 of every staged file.
The CLI does not create a commit or push anything.

Once the review is approved, call `commit_reviewed(output)` from the module. It
checks the reviewed ZIP and its inventory again, then creates a local Git commit
from those exact frozen bytes in a temporary index. Its only parent is the
public baseline, and its author identity is the project name with a reserved
`.invalid` email address. Existing branches and the normal index are unchanged.
The resulting SHA is recorded in `public-source-commit.txt`. This function does
not push or tag the commit.

Before any later push, check:

- the commit has exactly the reviewed public parent;
- its tree contains no binary/generated game source or complete EBOOT;
- current launcher and installation tool imports are present;
- the public README accurately distinguishes prebuilt runtime assets from the
  source-only automatic GitHub archive;
- the release assets' server SHA-256 matches the locally verified artifacts.

Public GitHub ownership still displays the account name. This workflow avoids
publishing private commit history; it is not an anonymity guarantee.

The English data builder imports the generic MKDELTA1 codec from make_dist.py.
The snapshot therefore includes that module's independent import closure:
check_dist_env.py, release_profile.py, release_inputs.py and
native_binary_launch.py. This does not include their private proof fixtures or
frozen translation data. To reproduce the English release artifact, separately
provide the approved frozen v11 readiness metadata and resources referenced by
localization/en-US/pack-ready.json. Those full catalogs/assets are deliberately
not copied into the Git source snapshot; consumers use the released delta pack.
