"""Read-only source extraction and honest localization coverage inventory.

Writes analysis/localization catalogs only. ELF strings are candidates pending
UI-role review; DDS filenames are hints, never proof of Japanese image text.
"""
import argparse
import collections
import hashlib
import json
import re
import struct
from pathlib import Path


JP = re.compile(r"[\u3040-\u30ff\u3400-\u9fff]")
TEXTURE_HINT = re.compile(r"title|ttl_|menu|text|font|guide|help|option|opt_|message|msg|placename|brief|caption|dialog|tutorial", re.I)
TOKENS = re.compile(r"%%|%[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]|\\[nrt]|\[[^\]]+\]")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def extract_elf(path):
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x02":
        raise ValueError("expected big-endian ELF64")
    phoff = struct.unpack_from(">Q", data, 32)[0]
    phsize, phcount = struct.unpack_from(">HH", data, 54)
    segments = []
    for index in range(phcount):
        kind, flags, offset, va, physical, size, memory, align = struct.unpack_from(">II6Q", data, phoff + index * phsize)
        if kind == 1 and size:
            if offset + size > len(data):
                raise ValueError("PT_LOAD file range exceeds ELF")
            segments.append(dict(index=index, flags=flags, file_offset=offset,
                                 virtual_address=va, file_size=size, memory_size=memory))
    decoded = []
    for match in re.finditer(rb"[^\x00]{3,}\x00", data):
        raw = match.group()[:-1]
        if len(raw) > 16384:
            continue
        segment = next((s for s in segments if s["file_offset"] <= match.start()
                        and match.end() <= s["file_offset"] + s["file_size"]), None)
        if segment is None:
            continue
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        if any(ord(c) < 32 and c not in "\r\n\t" for c in text):
            continue
        decoded.append(dict(file_offset=match.start(), text=text, size=len(raw), segment=segment))
    entries = []
    for i, row in enumerate(decoded):
        if not JP.search(row["text"]):
            continue
        offset, segment, text = row["file_offset"], row["segment"], row["text"]
        va = segment["virtual_address"] + offset - segment["file_offset"]
        string_hash = digest(text.encode("utf-8"))
        context_start = max(segment["file_offset"], offset - 32)
        context_end = min(segment["file_offset"] + segment["file_size"], offset + row["size"] + 33)
        entries.append(dict(
            id=f"EBOOT:va:{va:08x}:sha256:{string_hash[:12]}",
            file_offset=offset, file_offset_hex=f"0x{offset:X}",
            virtual_address=va, virtual_address_hex=f"0x{va:X}",
            load_segment_index=segment["index"], source=text,
            source_bytes=row["size"], source_sha256=string_hash,
            terminator_file_offset=offset + row["size"],
            control_tokens=TOKENS.findall(text),
            actual_newline_count=text.count("\n"),
            context=dict(previous=[dict(file_offset=r["file_offset"], text=r["text"][:240])
                                   for r in decoded[max(0, i - 2):i]],
                         following=[dict(file_offset=r["file_offset"], text=r["text"][:240])
                                    for r in decoded[i + 1:i + 3]],
                         file_offset=context_start, bytes_hex=data[context_start:context_end].hex()),
            translation="", status="candidate; display role and references require review",
            in_place_capacity_verified=False))
    return data, segments, entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, default=Path("analysis/extracted/PS3_GAME/USRDIR/EBOOT.elf"))
    parser.add_argument("--catalogs", type=Path, default=Path("localization/ko-KR/structured"))
    parser.add_argument("--pack-inventory", type=Path, default=Path("analysis/pack_inventory.json"))
    parser.add_argument("--output", type=Path, default=Path("analysis/localization"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    elf_bytes, segments, elf_entries = extract_elf(args.elf)
    elf_document = dict(source_file=str(args.elf), source_sha256=digest(elf_bytes),
                        extraction="Null-terminated strict UTF-8 Japanese candidates entirely within ELF64 PT_LOAD file ranges",
                        load_segments=segments, entries=elf_entries,
                        limitations=["Null-delimited scan can miss strings adjacent to binary prefixes or constructed at runtime.",
                                     "Candidates include debug/editor text; extraction does not prove user-visible UI.",
                                     "Offsets are provenance, not authorization for variable-length in-place replacement."])
    write_json(args.output / "elf_japanese_candidates.json", elf_document)
    structured = []
    catalog_files = []
    for path in sorted(args.catalogs.glob("*.json")):
        document = json.loads(path.read_text(encoding="utf-8"))
        rows = document["entries"]
        translated = sum(bool(r.get("translation")) for r in rows)
        source_file = Path(document["source_file"])
        source_matches = source_file.exists() and digest(source_file.read_bytes()) == document["source_sha256"]
        catalog_files.append(dict(catalog=str(path), source_file=str(source_file), entries=len(rows),
                                  translated_drafts=translated, remaining=len(rows) - translated,
                                  source_hash_matches=source_matches))
        structured.extend((str(path), r) for r in rows)
    source_set = {r["source"] for _, r in structured}
    elf_set = {r["source"] for r in elf_entries}
    translations = collections.defaultdict(set)
    for _, row in structured:
        if row.get("translation"):
            translations[row["source"]].add(row["translation"])
    conflicting = {source: sorted(values) for source, values in translations.items() if len(values) > 1}
    inventory = json.loads(args.pack_inventory.read_text(encoding="utf-8"))
    textures = []
    for row in inventory:
        if Path(row["path"]).suffix.lower() != ".dds":
            continue
        identity = f"{row['pack']}:{row['offset']:08x}:{row['path']}"
        name = row["path"]
        matches = sorted(set(m.group().lower() for m in TEXTURE_HINT.finditer(name)))
        textures.append(dict(id=identity, archive_entry=name, pack_file=row["pack"],
                             pack_offset=row["offset"], size=row["size"],
                             compressed_size=row["compressed_size"],
                             filename_text_hints=matches,
                             review_priority="inspect" if matches else "unclassified",
                             japanese_image_text_verified=False,
                             japanese_text_extracted=False, translated=False))
    write_json(args.output / "texture_inventory.json", dict(
        source=str(args.pack_inventory), entries=textures,
        limitations="Inventory paths/offsets identify actual assets. Filename hints do not establish visible Japanese text or translation coverage."))
    union = source_set | elf_set
    summary = dict(
        structured_catalogs=catalog_files,
        structured_entries=len(structured),
        structured_translated_draft_entries=sum(bool(r.get("translation")) for _, r in structured),
        structured_remaining_entries=sum(not r.get("translation") for _, r in structured),
        structured_unique_sources=len(source_set),
        structured_duplicate_occurrences=len(structured) - len(source_set),
        elf_candidate_occurrences=len(elf_entries), elf_unique_candidate_sources=len(elf_set),
        elf_duplicate_occurrences=len(elf_entries) - len(elf_set),
        shared_exact_sources_between_structured_and_elf=len(source_set & elf_set),
        unified_unique_source_candidates=len(union),
        unified_unique_sources_with_at_least_one_draft=len(set(translations) & union),
        unified_unique_sources_without_any_draft=len(union - set(translations)),
        conflicting_translation_sources=conflicting,
        texture_entries=len(textures), texture_filename_hint_candidates=sum(bool(r["filename_text_hints"]) for r in textures),
        source_elf_sha256=digest(elf_bytes),
        completion_verified=False,
        limitations=["Unified unique count deduplicates exact Japanese source text only; context-dependent translations can differ.",
                     "A draft for one occurrence does not establish translation of every matching occurrence.",
                     "ELF candidate count includes unclassified debug/editor strings and excludes missed encodings/runtime strings.",
                     "Texture occurrences are separate visual candidates, not added to textual string counts.",
                     "Only 12 GOP tables have structurally selected Japanese catalogs; other assets need independent review.",
                     "No translation accuracy, complete UI coverage or in-game rendering claim follows from these counts."])
    write_json(args.output / "scope_summary.json", summary)
    print(f"Structured: {len(structured)} entries; {summary['structured_translated_draft_entries']} drafts; "
          f"{summary['structured_remaining_entries']} remaining.")
    print(f"ELF: {len(elf_entries)} candidates; exact-deduplicated unified source candidates: {len(union)}.")
    print(f"DDS: {len(textures)} actual entries; {summary['texture_filename_hint_candidates']} filename hints pending visual review.")
    if args.elf.read_bytes() != elf_bytes:
        raise AssertionError("source ELF changed during read-only audit")


if __name__ == "__main__":
    main()
