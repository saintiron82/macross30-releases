"""Review or append the segmented resident font artifact to native/state-ko only.

Dry-run by default. Explicit --apply backs up the complete resident slot,
updates both resident PIDX references, and verifies the old DATA bytes except the
embedded eight-byte reference. Dry-run performs no writes.
"""
import argparse
import hashlib
import json
import os
import struct
from pathlib import Path

import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from analyze_archives import cstring, unpack

ROOT = Path(__file__).resolve().parents[2]
STATE_ROOT = ROOT / "native/state-ko"
TARGET = STATE_ROOT / "hdd0/game/BLJS10184_INSTALL/USRDIR/data/pack/data.dat"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_range(stream, offset, size):
    stream.seek(offset)
    data = stream.read(size)
    if len(data) != size:
        raise ValueError("short resident read")
    return data


def outside_hash(stream, offset, slot_size, total):
    digest = hashlib.sha256()
    for start, stop in ((0, offset), (offset + slot_size, total)):
        stream.seek(start)
        remaining = stop - start
        while remaining:
            block = stream.read(min(8 * 1024 * 1024, remaining))
            if not block:
                raise ValueError("short outside-slot read")
            digest.update(block)
            remaining -= len(block)
    return digest.hexdigest()


def verify_payloads(slot, manifest):
    if slot[:4] != b"FSTS":
        raise ValueError("resident is not FSTS")
    count, table, names, names_size = struct.unpack_from("<4I", slot, 4)
    if count != 69 or count != len(manifest["entries"]):
        raise ValueError("unexpected resident entry count")
    for index, expected in enumerate(manifest["entries"]):
        no, offset, size, packed_size = struct.unpack_from("<4I", slot, table + 16 * index)
        if cstring(slot[names:names + names_size], no) != expected["name"]:
            raise ValueError("resident names/order mismatch")
        encoded_size = packed_size if 0 < packed_size < size else size
        if offset % 16 or offset + encoded_size > len(slot):
            raise ValueError("resident payload extent invalid")
        encoded = slot[offset:offset + encoded_size]
        raw = unpack(encoded, size) if 0 < packed_size < size else encoded
        if sha(raw) != expected["sha256"]:
            raise ValueError(f"payload mismatch: {expected['name']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--artifact", type=Path, default=ROOT / "build/font-ko/resident-segmented.fsts")
    parser.add_argument("--manifest", type=Path, default=ROOT / "build/font-ko/resident-segmented-manifest.json")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    artifact = args.artifact.read_bytes()
    if sha(artifact) != manifest["sha256"]:
        raise ValueError("artifact differs from verified archive manifest")
    offset, slot_size = manifest["source_offset"], manifest["original_size"]
    if (offset, slot_size) != (1329485824, 3692576) or manifest["codec_policy"] != "preserve":
        raise ValueError("only the verified codec-preserving resident artifact is accepted")
    if manifest["codec_changed_entries"] != 0:
        raise ValueError("resident compression codecs must be preserved")
    verify_payloads(artifact, manifest)
    target = TARGET.resolve(strict=True)
    index_path = TARGET.with_name("pack.idx").resolve(strict=True)
    allowed = Path(os.path.abspath(STATE_ROOT))
    if STATE_ROOT.resolve() != allowed:
        raise ValueError("state-ko must not redirect to another state tree")
    for path in (target, index_path):
        if not path.is_relative_to(allowed) or path.stat().st_nlink != 1:
            raise ValueError("both targets must be independent files physically inside state-ko")
    baseline = (ROOT / manifest["source"]).resolve(strict=True)
    if os.path.samefile(target, baseline):
        raise ValueError("target aliases original VFS")
    with baseline.open("rb") as source:
        baseline_slot = read_range(source, offset, slot_size)
    if sha(baseline_slot) != manifest["original_sha256"]:
        raise ValueError("baseline resident changed")
    data_field, index_field = 131992, 1501372
    original_index = index_path.read_bytes()
    expected_old = struct.pack("<II", offset, slot_size)
    def resident_field(blob):
        if blob[:4] != b"PIDX":
            raise ValueError("target index is not PIDX")
        fields = struct.unpack_from("<9I", blob, 4)
        secondary, names, names_size = fields[5], fields[7], fields[8]
        found = []
        for i in range(struct.unpack_from("<I", blob, secondary)[0]):
            record = secondary + struct.unpack_from("<I", blob, secondary + 4 + i*4)[0]
            name_offset = struct.unpack_from("<I", blob, record)[0]
            if cstring(blob[names:names+names_size], name_offset).lower() == "resident":
                found.append(record + 8)
        if len(found) != 1:
            raise ValueError("PIDX must contain exactly one resident archive")
        return found[0]
    if resident_field(original_index) != index_field or original_index[index_field:index_field+8] != expected_old:
        raise ValueError("master PIDX resident reference differs from verified source")
    with target.open("r+b" if args.apply else "rb") as stream:
        total = os.fstat(stream.fileno()).st_size
        original = read_range(stream, offset, slot_size)
        if sha(original) != manifest["original_sha256"]:
            raise ValueError("target original resident differs from baseline")
        data_header = read_range(stream, 0, 40)
        fields = struct.unpack_from("<9I", data_header, 4)
        embedded = read_range(stream, 0, fields[7] + fields[8])
        if resident_field(embedded) != data_field or embedded[data_field:data_field+8] != expected_old:
            raise ValueError("embedded PIDX resident reference differs from verified source")
        append_offset = (total + 2047) // 2048 * 2048
        append_padding = append_offset - total
        if append_offset + len(artifact) > 0xffffffff:
            raise ValueError("appended archive exceeds PIDX 32-bit offsets")
        new_fields = struct.pack("<II", append_offset, len(artifact))
        # This hashes every original DATA byte except exactly the eight bytes
        # intentionally updated in the embedded resident reference.
        before_prefix = outside_hash(stream, data_field, 8, total)
        plan = dict(mode="apply" if args.apply else "dry-run", target=str(target),
                    master_index=str(index_path), original_length=total,
                    append_offset=append_offset, append_alignment=2048,
                    append_padding=append_padding, artifact_size=len(artifact),
                    final_length=append_offset+len(artifact),
                    original_resident_offset=offset, original_resident_size=slot_size,
                    original_resident_sha256=sha(original),
                    embedded_reference_offset=data_field, master_reference_offset=index_field,
                    original_reference_hex=expected_old.hex(), new_reference_hex=new_fields.hex(),
                    unchanged_original_data_sha256=before_prefix,
                    original_master_index_sha256=sha(original_index),
                    artifact_sha256=sha(artifact), verified_payloads=69,
                    compression_codecs_preserved=True)
        if not args.apply:
            print(json.dumps(plan, indent=2))
            print("Dry-run only: no target, backup, index or state writes.")
            return
        backup_dir = ROOT / "build/font-ko/backups"
        backup_dir.mkdir(parents=True, exist_ok=True)
        index_backup = backup_dir / ("pack_"+sha(original_index)+".idx")
        slot_backup = backup_dir / ("resident_"+sha(original)+".bin")
        transaction_path = backup_dir / ("append_transaction_"+sha(original_index)+".json")
        for backup, data in ((index_backup,original_index),(slot_backup,original)):
            if not backup.exists():
                with backup.open("xb") as output:
                    output.write(data);output.flush();os.fsync(output.fileno())
            if backup.read_bytes() != data:
                raise ValueError("transaction backup differs from source")
        transaction = dict(plan, index_backup=str(index_backup), resident_backup=str(slot_backup),
                           rollback="Restore master PACK.IDX, restore embedded eight bytes, truncate DATA.DAT to original_length")
        transaction_path.write_text(json.dumps(transaction,indent=2),encoding="utf-8")
        if (read_range(stream,data_field,8)!=expected_old or
                os.fstat(stream.fileno()).st_size!=total or index_path.read_bytes()!=original_index):
            raise ValueError("targets changed during preflight")
        try:
            stream.seek(total)
            stream.write(bytes(append_padding))
            if stream.write(artifact)!=len(artifact):
                raise OSError("short appended archive write")
            stream.flush();os.fsync(stream.fileno())
            installed=read_range(stream,append_offset,len(artifact))
            if installed!=artifact: raise AssertionError("appended archive differs")
            verify_payloads(installed,manifest)
            stream.seek(data_field);stream.write(new_fields)
            stream.flush();os.fsync(stream.fileno())
            with index_path.open("r+b") as master:
                master.seek(index_field);master.write(new_fields)
                master.flush();os.fsync(master.fileno())
            changed_index=bytearray(original_index)
            changed_index[index_field:index_field+8]=new_fields
            if index_path.read_bytes()!=changed_index:
                raise AssertionError("master PIDX mutation exceeds resident reference")
            if (read_range(stream,data_field,8)!=new_fields or
                    os.fstat(stream.fileno()).st_size!=append_offset+len(artifact) or
                    outside_hash(stream,data_field,8,total)!=before_prefix or
                    read_range(stream,offset,slot_size)!=original):
                raise AssertionError("post-apply original DATA preservation check failed")
        except BaseException:
            with index_path.open("r+b") as master:
                master.seek(0);master.write(original_index);master.truncate(len(original_index))
                master.flush();os.fsync(master.fileno())
            stream.seek(data_field);stream.write(expected_old);stream.truncate(total)
            stream.flush();os.fsync(stream.fileno())
            if (index_path.read_bytes()!=original_index or os.fstat(stream.fileno()).st_size!=total or
                    read_range(stream,data_field,8)!=expected_old):
                raise RuntimeError("transaction rollback verification failed")
            raise
        plan.update(index_backup=str(index_backup),resident_backup=str(slot_backup),
                    transaction_backup=str(transaction_path),
                    old_data_except_embedded_reference_identical=True,
                    original_resident_identical=True, master_except_reference_identical=True,
                    installed_payloads_verified=69,applied_to_vfs=False,
                    applied_to_japanese_state=False)
    (ROOT / "build/font-ko/state_apply_manifest.json").write_text(
        json.dumps(plan, indent=2), encoding="utf-8")
    print(json.dumps(plan, indent=2))


if __name__ == "__main__":
    main()
