"""Apply the shader-seed / PSO-preparation hooks to an rsx_live_draw.c copy.

The hooks are deliberately small; the logic lives in rsx_shader_prep.h and
rsx_shader_prep_runtime.h. The patch is anchored on exact text and refuses to
run when an anchor is missing or ambiguous, so it can be applied to the frozen
seed-v2 source (which already carries the earlier module seed helper) and to a
checkout copy alike. Re-running on an already patched file is a no-op.

    python tools/shader_cache/patch_live_draw.py <rsx_live_draw.c> [--check]
"""
import argparse
import re
import sys
from pathlib import Path

MARK = 'rsx_shader_prep_runtime.h'

INCLUDE_ANCHOR = '#include "rsx_vertex_formats.h"\n'
INCLUDE_ADD = '''#include "rsx_shader_prep_runtime.h"
static HRESULT ld_builtin_compile(LPCVOID source, SIZE_T bytes, LPCSTR name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* include, LPCSTR entry,
    LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors);
'''

LOADER = r'''static u64 g_ld_shader_seed_hits;

/* Reads one cache-format file; exact generated source, DXBC container and
 * header fields must all match. *reject_kind says why it did not. */
static int shader_disk_cache_read(
    FILE* file, u32 stage, const char* source, u32 source_length, u64 hash,
    ID3DBlob** out, int* reject_kind)
{
    shader_disk_cache_header header;
    memset(&header, 0, sizeof(header));
    *out = NULL;
    *reject_kind = RSP_REJECT_FORMAT;
    int valid = fread(&header, sizeof(header), 1, file) == 1 &&
        header.magic == SHADER_DISK_CACHE_MAGIC &&
        header.version == SHADER_DISK_CACHE_VERSION &&
        header.stage == stage && header.source_hash == hash &&
        header.source_length == source_length && source_length <= 256u * 1024u &&
        header.blob_length > 0 &&
        header.blob_length <= SHADER_DISK_CACHE_MAX_BLOB;
    char* stored_source = NULL;
    ID3DBlob* blob = NULL;
    if (valid) {
        stored_source = (char*)malloc(source_length ? source_length : 1u);
        valid = stored_source != NULL &&
            fread(stored_source, 1, source_length, file) == source_length;
        if (valid && memcmp(stored_source, source, source_length) != 0) {
            valid = 0;
            *reject_kind = RSP_REJECT_SOURCE;
        }
    }
    if (valid) {
        valid = SUCCEEDED(D3DCreateBlob(header.blob_length, &blob)) && blob &&
            fread(blob->lpVtbl->GetBufferPointer(blob), 1,
                  header.blob_length, file) == header.blob_length;
    }
    if (valid && !rsp_dxbc_valid(blob->lpVtbl->GetBufferPointer(blob),
                                 blob->lpVtbl->GetBufferSize(blob))) {
        valid = 0;
        *reject_kind = RSP_REJECT_DXBC;
    }
    free(stored_source);
    if (!valid) {
        if (blob)
            blob->lpVtbl->Release(blob);
        return 0;
    }
    *out = blob;
    return 1;
}

/* User cache first (it has precedence), then the shipped seed next to the
 * executable. A bad user entry is deleted; the seed is never modified.
 * Driver PSO data is never part of either. */
static ID3DBlob* shader_disk_cache_load(
    u32 stage, const char* source, u32 source_length, u64 hash)
{
    char path[MAX_PATH];
    const u32 stage_index = shader_disk_cache_stage_index(stage);
    if (!shader_disk_cache_path(path, stage, hash, source_length))
        return NULL;

    ID3DBlob* blob = NULL;
    int reject_kind = RSP_REJECT_FORMAT;
    FILE* file = fopen(path, "rb");
    if (file) {
        const int ok = shader_disk_cache_read(
            file, stage, source, source_length, hash, &blob, &reject_kind);
        fclose(file);
        if (ok) {
            const u64 value = ++g_ld_shader_disk_hits[stage_index];
            shader_disk_cache_progress(value);
            rsp_rt_note_load(stage, 0, 1, -1);
            return blob;
        }
        DeleteFileA(path);
        g_ld_shader_disk_rejects++;
        rsp_rt_note_load(stage, 0, 0, reject_kind);
    }
    file = rsp_seed_open(SHADER_DISK_CACHE_VERSION, stage, hash, source_length);
    if (file) {
        const int ok = shader_disk_cache_read(
            file, stage, source, source_length, hash, &blob, &reject_kind);
        fclose(file);
        if (ok) {
            const u64 value = ++g_ld_shader_disk_hits[stage_index];
            shader_disk_cache_progress(value);
            rsp_rt_note_load(stage, 1, 1, -1);
            ++g_ld_shader_seed_hits;
            if (g_ld_shader_seed_hits <= 4 ||
                (g_ld_shader_seed_hits & (g_ld_shader_seed_hits - 1)) == 0)
                fprintf(stderr, "[shader-cache-seed] hit=%llu stage=%c source=%016llX bytes=%u exact_source=1\n",
                    (unsigned long long)g_ld_shader_seed_hits, (char)stage,
                    (unsigned long long)hash, source_length);
            return blob;
        }
        g_ld_shader_disk_rejects++;
        rsp_rt_note_load(stage, 1, 0, reject_kind);
    }
    const u64 value = ++g_ld_shader_disk_misses[stage_index];
    shader_disk_cache_progress(value);
    return NULL;
}

'''

