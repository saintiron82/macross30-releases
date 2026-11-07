"""Prepare a coherent Korean disc VFS without changing JP or draft sources.

Unchanged assets are shared hard links and must be treated as read-only. DATA.DAT
and PACK.IDX are independent copies from the verified Korean installed state.
"""
import hashlib
import json
import os
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "build/game-next"
OUTPUT = ROOT / "build/game-ko"
INSTALLED = ROOT / "native/state-ko/hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack"
MANIFEST = ROOT / "build/font-ko/game-ko-manifest.json"


def sha_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def live_processes():
    command = ("$p=@(Get-CimInstance Win32_Process -Filter \"Name='Macross30Recompiled.exe'\" | "
               "Select-Object ProcessId,CommandLine);ConvertTo-Json -InputObject $p -Compress")
    result = subprocess.run([r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe",
                             "-NoProfile", "-Command", command], capture_output=True,
                            text=True, encoding="utf-8", check=True, timeout=20)
    return json.loads(result.stdout.strip() or "[]")


def main():
    processes = live_processes()
    for process in processes:
        command = (process.get("CommandLine") or "").replace("\\", "/").lower()
        if not command:
            raise ValueError("cannot determine active native game's VFS scope")
        if any(str(p).replace("\\", "/").lower() in command for p in (SOURCE, OUTPUT, ROOT / "native/state-ko")):
            raise ValueError("native process is using Korean/draft source scope; stop it before preparing VFS")
    if OUTPUT.exists():
        raise FileExistsError("build/game-ko already exists; refusing all overwrite conflicts")
    installed_manifest = json.loads((ROOT / "build/font-ko/state_apply_manifest.json").read_text(encoding="utf-8"))
    overrides = {"ps3_game/usrdir/data/pack/data.dat": INSTALLED / "data.dat",
                 "ps3_game/usrdir/data/pack/pack.idx": INSTALLED / "pack.idx"}
    for source in overrides.values():
        if source.stat().st_nlink != 1:
            raise ValueError("Korean installed overrides must be independent files")
    if overrides["ps3_game/usrdir/data/pack/data.dat"].stat().st_size != installed_manifest["final_length"]:
        raise ValueError("installed Korean DATA length differs from apply manifest")
    required = sum(p.stat().st_size for p in overrides.values()) + 128 * 1024 * 1024
    free = shutil.disk_usage(ROOT).free
    if free < required:
        raise OSError(f"insufficient free disk space: need {required}, free {free}")
    source_files = [(p, Path("PS3_GAME") / p.relative_to(SOURCE / "PS3_GAME"))
                    for p in sorted((SOURCE / "PS3_GAME").rglob("*")) if p.is_file()]
    source_files.append((SOURCE / "PS3_DISC.SFB", Path("PS3_DISC.SFB")))
    found_overrides = set()
    plan = []
    for source, relative in source_files:
        if not source.resolve().is_relative_to(SOURCE.resolve()):
            raise ValueError("source asset resolves outside draft VFS")
        key = relative.as_posix().lower()
        selected = overrides.get(key, source)
        if key in overrides:
            found_overrides.add(key)
        plan.append((selected, OUTPUT / relative, key in overrides, relative))
    if found_overrides != set(overrides):
        raise ValueError("disc asset tree lacks one or both override paths")
    OUTPUT.mkdir(parents=True)
    for source, target, independent, relative in plan:
        target.parent.mkdir(parents=True, exist_ok=True)
        if independent:
            with source.open("rb") as original, target.open("xb") as output:
                shutil.copyfileobj(original, output, length=8 * 1024 * 1024)
                output.flush()
                os.fsync(output.fileno())
            if os.path.samefile(source, target) or target.stat().st_nlink != 1:
                raise AssertionError("Korean override is not independent")
        else:
            os.link(source, target)
    def verify(item):
        source, target, independent, relative = item
        before = source.stat()
        digest = sha_file(source)
        if before.st_size != target.stat().st_size:
            raise AssertionError(f"asset size differs: {relative}")
        if independent:
            if sha_file(target) != digest:
                raise AssertionError(f"independent copy hash differs: {relative}")
        elif not os.path.samefile(source, target):
            raise AssertionError("shared asset is not the expected hard link")
        after = source.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise AssertionError("source changed during verification")
        return dict(path=relative.as_posix(), source=str(source), size=before.st_size,
                    sha256=digest, storage="independent copy" if independent else "shared hard link",
                    editable=independent)
    with ThreadPoolExecutor(max_workers=4) as pool:
        records = list(pool.map(verify, plan))
    # Guest install validation compares these eight actual disc/HDD sizes.
    install_names = ("pack.idx", "data.dat", "data2.dat", "fileset0.dat", "fileset1.dat",
                     "fileset2.dat", "init.dat", "shaders.dat")
    sizes = []
    by_name = {row["path"].lower(): row for row in records}
    for name in install_names:
        row = by_name["ps3_game/usrdir/data/pack/" + name]
        hdd = INSTALLED / name
        if row["size"] != hdd.stat().st_size:
            raise AssertionError(f"guest exact install size mismatch remains: {name}")
        sizes.append(dict(name=name, disc_size=row["size"], installed_size=hdd.stat().st_size,
                          equal=True))
    result = dict(vfs=str(OUTPUT), source_vfs=str(SOURCE), independent_overrides=2,
                  shared_read_only_assets=len(records) - 2, files=records,
                  shared_asset_semantics="Hard-linked assets are shared with build/game-next and MUST remain read-only. Never patch/chmod shared links; make independent copies first.",
                  excluded_source_directories=["game (runtime/HDD state)"],
                  native_processes_observed=processes,
                  unrelated_native_processes_allowed_read_only=True,
                  free_bytes_at_start=free, required_independent_copy_bytes=required,
                  guest_exact_size_validation=sizes, source_content_writes=False,
                  japanese_state_writes=False, original_vfs_writes=False,
                  game_rendering_verified=False)
    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps(result, indent=2), encoding="utf-8")
    (OUTPUT / "READ_ONLY_SHARED_ASSETS.json").write_text(json.dumps(dict(
        manifest=str(MANIFEST), semantics=result["shared_asset_semantics"]), indent=2), encoding="utf-8")
    print(f"Prepared {OUTPUT}: {len(records)} assets, 2 independent KO overrides.")
    print(f"All eight guest install sizes match; hashes verified. Manifest: {MANIFEST}")


if __name__ == "__main__":
    main()
