"""Build an isolated Korean BFNT/DXT5 overlay while preserving original glyph blocks."""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

import sys as _sys, pathlib as _pl
if not getattr(_sys, "frozen", False):
    _sys.path[:0] = [str(_d) for _d in sorted(_pl.Path(__file__).resolve().parents[1].iterdir()) if _d.is_dir()]
from audit_runtime_font import chunks, occupied_rows, read_font, verify_placements


def sha(data):
    return hashlib.sha256(data).hexdigest()


def alpha_palette(a, b):
    return ([a, b] + [((7 - i) * a + i * b) // 7 for i in range(1, 7)]
            if a > b else [a, b] + [((5 - i) * a + i * b) // 5 for i in range(1, 5)] + [0, 255])


def decode_alpha(block):
    palette = alpha_palette(block[0], block[1])
    packed = int.from_bytes(block[2:8], "little")
    return [palette[(packed >> (i * 3)) & 7] for i in range(16)]


def encode_white_block(values):
    maximum, minimum = max(values), min(values)
    palette = alpha_palette(maximum, minimum)
    indices = [min(range(8), key=lambda i: abs(palette[i] - value)) for value in values]
    packed = sum(index << (i * 3) for i, index in enumerate(indices))
    # BC3 uses the four-color RGB565 mode independently of the alpha endpoints.
    return bytes((maximum, minimum)) + packed.to_bytes(6, "little") + struct.pack("<HHI", 0xFFFF, 0, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit", type=Path, default=Path("analysis/font_atlas_audit.json"))
    parser.add_argument("--abf", type=Path, default=Path("analysis/font_assets/font_j24x24.abf"))
    parser.add_argument("--dds", type=Path, default=Path("analysis/font_assets/font_j24x24_0.dds"))
    parser.add_argument("--font", type=Path,
                        default=Path("runtime/rpcs3/dev_flash/data/font/SCE-PS3-YG-R-KOR.TTF"))
    parser.add_argument("--output", type=Path, default=Path("build/font-ko"))
    parser.add_argument("--preserve-font-directory", type=Path,
                        help="verify matching previously built Hangul records and atlas blocks stay identical")
    args = parser.parse_args()
    if ((args.output / args.abf.name).resolve() == args.abf.resolve() or
            (args.output / args.dds.name).resolve() == args.dds.resolve()):
        raise ValueError("output must be isolated from original font assets")
    audit = json.loads(args.audit.read_text(encoding="utf-8"))
    original_abf, original_dds = args.abf.read_bytes(), args.dds.read_bytes()
    if sha(original_abf) != audit["source_sha256"]:
        raise ValueError("ABF differs from the audited source")
    common, original_glyphs, pages = read_font(original_abf)
    placements = audit["placements"]
    if (not audit["fits_preserving_original_rectangles"] or not placements or
            len(placements) != audit["required_missing_hangul"]):
        raise ValueError("a complete, nonempty Hangul placement audit is required")
    if audit["compression_block_size"] != 4 or audit["glyph_size"] != 24:
        raise ValueError("expected 24-pixel cells and 4-pixel BC3 blocks")
    rows = occupied_rows(common, original_glyphs, 4)
    verify_placements(rows, placements)
    if (original_dds[:4] != b"DDS " or original_dds[84:88] != b"DXT5" or
            struct.unpack_from("<II", original_dds, 12) != (common["height"], common["width"])):
        raise ValueError("DDS format/dimensions do not match audited BFNT")
    width, height = common["width"], common["height"]
    if common["page_count"] != 1 or len(original_dds) != 128 + width * height:
        raise ValueError("only a single complete BC3 base-level image is supported")
    blocks_per_row = width // 4
    reserved_blocks = set()
    alpha_checked = 0
    for placement in placements:
        cell = placement["reserved"]
        for y in range(cell["y"] // 4, (cell["y"] + cell["height"]) // 4):
            for x in range(cell["x"] // 4, (cell["x"] + cell["width"]) // 4):
                index = y * blocks_per_row + x
                offset = 128 + index * 16
                if any(decode_alpha(original_dds[offset:offset + 16])):
                    raise ValueError(f"reserved block {index} contains visible original alpha; re-audit positions")
                reserved_blocks.add(index)
                alpha_checked += 16
    args.output.mkdir(parents=True, exist_ok=True)
    powershell = Path(r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe")
    subprocess.run([str(powershell), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                    str(Path(__file__).with_name("render_korean_glyphs.ps1").resolve()),
                    "-Audit", str(args.audit.resolve()), "-Font", str(args.font.resolve()),
                    "-OutputDirectory", str(args.output.resolve())], check=True, timeout=120)
    metrics = json.loads((args.output / "glyph_metrics.json").read_text(encoding="utf-8-sig"))
    masks = (args.output / "glyph_alpha.bin").read_bytes()
    if len(metrics) != len(placements) or len(masks) != len(placements) * 24 * 24:
        raise ValueError("rasterizer result count mismatch")
    patched_dds = bytearray(original_dds)
    new_records = bytearray()
    changed_blocks = set()
    quantization_error = []
    for i, (placement, metric) in enumerate(zip(placements, metrics)):
        codepoint = int(placement["codepoint"][2:], 16)
        if metric["codepoint"] != codepoint:
            raise ValueError("rasterizer codepoint order mismatch")
        mask = masks[i * 576:(i + 1) * 576]
        if not any(mask) or not 0 < metric["width"] <= 24 or not 0 < metric["height"] <= 24:
            raise ValueError("empty or oversized glyph raster")
        for y in range(0, 24, 4):
            for x in range(0, 24, 4):
                values = [mask[(y + dy) * 24 + x + dx] for dy in range(4) for dx in range(4)]
                if not any(values):
                    continue
                index = ((placement["y"] + y) // 4) * blocks_per_row + (placement["x"] + x) // 4
                if index not in reserved_blocks:
                    raise AssertionError("glyph would modify an unaudited DDS block")
                encoded = encode_white_block(values)
                decoded = decode_alpha(encoded)
                quantization_error.extend(abs(a - b) for a, b in zip(values, decoded))
                offset = 128 + index * 16
                patched_dds[offset:offset + 16] = encoded
                changed_blocks.add(index)
        new_records.extend(struct.pack(">I4H3h2B", codepoint, placement["x"], placement["y"],
                                       metric["width"], metric["height"], metric["bearing_x"],
                                       metric["bearing_y"], metric["advance"], placement["page"], 15) + b"\xFD" * 12)
    output_abf = bytearray()
    old_cblk = None
    for offset, magic, kind, payload, extra in chunks(original_abf):
        if magic == b"BFNT" and kind == b"HCMN":
            payload = struct.pack(">I", common["glyph_count"] + len(placements)) + payload[4:]
        elif magic == b"BFNT" and kind == b"CBLK":
            if old_cblk is not None:
                raise ValueError("multiple CBLK chunks require separate audit")
            old_cblk = payload
            old_size = len(payload)
            payload += new_records
            if extra == old_size + 16:
                extra = len(payload) + 16
            elif extra:
                raise ValueError("unknown CBLK extra-size convention")
        output_abf.extend(struct.pack(">4s4sII", magic, kind, len(payload), extra) + payload)
    new_common, new_glyphs, new_pages = read_font(output_abf)
    original_keys = {g["codepoint"] for g in original_glyphs}
    new_keys = {g["codepoint"] for g in new_glyphs}
    expected = {int(p["codepoint"][2:], 16) for p in placements}
    if new_keys - original_keys != expected or len(new_keys) != len(new_glyphs):
        raise AssertionError("new character map differs from audited Hangul set")
    if new_pages != pages or new_glyphs[:len(original_glyphs)] != original_glyphs:
        raise AssertionError("original glyph records/page definitions changed")
    old_chunks = list(chunks(original_abf))
    new_chunks = list(chunks(output_abf))
    if len(old_chunks) != len(new_chunks):
        raise AssertionError("ABF chunk count changed")
    for old, new in zip(old_chunks, new_chunks):
        if old[1:3] != new[1:3]:
            raise AssertionError("ABF chunk order/type changed")
        if old[1:3] == (b"BFNT", b"CBLK"):
            if new[3][:len(old[3])] != old[3]:
                raise AssertionError("original CBLK record bytes changed")
        elif old[1:3] == (b"BFNT", b"HCMN"):
            if old[3][4:] != new[3][4:] or old[4] != new[4]:
                raise AssertionError("HCMN fields other than glyph count changed")
        elif old[1:] != new[1:]:
            raise AssertionError("original non-glyph ABF chunk changed")
    protected_blocks = set()
    for y in range(0, height, 4):
        for x in range(0, width, 4):
            if rows[0][y] & (15 << x):
                protected_blocks.add((y // 4) * blocks_per_row + x // 4)
    for index in range(width * height // 16):
        offset = 128 + index * 16
        differs = patched_dds[offset:offset + 16] != original_dds[offset:offset + 16]
        if differs and index not in changed_blocks:
            raise AssertionError("unexpected DDS block mutation")
        if index in protected_blocks and differs:
            raise AssertionError("original glyph DDS block changed")
    if patched_dds[:128] != original_dds[:128]:
        raise AssertionError("DDS header changed")
    previous_glyphs_verified = 0
    if args.preserve_font_directory:
        previous_abf = (args.preserve_font_directory / args.abf.name).read_bytes()
        previous_dds = (args.preserve_font_directory / args.dds.name).read_bytes()
        previous_common, previous_glyphs, _ = read_font(previous_abf)
        if previous_common["width"] != width or previous_common["height"] != height:
            raise ValueError("previous font atlas dimensions differ")
        new_by_key = {g["codepoint"]: g for g in new_glyphs}
        for glyph in previous_glyphs:
            if glyph["codepoint"] in original_keys:
                continue
            new_glyph = new_by_key.get(glyph["codepoint"])
            if not new_glyph or any(new_glyph[k] != glyph[k] for k in glyph if k != "record_offset"):
                raise AssertionError("previous Hangul placement or metrics changed")
            for y in range(glyph["y"] // 4, (glyph["y"] + glyph["height"] + 3) // 4):
                for x in range(glyph["x"] // 4, (glyph["x"] + glyph["width"] + 3) // 4):
                    block = 128 + (y * blocks_per_row + x) * 16
                    if previous_dds[block:block + 16] != patched_dds[block:block + 16]:
                        raise AssertionError("previous Hangul glyph texture block changed")
            previous_glyphs_verified += 1
    (args.output / args.abf.name).write_bytes(output_abf)
    (args.output / args.dds.name).write_bytes(patched_dds)
    report = dict(
        scope="Isolated font overlay only; no VFS/archive modification or game rendering claim.",
        source_abf=dict(path=str(args.abf), sha256=sha(original_abf)),
        source_dds=dict(path=str(args.dds), sha256=sha(original_dds)),
        source_font=dict(path=str(args.font), sha256=sha(args.font.read_bytes()),
                         family=(args.output / "font_family.txt").read_text(encoding="utf-8-sig")),
        outputs=dict(abf_sha256=sha(output_abf), dds_sha256=sha(patched_dds)),
        original_glyph_count=len(original_glyphs), new_hangul_count=len(placements),
        total_glyph_count=new_common["glyph_count"], original_records_identical=True,
        original_glyph_blocks_identical=True, protected_original_blocks=len(protected_blocks),
        previous_hangul_glyphs_preserved=previous_glyphs_verified,
        reserved_original_alpha_pixels_checked=alpha_checked,
        reserved_original_alpha_all_zero=True, changed_dds_blocks=len(changed_blocks),
        only_audited_free_blocks_changed=True, original_dds_header_identical=True,
        original_other_abf_chunks_preserved=True,
        raster=dict(cell=24, font_pixel_size=20, advance=19, bearing_x=1, bearing_y=4,
                    note="Advance/bearings follow existing Japanese full-width metrics; runtime layout review remains required.",
                    alpha_quantization_max_error=max(quantization_error),
                    alpha_quantization_mean_error=sum(quantization_error) / len(quantization_error)),
        game_rendering_verified=False)
    (args.output / "manifest.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    if args.abf.read_bytes() != original_abf or args.dds.read_bytes() != original_dds:
        raise AssertionError("original font assets changed during build")
    print(f"Built {len(placements)} Hangul glyphs; preserved {len(protected_blocks)} original glyph BC3 blocks.")
    print(f"Changed only {len(changed_blocks)} previously transparent audited blocks; output: {args.output}")


if __name__ == "__main__":
    main()
