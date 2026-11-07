Data-only font extensions
=========================

`pack_font.py` reads normalized big-endian BFNT ABF and its existing single-page
DXT5/BC3 DDS. By default it preserves every existing glyph record and occupied atlas block,
and adds missing Unicode codepoints into transparent unused atlas cells. It
refuses unsupported source-font codepoints and insufficient space. No text is
rewritten and no executable is rebuilt.

Example local build:

```
python -X utf8 tools/localization/pack_font.py --abf analysis/font_assets/font_j24x24.abf --dds analysis/font_assets/font_j24x24_0.dds --font C:/Windows/Fonts/arial.ttf --texts build/display-corpus.txt --output build/locale-font
```

Supply actual display text (all GOP, ELF, UI and subtitle text) in UTF-8 files.
`--characters` can add an explicit set. JSON input is scanned literally; callers
can extract display values first to avoid including non-display metadata.
The builder requires Pillow. Its own sfnt cmap reader verifies glyph presence.

Python integration:

```
from pack_font import extract_resident_font, build
abf, dds = extract_resident_font(base_vfs / 'PS3_GAME/USRDIR/DATA/PACK', scratch / 'original-font')
report = build(abf, dds, installed_font_path, all_display_text, output_directory)
```

`extract_resident_font` returns two paths containing actual resident payloads.
`build` returns the manifest dictionary and writes the three output files below.

Russian packs may explicitly pass `proportional_cyrillic_metrics=True` to `build`
or select CLI `--proportional-cyrillic`. `proportional_cyrillic(abf)` also returns
the adjusted ABF bytes directly. This normalizes exactly the 66 Russian letters:
advance becomes ink width plus one pixel, and horizontal bearing becomes
`min(0, original bearing)`. Every other original record byte, all rectangles,
character IDs, page definitions and existing atlas pixels remain unchanged.
The validator permits the complete exact profile or unchanged metrics; partial
normalization and arbitrary metric edits are rejected. This option is never
automatically applied to other locales. The manifest records representative
before/after widths and all 66 metrics.

For a smaller Russian display font, explicitly pass `compact_russian=True` or
select `--compact-russian`. This applies the proportional Russian policy first,
then scales ASCII32–126, the66Russian letters and newly added glyphs to4/5 size.
Rasters use Pillow LANCZOS; ink dimensions, advances and bearings shrink while
the20-pixel baseline, global lineheight, glyph IDs, page and x/y origins remain
fixed. Advances cover the scaled ink. The validator reconstructs every existing
target glyph and its BC3 bytes deterministically. Shared blocks retain their
palette, RGB bytes and all unrelated alpha indices; unrelated glyph pixels
remain exact. New glyphs retain the bounded transparent-cell proof. This option
is never inferred from the locale name or applied to Spanish automatically.

Copy the generated `font_j24x24.abf` and `font_j24x24_0.dds` into the pack's
`gop_overlay` directory. The resident archive destinations are respectively
`data/font/font_j24x24.abf` and `data/font/font_j24x24_0.dds`. The page name in ABF
is unchanged. `font-extension.json` records source/output hashes, local font
provenance, glyph metrics and preservation checks; retain it as build evidence,
outside the payload-only overlay directory. The font file itself is not copied.
Users remain responsible for the installed font's applicable license.

Packaging code should call:

```
from pack_font import validate_extension
proof = validate_extension(resident_abf, resident_dds, supplied_abf, supplied_dds)
```

Validation raises on unapproved existing glyph changes, overlap, malformed dimensions,
changed original atlas pixels, or unrelated ABF changes. Extension is deliberately
compatible with the explicit Russian metric profile above. Starting with the
original resident font lets the validator prove that narrow exception exactly.

Runtime visual alignment still requires an actual game check. Build-time tests
prove mapping/block preservation and coverage, not game rendering.