BUILTIN = r'''
static const char* ld_shader_cache_dir_or_null(void)
{
    return g_ld_shader_disk_ready > 0 ? g_ld_shader_disk_dir : NULL;
}

/* Fixed renderer shaders (blits, post-process, original bloom kernels): the
 * same cache/seed path as generated shaders under stage 'B'. The identity
 * text is name/entry/target/flags followed by the complete source, so any
 * change to either compiles normally. Defines/includes bypass the cache. */
static HRESULT ld_builtin_compile(LPCVOID source, SIZE_T bytes, LPCSTR name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* include, LPCSTR entry,
    LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors)
{
    uint32_t identity_length = 0;
    char* identity = defines || include ? NULL : rsp_builtin_identity(
        source, bytes, name, entry, target, flags1, flags2, &identity_length);
    if (!identity)
        return D3DCompile(source, bytes, name, defines, include, entry, target,
                          flags1, flags2, code, errors);
    const u64 hash = fnv1a(identity, identity_length, 1469598103934665603ull);
    ID3DBlob* blob = shader_disk_cache_load('B', identity, identity_length, hash);
    if (blob) {
        *code = blob;
        if (errors)
            *errors = NULL;
        free(identity);
        return S_OK;
    }
    const double begin = rsp_now_ms();
    const HRESULT hr = D3DCompile(source, bytes, name, defines, include, entry,
                                  target, flags1, flags2, code, errors);
    rsp_rt_note_compile('B', hash, identity_length, rsp_now_ms() - begin,
                        g_ld_frames, hr, ld_shader_cache_dir_or_null());
    if (SUCCEEDED(hr) && *code)
        shader_disk_cache_store('B', identity, identity_length, hash, *code);
    free(identity);
    return hr;
}

'''


def _one(s, needle, what):
    n = s.count(needle)
    if n != 1:
        raise SystemExit(f'anchor {what!r} found {n} times')


