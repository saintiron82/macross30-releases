"""Windows PowerShell 5.1 integration for the production Python probe."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
SCRIPT=ROOT/'tools/local_build/setup_tools.ps1'
PS=Path('C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe')
CLANG=Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang.exe')


@unittest.skipUnless(os.name=='nt' and CLANG.is_file(), 'Windows PowerShell native-process integration')
class PythonProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='python-probe-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory=Path(cls.temp.name)
        source=cls.directory/'probe.c'
        source.write_text(r'''
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
 const char* mode=getenv("MACROSS_PROBE_CASE");
 if(mode && !strcmp(mode,"old")){fputs("Traceback (most recent call last):\nAssertionError\n",stderr);return 1;}
 if(mode && !strcmp(mode,"broken")){fputs("Fatal Python error\n",stderr);return 86;}
 if(mode && !strcmp(mode,"hang")){Sleep(30000);return 1;}
 if(mode && !strcmp(mode,"noisy"))fputs("benign interpreter warning\n",stderr);
 if(mode && !strcmp(mode,"flood"))for(int i=0;i<200000;i++){fputc('x',stdout);fputc('y',stderr);}
 return 0;
}
''',encoding='utf-8')
        cls.fake=cls.directory/'Python 폴더 space/python.exe';cls.fake.parent.mkdir()
        r=subprocess.run([str(CLANG),str(source),'-o',str(cls.fake)],capture_output=True)
        if r.returncode:raise RuntimeError(r.stderr.decode(errors='replace'))
        cls.harness=cls.directory/'invoke.ps1'
        cls.harness.write_text('''$ErrorActionPreference='Stop'
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($env:MACROSS_SETUP_SCRIPT,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Setup script parse errors'}
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-LocalPython'},$true)
Invoke-Expression $function.Extent.Text
$value=Test-LocalPython -Executable $env:MACROSS_PROBE_EXE -TimeoutMilliseconds ([int]$env:MACROSS_PROBE_TIMEOUT)
@{usable=[bool]$value;output_count=@($value).Count}|ConvertTo-Json -Compress
''',encoding='utf-8')

    def probe(self,executable,mode='',timeout=2000,extra=None):
        env=dict(os.environ,MACROSS_SETUP_SCRIPT=str(SCRIPT),MACROSS_PROBE_EXE=str(executable),
                 MACROSS_PROBE_CASE=mode,MACROSS_PROBE_TIMEOUT=str(timeout))
        env.update(extra or {})
        r=subprocess.run([str(PS),'-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',str(self.harness)],
                         capture_output=True,env=env,timeout=15)
        self.assertEqual(r.returncode,0,r.stderr.decode(errors='replace'))
        result=json.loads(r.stdout.decode('utf-8-sig'))
        self.assertEqual(result['output_count'],1)
        return result['usable']

    def test_supported_real_python(self):
        self.assertTrue(self.probe(sys.executable))

    def test_unsupported_python_stderr_is_a_rejection_not_a_powershell_abort(self):
        self.assertFalse(self.probe(self.fake,'old'))

    def test_broken_python_falls_back(self):
        self.assertFalse(self.probe(self.fake,'broken'))

    def test_missing_python_falls_back(self):
        self.assertFalse(self.probe(self.directory/'missing.exe'))

    def test_hanging_python_is_stopped(self):
        self.assertFalse(self.probe(self.fake,'hang',timeout=100))

    def test_successful_python_can_write_stderr(self):
        self.assertTrue(self.probe(self.fake,'noisy'))

    def test_both_output_pipes_are_drained(self):
        self.assertTrue(self.probe(self.fake,'flood',timeout=5000))

    def test_user_pythonpath_does_not_corrupt_the_version_probe(self):
        fixture=self.directory/'bad site';fixture.mkdir()
        (fixture/'sitecustomize.py').write_text('raise RuntimeError("unexpected user startup")\n',encoding='utf-8')
        self.assertTrue(self.probe(sys.executable,extra={'PYTHONPATH':str(fixture)}))


if __name__=='__main__':unittest.main()
