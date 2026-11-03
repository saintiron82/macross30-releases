"""Read-only selection of the exact localized binary approved for packaging."""
import json
from pathlib import Path
from native_binary_launch import select_native_binary, battle_subtitles_default, sha_file


def program_inputs(root):
    root = Path(root).resolve()
    exe = select_native_binary(root)
    enabled = battle_subtitles_default(root, exe)
    provenance = dict(exe_sha256=sha_file(exe), battle_subtitles=enabled)
    if enabled:
        pointer = root / 'native/subtitle-candidate/active.json'
        active = json.loads(pointer.read_text(encoding='utf-8'))
        proof_path = (root / active['inherited_runtime_proof']).resolve()
        if not proof_path.is_relative_to(root / 'native/subtitle-candidate'):
            raise ValueError('Subtitle proof outside candidate directory')
        proof = json.loads(proof_path.read_text(encoding='utf-8'))
        if (proof.get('status') != 'DATA_ONLY_BRIDGE_RUNTIME_INHERITED' or
                proof.get('bridge_runtime_inherited_verified') is not True or
                proof.get('candidate_sha256') != sha_file(exe)):
            raise ValueError('Subtitle runtime proof does not match selected binary')
        for row in proof['evidence'] + [proof['catalog_test_snapshot'], proof['catalog_build_snapshot']]:
            path = (root / row['path']).resolve()
            if not path.is_relative_to(root) or sha_file(path) != row['sha256']:
                raise ValueError('Subtitle evidence checksum changed')
        provenance.update(candidate_live_game_verified=proof['candidate_live_game_verified'],
                          bridge_runtime_inherited_verified=True,
                          proof_sha256=sha_file(proof_path))
    return exe, provenance