def patch(s: str) -> str:
    if MARK in s:
        return s
    _one(s, INCLUDE_ANCHOR, 'include')
    s = s.replace(INCLUDE_ANCHOR, INCLUDE_ANCHOR + INCLUDE_ADD)

    # Loader: replace from the (optional, frozen-only) seed helper or the
    # loader itself up to the store function.
    end = s.index('static void shader_disk_cache_store(')
    frozen = s.find('\n/* Optional portable DXBC seed.')
    start = frozen if 0 <= frozen < end else s.index('static ID3DBlob* shader_disk_cache_load(')
    _one(s, 'static ID3DBlob* shader_disk_cache_load(', 'loader')
    s = s[:start] + LOADER + s[end:]

    # Built-in compile sites: every D3DCompile( outside build_pso.
    b0 = s.index('static ID3D12PipelineState* build_pso(')
    b1 = s.index('static int ld_current_vertex_layout(', b0)
    head, body, tail = s[:b0], s[b0:b1], s[b1:]
    builtin_sites = head.count('D3DCompile(') + tail.count('D3DCompile(')
    if builtin_sites < 10:
        raise SystemExit(f'unexpected built-in compile site count {builtin_sites}')
    if body.count('D3DCompile(') != 2:
        raise SystemExit('build_pso must hold exactly the two generated-shader compiles')
    head = head.replace('D3DCompile(', 'ld_builtin_compile(')
    tail = tail.replace('D3DCompile(', 'ld_builtin_compile(')

    # Generated-shader compiles count as coverage misses.
    for stage, var in (('V', 'vs'), ('P', 'ps')):
        target = 'vs_5_0' if stage == 'V' else 'ps_5_0'
        name = 'xvs' if stage == 'V' else 'xps'
        old = (f'            {var}_hr = D3DCompile(\n'
               f'                {var}_hlsl, {var}_length, "{name}", NULL, NULL, "main",\n'
               f'                "{target}", 0, 0, &{var}, &err);\n')
        _one(body, old, f'{var} compile')
        new = (f'            const double {var}_compile_begin = rsp_now_ms();\n' + old +
               f"            rsp_rt_note_compile('{stage}', {var}_hash, {var}_length,\n"
               f'                rsp_now_ms() - {var}_compile_begin, g_ld_frames, {var}_hr,\n'
               f'                ld_shader_cache_dir_or_null());\n')
        body = body.replace(old, new)

    old = ('    ID3D12PipelineState* pso = NULL;\n'
           '#if defined(YZ_PERF_PROFILE)\n'
           '    g_ld_profile.total.create_pso_calls++;\n'
           '    const LONGLONG create_begin = ld_profile_qpc();\n'
           '#endif\n'
           '    HRESULT hr = g.dev->lpVtbl->CreateGraphicsPipelineState(\n'
           '        g.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);\n')
    _one(body, old, 'create pso')
    new = ('    /* Local pipeline library (built on this PC for this adapter/driver)\n'
           '     * first; a miss creates the PSO exactly as before and records the\n'
           '     * descriptor for the next preparation run. */\n'
           '    rsp_rt_query pso_query;\n'
           '    ID3D12PipelineState* pso = rsp_rt_pso_lookup(\n'
           '        &pso_query, g.dev, ld_shader_cache_dir_or_null(), &pd,\n'
           '        vs_hash, vs_length, ps_hash, ps_length);\n'
           '    HRESULT hr = S_OK;\n'
           '#if defined(YZ_PERF_PROFILE)\n'
           '    g_ld_profile.total.create_pso_calls++;\n'
           '    const LONGLONG create_begin = ld_profile_qpc();\n'
           '#endif\n'
           '    if (!pso) {\n'
           '        hr = g.dev->lpVtbl->CreateGraphicsPipelineState(\n'
           '            g.dev, &pd, &IID_ID3D12PipelineState, (void**)&pso);\n'
           '        rsp_rt_pso_created(&pso_query, hr, g_ld_frames);\n'
           '    }\n')
    body = body.replace(old, new)
    s = head + body + tail

    # Built-in wrapper after the store function.
    anchor = 'static u64 ld_hash_structural_render_state('
    _one(s, anchor, 'wrapper anchor')
    s = s.replace(anchor, BUILTIN.lstrip('\n') + anchor)

    # Original bloom kernels live in a header owned by the bloom work; route
    # its compile through the wrapper without editing that file.
    inc = '#include "rsx_macross_bloom.h"\n'
    if inc in s:
        _one(s, inc, 'bloom include')
        s = s.replace(inc, '#define D3DCompile ld_builtin_compile\n' + inc + '#undef D3DCompile\n')

    # Main draw root signature identity for descriptor records.
    old = ('                                                    &IID_ID3D12RootSignature, (void**)&g.rootsig_x);\n'
           '    sig->lpVtbl->Release(sig);\n')
    _one(s, old, 'rootsig_x')
    s = s.replace(old, old.replace(
        '    sig->lpVtbl->Release(sig);\n',
        '    if (SUCCEEDED(hr))\n'
        '        rsp_rt_register_root(sig->lpVtbl->GetBufferPointer(sig),\n'
        '                             sig->lpVtbl->GetBufferSize(sig), g.rootsig_x);\n'
        '    sig->lpVtbl->Release(sig);\n'))
    return s


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('path', type=Path)
    parser.add_argument('--check', action='store_true', help='verify the patch applies; write nothing')
    a = parser.parse_args()
    raw = a.path.read_bytes()
    crlf = b'\r\n' in raw
    text = raw.decode('utf-8').replace('\r\n', '\n')
    out = patch(text)
    if a.check:
        print('already patched' if out == text else 'applies cleanly')
        return
    if out != text:
        a.path.write_bytes((out.replace('\n', '\r\n') if crlf else out).encode('utf-8'))
        print(f'patched {a.path}')
    else:
        print('already patched')


if __name__ == '__main__':
    main()
