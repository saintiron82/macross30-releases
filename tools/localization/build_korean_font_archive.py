"""Repack resident fonts into an isolated, fully verified FSTS artifact.

Default output preserves each entry's segmented/zlib codec. It must be relocated
when larger than the original slot. Flat-zlib is available for analysis only;
large flat-stream runtime support is unverified. No VFS/ISO/state is written.
"""
import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

import sys as _sys, pathlib as _pl; _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]  # tools/* siblings
from analyze_archives import cstring, unpack

RESIDENT_OFFSET = 1329485824
RESIDENT_SIZE = 3692576
NEXT_ARCHIVE_OFFSET = 1333180416


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_region(path, offset, size):
    with path.open("rb") as stream:
        stream.seek(offset)
        data = stream.read(size)
    if len(data) != size:
        raise ValueError("short source region")
    return data


def segmented_size(data):
    """Size of same-codec level-9 raw-deflate segs, including aligned chunks."""
    parts = [data[i:i + 65536] for i in range(0, len(data), 65536)]
    size = 16 + 8 * len(parts)
    for part in parts:
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
        encoded = compressor.compress(part) + compressor.flush()
        size = (size + 15) // 16 * 16 + len(encoded)
    return size


def encode_segmented(data):
    parts = [data[i:i + 65536] for i in range(0, len(data), 65536)]
    output = bytearray(16 + 8 * len(parts))
    for index, part in enumerate(parts):
        choices = []
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED,
                         zlib.Z_RLE, zlib.Z_HUFFMAN_ONLY, zlib.Z_FIXED):
            compressor = zlib.compressobj(9, zlib.DEFLATED, -15, 9, strategy)
            choices.append(compressor.compress(part) + compressor.flush())
        encoded = min(choices, key=len)
        if len(encoded) >= len(part) or len(encoded) > 65535:
            raise ValueError("segmented chunk requires unsupported raw-chunk encoding")
        output.extend(b"\0" * (-len(output) % 16))
        offset = len(output)
        output.extend(encoded)
        struct.pack_into(">HHI", output, 16 + 8 * index,
                         len(encoded), len(part) & 0xFFFF, offset + 1)
    struct.pack_into(">4sHHII", output, 0, b"segs", 5, len(parts), len(data), len(output))
    if unpack(output, len(data)) != data:
        raise AssertionError("segmented compression round-trip failed")
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=Path("native/vfs/PS3_GAME/USRDIR/DATA/PACK/DATA.DAT"))
    parser.add_argument("--font-directory", type=Path, default=Path("build/font-ko"))
    parser.add_argument("--output", type=Path, default=Path("build/font-ko/resident-segmented.fsts"))
    parser.add_argument("--manifest", type=Path, default=Path("build/font-ko/resident-segmented-manifest.json"))
    parser.add_argument("--codec-policy", choices=("preserve", "flat"), default="preserve")
    args = parser.parse_args()
    if args.output.resolve() == args.source.resolve() or args.manifest.resolve() == args.source.resolve():
        raise ValueError("output must be isolated from original DATA.DAT")
    source = read_region(args.source, RESIDENT_OFFSET, RESIDENT_SIZE)
    padding_size = NEXT_ARCHIVE_OFFSET - RESIDENT_OFFSET - RESIDENT_SIZE
    padding = read_region(args.source, RESIDENT_OFFSET + RESIDENT_SIZE, padding_size)
    neighbor = read_region(args.source, NEXT_ARCHIVE_OFFSET, 4096)
    if source[:4] != b"FSTS" or neighbor[:4] != b"FSTS" or any(padding):
        raise ValueError("resident region, zero padding or next archive differs from verified layout")
    count, table, names, names_size = struct.unpack_from("<4I", source, 4)
    if (count, table, names, names_size) != (69, 32, 1136, 2042):
        raise ValueError("unexpected resident FSTS metadata")
    font_manifest = json.loads((args.font_directory / "manifest.json").read_text(encoding="utf-8"))
    patches = {"data/font/font_j24x24.abf": ("abf", "source_abf"),
               "data/font/font_j24x24_0.dds": ("dds", "source_dds")}
    entries = []
    first_payload = min(struct.unpack_from("<I", source, table + i * 16 + 4)[0] for i in range(count))
    output = bytearray(source[:first_payload])
    same_codec_size = first_payload
    codec_changes = 0
    patched_names = set()
    for i in range(count):
        no, offset, size, packed_size = struct.unpack_from("<4I", source, table + 16 * i)
        name = cstring(source[names:names + names_size], no)
        encoded = source[offset:offset + (packed_size if 0 < packed_size < size else size)]
        original = unpack(encoded, size) if 0 < packed_size < size else encoded
        payload = original
        patched = name in patches
        if patched:
            key, source_key = patches[name]
            if sha(original) != font_manifest[source_key]["sha256"]:
                raise ValueError(f"original font hash mismatch: {name}")
            payload = (args.font_directory / Path(name).name).read_bytes()
            if sha(payload) != font_manifest["outputs"][key + "_sha256"]:
                raise ValueError(f"font overlay hash mismatch: {name}")
            patched_names.add(name)
        compressed = zlib.compress(payload, 9)
        old_codec = "segs" if encoded.startswith(b"segs") else "zlib" if 0 < packed_size < size else "raw"
        same_encoded_size = segmented_size(payload) if old_codec == "segs" else len(compressed)
        same_codec_size = (same_codec_size + 15) // 16 * 16 + min(len(payload), same_encoded_size)
        if args.codec_policy == "preserve" and old_codec == "segs":
            encoded_output = encode_segmented(payload)
            if len(encoded_output) >= len(payload):
                raise ValueError("segmented output does not meet FSTS compressed-size convention")
            new_packed_size, new_codec = len(encoded_output), "segs"
        elif args.codec_policy == "preserve" and old_codec == "raw":
            encoded_output, new_packed_size, new_codec = payload, 0, "raw"
        elif len(compressed) < len(payload):
            encoded_output, new_packed_size, new_codec = compressed, len(compressed), "zlib"
        else:
            encoded_output, new_packed_size, new_codec = payload, 0, "raw"
        codec_changes += old_codec != new_codec
        output.extend(b"\0" * (-len(output) % 16))
        new_offset = len(output)
        output.extend(encoded_output)
        struct.pack_into("<4I", output, table + i * 16,
                         no, new_offset, len(payload), new_packed_size)
        entries.append(dict(index=i, name=name, patched=patched,
                            original_size=len(original), size=len(payload),
                            original_sha256=sha(original), sha256=sha(payload),
                            old_codec=old_codec, new_codec=new_codec,
                            packed_size=len(encoded_output), offset=new_offset))
    output.extend(b"\0" * (-len(output) % 16))
    if patched_names != set(patches):
        raise AssertionError("not all requested font entries were replaced")
    for i, entry in enumerate(entries):
        no, offset, size, packed_size = struct.unpack_from("<4I", output, table + 16 * i)
        if offset % 16 or cstring(output[names:names + names_size], no) != entry["name"]:
            raise AssertionError("entry order/name/alignment changed")
        packed = bytes(output[offset:offset + (packed_size if 0 < packed_size < size else size)])
        restored = unpack(packed, size) if 0 < packed_size < size else packed
        if sha(restored) != entry["sha256"]:
            raise AssertionError(f"round-trip failed: {entry['name']}")
        if not entry["patched"] and sha(restored) != entry["original_sha256"]:
            raise AssertionError("unmodified entry payload changed")
    if output[names:names + names_size] != source[names:names + names_size]:
        raise AssertionError("name table bytes changed")
    budget = NEXT_ARCHIVE_OFFSET - RESIDENT_OFFSET
    fits = len(output) <= budget
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    manifest = dict(
        source=str(args.source), source_offset=RESIDENT_OFFSET,
        original_size=RESIDENT_SIZE, original_sha256=sha(source),
        next_archive_offset=NEXT_ARCHIVE_OFFSET, verified_zero_padding_bytes=padding_size,
        available_region_bytes=budget, rebuilt_size=len(output), growth=len(output) - RESIDENT_SIZE,
        fits_original_region=fits, remaining_headroom=budget - len(output),
        same_codec_level9_estimated_size=same_codec_size,
        codec_policy=args.codec_policy,
        compression="level9; segmented64KiB raw-deflate chunks choose smallest of five standard strategies",
        codec_changed_entries=codec_changes, verified_round_trip_payloads=count,
        unchanged_payloads_verified=count - len(patched_names),
        names_and_order_preserved=True, source_and_next_archive_untouched=True,
        next_archive_prefix_sha256=sha(neighbor), sha256=sha(output),
        patched_files=sorted(patched_names), entries=entries,
        outer_index_updates_needed={"master_PACK_IDX": {"offset_field": 1501372, "size_field": 1501376},
                                    "embedded_DATA_DAT_PIDX": {"offset_field": 131992, "size_field": 131996}},
        applied_to_vfs=False, applied_to_iso=False, game_rendering_verified=False)
    # Verify source evidence again: this tool never opens source files for writing.
    if (read_region(args.source, RESIDENT_OFFSET, RESIDENT_SIZE) != source or
            read_region(args.source, RESIDENT_OFFSET + RESIDENT_SIZE, padding_size) != padding or
            read_region(args.source, NEXT_ARCHIVE_OFFSET, len(neighbor)) != neighbor):
        raise AssertionError("source region changed during isolated build")
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Resident: {len(output)} bytes; budget {budget}; fits {fits}; headroom {budget-len(output)}.")
    print(f"Verified {count} payloads, {count-len(patched_names)} unchanged, only 2 font replacements.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
