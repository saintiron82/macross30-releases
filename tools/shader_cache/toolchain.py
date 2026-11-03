"""Locate the clang-cl/MSVC/Windows SDK toolchain used by the private candidate builds."""
import os
from pathlib import Path

VS = Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.50.35717')
SDK = Path('C:/Program Files (x86)/Windows Kits/10')
SDK_VERSION = '10.0.26100.0'


def environment() -> dict:
    env = dict(os.environ)
    inc = SDK / 'Include' / SDK_VERSION
    lib = SDK / 'Lib' / SDK_VERSION
    env['INCLUDE'] = ';'.join(map(str, [VS / 'include', inc / 'ucrt', inc / 'shared', inc / 'um', inc / 'winrt']))
    env['LIB'] = ';'.join(map(str, [VS / 'lib/x64', lib / 'ucrt/x64', lib / 'um/x64']))
    return env


def clang_cl() -> str:
    return str(VS.parent.parent / 'Llvm/x64/bin/clang-cl.exe')


def missing() -> list:
    return [str(p) for p in (VS, SDK / 'Include' / SDK_VERSION, Path(clang_cl())) if not Path(p).exists()]
