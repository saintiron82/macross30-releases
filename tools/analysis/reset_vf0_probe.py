"""Reset only this experiment's bounded capture counter, with source/PID guards."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[2]


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--apply',action='store_true');args=ap.parse_args()
    latest=json.loads((ROOT/'experiments/vf0-hd-v1/latest-run.json').read_text(encoding='utf8'))
    assert latest['candidate']=='vf0-hd-v2-r2'
    run=Path(latest['run']);process=json.loads((run/'process.json').read_text(encoding='utf8'))
    folder=ROOT/'native/subtitle-candidate'/latest['candidate'];expected=folder/'Macross30Recompiled.exe'
    build=json.loads((folder/'build-result.json').read_text(encoding='utf8'))
    assert hashlib.sha256(expected.read_bytes()).hexdigest()==build['products']['exe']['sha256']
    text=(folder/'Macross30Recompiled.map').read_text(encoding='utf8',errors='replace')
    symbol=int(re.search(r'^\s*\S+\s+vf0_probe_texture\.count\s+([0-9a-fA-F]+)',text,re.M)[1],16)
    preferred=int(re.search(r'Preferred load address is\s+([0-9a-fA-F]+)',text)[1],16)
    kernel=C.WinDLL('kernel32',use_last_error=True);psapi=C.WinDLL('psapi',use_last_error=True)
    kernel.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];kernel.OpenProcess.restype=W.HANDLE
    kernel.CloseHandle.argtypes=[W.HANDLE]
    kernel.ReadProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t)]
    kernel.WriteProcessMemory.argtypes=kernel.ReadProcessMemory.argtypes
    psapi.EnumProcessModules.argtypes=[W.HANDLE,C.POINTER(W.HMODULE),W.DWORD,C.POINTER(W.DWORD)]
    psapi.GetModuleFileNameExW.argtypes=[W.HANDLE,W.HMODULE,W.LPWSTR,W.DWORD]
    h=kernel.OpenProcess(0x400|0x10|(0x20|0x8 if args.apply else 0),False,process['pid'])
    if not h:raise C.WinError(C.get_last_error())
    try:
        modules=(W.HMODULE*1024)();needed=W.DWORD()
        if not psapi.EnumProcessModules(h,modules,C.sizeof(modules),C.byref(needed)):raise C.WinError(C.get_last_error())
        path=C.create_unicode_buffer(32768)
        assert psapi.GetModuleFileNameExW(h,modules[0],path,len(path))
        assert Path(path.value).resolve()==expected.resolve()
        address=int(modules[0])+symbol-preferred;count=W.DWORD();done=C.c_size_t()
        if not kernel.ReadProcessMemory(h,address,C.byref(count),4,C.byref(done)) or done.value!=4:raise C.WinError(C.get_last_error())
        before=count.value;assert before<=64
        if args.apply:
            count.value=0
            if not kernel.WriteProcessMemory(h,address,C.byref(count),4,C.byref(done)) or done.value!=4:raise C.WinError(C.get_last_error())
        result=dict(pid=process['pid'],symbol='vf0_probe_texture.count',before=before,reset=args.apply,only_diagnostic_counter_written=args.apply)
        (run/'probe-reset.json').write_text(json.dumps(result,indent=2),encoding='utf8');print(json.dumps(result))
    finally:kernel.CloseHandle(h)


if __name__=='__main__':main()
