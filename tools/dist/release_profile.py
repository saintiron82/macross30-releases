"""Explicit runtime policy for the conservative distribution."""
STABLE_ENV = {
    'EDGE_SKIN': '0',
    'EDGE_ANIM': '0',
    'RIBBON_NATIVE': '0',
    'MACROSS_POSTFX_HOST': '0',
    'MACROSS_NATIVE_ZLIB': '0',
}

# Next release candidate: only runtime-validated geometry may transition.
# Keep the other, separately reviewed replacement families conservative.
NORMALIZED_ENV = {
    **STABLE_ENV,
    'EDGE_NATIVE': '1',
    'EDGE_SKIN': '1',
    'EDGE_VERIFY': '0',
    'EDGE_GEOM6_EVIDENCE': '0',
}
