using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading;

namespace MacrossLauncher {
public static class SelfUpdate {
    public const string Version = "0.2.13";
    public static void ValidateManifest(Dictionary<string,object> manifest) {
        string version = Packages.Text(manifest,"version"); Packages.VersionOf(version);
        if (Packages.Text(manifest,"file") != "Macross30Launcher-" + version + ".exe" ||
            Convert.ToInt64(manifest["bytes"]) <= 0 || Convert.ToInt64(manifest["bytes"]) > 32 * 1024 * 1024 ||
            !System.Text.RegularExpressions.Regex.IsMatch(Packages.Text(manifest,"sha256"), "^[a-f0-9]{64}$"))
            throw new InvalidDataException(UiText.T("런처 업데이트 정보가 잘못됐습니다."));
    }
    public static void ReplaceAndStart(string target, string pending, string backup, string currentHash, string nextHash, Action start) {
        if (Packages.Hash(target) != currentHash || Packages.Hash(pending) != nextHash)
            throw new InvalidDataException(UiText.T("런처 교체 전 해시가 변경됐습니다."));
        ReplaceFile(pending,target,backup);
        try { start(); }
        catch {
            ReplaceFile(backup,target,null);
            throw;
        }
    }
    static void ReplaceFile(string source,string target,string backup) {
        for(int attempt=0;;attempt++) {
            try {File.Replace(source,target,backup);return;}
            catch(IOException e) {
                int code=e.HResult & 0xffff;
                if(attempt>=49 || (code!=32 && code!=33))throw;
                Thread.Sleep(100); // one-file bootloader may still be releasing the EXE
            }
        }
    }
    static ProcessStartInfo RestartInfo(string target,string args) {
        var info=new ProcessStartInfo(target,args) {UseShellExecute=false,WorkingDirectory=Path.GetDirectoryName(target)};
        info.EnvironmentVariables["PYINSTALLER_RESET_ENVIRONMENT"]="1";
        return info;
    }
    public static void ValidatePlan(string path, Dictionary<string,object> plan) {
        string work = Path.GetDirectoryName(Path.GetFullPath(path));
        string target = Path.GetFullPath(Packages.Text(plan,"target"));
        if (Path.GetFileName(path) != "plan.json" || !System.Text.RegularExpressions.Regex.IsMatch(Path.GetFileName(target),@"^Macross30Launcher(?:-\d+\.\d+\.\d+)?\.exe$",System.Text.RegularExpressions.RegexOptions.IgnoreCase) ||
            !System.Text.RegularExpressions.Regex.IsMatch(Path.GetFileName(work), "^\\.launcher-update-[a-f0-9]{32}$") ||
            !String.Equals(Path.GetDirectoryName(work),Path.GetDirectoryName(target),StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException(UiText.T("런처 업데이트 계획의 경로가 잘못됐습니다."));
        Packages.NoLinks(Path.GetDirectoryName(target),Path.GetFileName(work));
        Packages.NoLinks(Path.GetDirectoryName(target),Path.GetFileName(target));
    }
    public static void Run(string path) {
        var plan = Packages.Read(File.ReadAllText(path)); ValidatePlan(path,plan);
        string work = Path.GetDirectoryName(Path.GetFullPath(path));
        string target = Path.GetFullPath(Packages.Text(plan,"target"));
        string actual = Process.GetCurrentProcess().MainModule.FileName;
        if (!String.Equals(actual,Path.Combine(work,"helper.exe"),StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException(UiText.T("보조 런처 실행 경로가 다릅니다."));
        try {
            using (var parent = Process.GetProcessById(Convert.ToInt32(plan["pid"]))) {
                if (parent.StartTime.ToUniversalTime().Ticks != Convert.ToInt64(plan["started"])) throw new IOException(UiText.T("런처 프로세스 식별자가 바뀌었습니다."));
                if (!String.Equals(parent.MainModule.FileName,target,StringComparison.OrdinalIgnoreCase)) throw new IOException(UiText.T("교체 대상과 실행 중인 런처가 다릅니다."));
                if (!parent.WaitForExit(30000)) throw new IOException(UiText.T("기존 런처가 종료되지 않아 교체를 중단했습니다."));
            }
        } catch (ArgumentException) { /* parent already exited */ }
        string health = Path.Combine(work,"health.txt");
        string config=plan.ContainsKey("config") ? Packages.Text(plan,"config") : "";
        if(config.Length>0 && (!Path.IsPathRooted(config) || config.IndexOf('"')>=0 || config.IndexOf('\0')>=0))throw new InvalidDataException("Invalid restart configuration path");
        string configArgs=config.Length>0 ? " --config \""+config+"\"" : "";
        bool replaced = false;
        try {
            ReplaceAndStart(target,Path.Combine(work,"new.exe"),Path.Combine(work,"previous.exe"),Packages.Text(plan,"current_sha256"),Packages.Text(plan,"sha256"), () => {
                replaced = true;
                using (var child = Process.Start(RestartInfo(target,"--update-health \"" + health + "\""+configArgs))) {
                    var timer = Stopwatch.StartNew();
                    while (timer.ElapsedMilliseconds < 15000) {
                        if (File.Exists(health) && File.ReadAllText(health) == Packages.Text(plan,"sha256")) return;
                        if (child.HasExited) break;
                        Thread.Sleep(100);
                    }
                    if (!child.HasExited) { child.Kill(); child.WaitForExit(5000); }
                    throw new IOException(UiText.T("새 런처 시작 확인에 실패하여 이전 버전으로 복구했습니다."));
                }
            });
            File.WriteAllText(Path.Combine(work,"result.txt"),"SUCCESS");
        } catch (Exception error) {
            File.WriteAllText(Path.Combine(work,"result.txt"),"FAILED: "+error.Message+"; backup=" + Path.Combine(work,"previous.exe"));
            if (replaced) Process.Start(RestartInfo(target,"--skip-launcher-update"+configArgs));
            throw;
        }
    }
}
}
