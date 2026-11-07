"""Audit BFNT atlas rectangles and reserve Hangul cells without moving glyphs.

Reads an unmodified big-endian runtime ABF probe, never the atlas pixels. Every
existing CBLK rectangle is occupied, including whitespace glyph rectangles.
The output is a metadata placement proposal, not proof that unreferenced DDS
pixels are blank. No source font, DDS, executable or game archive is changed.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def chunks(data, start=0, end=None):
    end = len(data) if end is None else end
    offset = start
    while offset < end:
        if offset + 16 > end:
            raise ValueError(f"truncated chunk at {offset:#x}")
        magic, kind, size, extra = struct.unpack_from(">4s4sII", data, offset)
        stop = offset + 16 + size
        if stop > end:
            raise ValueError(f"chunk {kind!r} exceeds its container")
        yield offset, magic, kind, data[offset + 16:stop], extra
        offset = stop


def read_font(data):
    common = None
    glyphs = []
    pages = []
    for offset, magic, kind, payload, extra in chunks(data):
        if magic == b"ENDI":
            if kind != b"BIGE":
                raise ValueError("only normalized big-endian ABF probes are supported")
        elif magic == b"BFNT" and kind == b"HCMN":
            if len(payload) < 10:
                raise ValueError("short HCMN")
            common = dict(zip(("glyph_count", "page_count", "width", "height"),
                              struct.unpack_from(">IHHH", payload)))
        elif magic == b"BFNT" and kind == b"PBLK":
            for _, child_magic, child_kind, child, _ in chunks(payload):
                if child_magic == b"BFNT" and child_kind == b"PINF":
                    pages.append(child.split(b"\0", 1)[0].decode("utf-8"))
        elif magic == b"BFNT" and kind == b"CBLK":
            if len(payload) % 32:
                raise ValueError("CBLK is not a multiple of 32 bytes")
            for i in range(0, len(payload), 32):
                key, x, y, w, h, bx, by, advance, page = struct.unpack_from(
                    ">I4H3hB", payload, i)
                glyphs.append(dict(codepoint=key, x=x, y=y, width=w, height=h,
                                   bearing_x=bx, bearing_y=by, advance=advance,
                                   page=page, record_offset=offset + 16 + i))
    if common is None or common["glyph_count"] != len(glyphs):
        raise ValueError("HCMN glyph count does not match CBLK")
    if common["page_count"] != len(pages):
        raise ValueError("HCMN page count does not match PINF count")
    if not 0 < common["width"] <= 16384 or not 0 < common["height"] <= 16384:
        raise ValueError("invalid atlas dimensions")
    return common, glyphs, pages


def occupied_rows(common, glyphs, block_size=1):
    width, height = common["width"], common["height"]
    rows = [[0] * height for _ in range(common["page_count"])]
    for glyph in glyphs:
        x, y, w, h, page = (glyph[k] for k in ("x", "y", "width", "height", "page"))
        if page >= len(rows) or x + w > width or y + h > height:
            raise ValueError(f"glyph U+{glyph['codepoint']:04X} rectangle out of bounds")
        if not w or not h:
            continue
        right = min(width, ((x + w + block_size - 1) // block_size) * block_size)
        bottom = min(height, ((y + h + block_size - 1) // block_size) * block_size)
        x = (x // block_size) * block_size
        y = (y // block_size) * block_size
        mask = ((1 << (right - x)) - 1) << x
        for row in range(y, bottom):
            rows[page][row] |= mask
    return rows


def place_cells(original_rows, width, height, codepoints, glyph_size, padding, block_size=1):
    """Exact row bitsets; row-major first fit at every block-aligned origin.

    A free run of cell_width bits is detected by shifted intersections. After
    placement its complete padded rectangle is occupied, preventing overlaps.
    This is a deterministic feasible-packing search, not an optimality proof.
    """
    size = ((glyph_size + 2 * padding + block_size - 1) // block_size) * block_size
    if size > width or size > height:
        return []
    rows = [page.copy() for page in original_rows]
    full = (1 << width) - 1
    origins = sum(1 << x for x in range(0, width - size + 1, block_size))
    result = []
    for page, bitmap in enumerate(rows):
        for y in range(0, height - size + 1, block_size):
            blocked = 0
            for row in range(y, y + size):
                blocked |= bitmap[row]
            while len(result) < len(codepoints):
                free = full ^ blocked
                fits = free
                for shift in range(1, size):
                    fits &= free >> shift
                fits &= origins
                if not fits:
                    break
                x = (fits & -fits).bit_length() - 1
                mask = ((1 << size) - 1) << x
                for row in range(y, y + size):
                    bitmap[row] |= mask
                blocked |= mask
                result.append(dict(codepoint=f"U+{codepoints[len(result)]:04X}",
                                   character=chr(codepoints[len(result)]), page=page,
                                   x=x + padding, y=y + padding,
                                   width=glyph_size, height=glyph_size,
                                   reserved=dict(x=x, y=y, width=size, height=size)))
            if len(result) == len(codepoints):
                return result
    return result


def verify_placements(rows, placements):
    audit = [page.copy() for page in rows]
    for placement in placements:
        block = placement["reserved"]
        mask = ((1 << block["width"]) - 1) << block["x"]
        for y in range(block["y"], block["y"] + block["height"]):
            if audit[placement["page"]][y] & mask:
                raise AssertionError("new cell overlaps original or proposed glyph rectangle")
            audit[placement["page"]][y] |= mask


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path,
                        default=Path("native/run/font_probe_06_140EB990_454E4449.bin"))
    parser.add_argument("--translations", type=Path, default=Path("localization/ko-KR/structured"))
    parser.add_argument("--output", type=Path, default=Path("analysis/font_atlas_audit.json"))
    parser.add_argument("--glyph-size", type=int, default=24)
    parser.add_argument("--padding", type=int, default=4)
    parser.add_argument("--block-size", type=int, default=4,
                        help="DDS BC3/DXT5 block size; existing rectangles round outward to blocks")
    parser.add_argument("--preserve-placements", type=Path,
                        help="keep matching glyph cells from a previous audit unchanged")
    args = parser.parse_args()
    if args.glyph_size <= 0 or args.padding < 0 or args.block_size <= 0:
        parser.error("glyph/block size must be positive and padding nonnegative")
    if args.padding % args.block_size:
        parser.error("padding must be a multiple of block size so glyph origins stay aligned")
    data = args.font.read_bytes()
    common, glyphs, page_names = read_font(data)
    translated_chars = set()
    translated_entries = 0
    for path in sorted(args.translations.glob("*.json")):
        for entry in json.loads(path.read_text(encoding="utf-8"))["entries"]:
            if entry.get("translation"):
                translated_chars.update(entry["translation"])
                translated_entries += 1
    if not translated_entries:
        raise ValueError("no translated entries found")
    existing = {glyph["codepoint"] for glyph in glyphs}
    required = sorted(ord(c) for c in translated_chars if 0xAC00 <= ord(c) <= 0xD7A3
                      and ord(c) not in existing)
    exact_rows = occupied_rows(common, glyphs)
    rows = occupied_rows(common, glyphs, args.block_size)
    retained = []
    planning_rows = [page.copy() for page in rows]
    if args.preserve_placements:
        previous = json.loads(args.preserve_placements.read_text(encoding="utf-8"))
        if (previous["source_sha256"] != hashlib.sha256(data).hexdigest() or
                previous["glyph_size"] != args.glyph_size or
                previous["padding_each_side"] != args.padding or
                previous["compression_block_size"] != args.block_size):
            raise ValueError("previous placements do not match source/geometry")
        retained = [p for p in previous["placements"] if int(p["codepoint"][2:], 16) in required]
        verify_placements(rows, retained)
        for placement in retained:
            block = placement["reserved"]
            mask = ((1 << block["width"]) - 1) << block["x"]
            for y in range(block["y"], block["y"] + block["height"]):
                planning_rows[placement["page"]][y] |= mask
    retained_keys = {int(p["codepoint"][2:], 16) for p in retained}
    added = place_cells(planning_rows, common["width"], common["height"],
                       [key for key in required if key not in retained_keys],
                       args.glyph_size, args.padding, args.block_size)
    placements = sorted(retained + added, key=lambda p: int(p["codepoint"][2:], 16))
    verify_placements(rows, placements)
    fits = len(placements) == len(required)
    report = dict(
        source=str(args.font), source_sha256=hashlib.sha256(data).hexdigest(),
        font=common, page_names=page_names, translated_entries=translated_entries,
        original_hangul_syllables=sum(0xAC00 <= key <= 0xD7A3 for key in existing),
        required_missing_hangul=len(required), glyph_size=args.glyph_size,
        padding_each_side=args.padding,
        compression_block_size=args.block_size,
        retained_previous_placements=len(retained),
        previous_placement_source=str(args.preserve_placements) if args.preserve_placements else None,
        atlas_pages=[dict(page=i,
                          exact_glyph_union_pixels=sum(row.bit_count() for row in exact_rows[i]),
                          occupied_union_pixels=sum(row.bit_count() for row in bitmap),
                          free_pixels=common["width"] * common["height"] -
                                      sum(row.bit_count() for row in bitmap))
                     for i, bitmap in enumerate(rows)],
        fits_preserving_original_rectangles=fits, placed_candidate_count=len(placements),
        placement_overlap_check_passed=True,
        placements=placements if fits else [],
        recommendation=("All requested cells fit in metadata-unreferenced atlas rectangles; inspect DDS pixels before patching."
                        if fits else "Expand the atlas or add a page; no complete placement proposal was produced."),
        limitations=["All existing glyph rectangles, rounded outward to compression blocks, count as occupied regardless of pixel color or glyph usage.",
                     "DDS pixels and filtering halos are not inspected; free metadata space is not proven blank.",
                     "Row-major first-fit failure is not a mathematical proof that no other packing exists.",
                     "This audit reserves square glyph cells; rasterized metrics and UI layout remain unverified."])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Existing glyphs: {len(glyphs)}; missing Hangul: {len(required)}; "
          f"placed: {len(placements)}; all fit: {fits}; report: {args.output}")
    return 0 if fits else 2


if __name__ == "__main__":
    raise SystemExit(main())
