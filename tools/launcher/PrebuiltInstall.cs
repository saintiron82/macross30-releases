using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

namespace MacrossLauncher {
public static class PrebuiltInstall {
    [DllImport("kernel32.dll")] static extern bool IsProcessorFeaturePresent(uint feature);
    public static void CheckRuntime() { if(!IsProcessorFeaturePresent(40))throw new InvalidOperationException("This game runtime requires an AVX2-capable processor."); }
    public const string Mode="user-original-eboot";
    public static void Validate(Dictionary<string,object> manifest) {
        if(Packages.Text(manifest,"mode")!=Mode || Packages.Text(manifest,"original_elf_sha256")!=LocalBuild.OriginalHash)throw new InvalidDataException("Unsupported EBOOT installation package");
        Packages.VersionOf(Packages.Text(manifest,"version"));
        if(manifest.ContainsKey("min_launcher_version") && Packages.VersionOf(Packages.Text(manifest,"min_launcher_version"))>Packages.VersionOf(SelfUpdate.Version))throw new InvalidDataException("Update the launcher first");
        var files=Packages.Object(manifest["files"]);
        bool split=manifest.ContainsKey("language_packs");
        if(!files.ContainsKey("program") || files.Count!=(split ? 1 : 2) || !split && !files.ContainsKey("data"))throw new InvalidDataException("Invalid EBOOT package list");
        foreach(string kind in split ? new[]{"program"} : new[]{"program","data"}) {
            var row=Packages.Object(files[kind]);string prefix=kind=="program" ? "Macross30-program-" : "Macross30-local-data-";
            if(!Regex.IsMatch(Packages.Text(row,"file"),"^"+Regex.Escape(prefix)+@"[0-9]+\.[0-9]+\.[0-9]+\.zip$") || !Regex.IsMatch(Packages.Text(row,"sha256"),"^[a-f0-9]{64}$") || Convert.ToInt64(row["bytes"])<=0 || Convert.ToInt64(row["bytes"])>1024L*1024*1024)throw new InvalidDataException("Invalid EBOOT artifact identity");
        }
        if(split)foreach(var item in Packages.Object(manifest["language_packs"])) {
            if(!GameLanguage.Codes.Contains(item.Key))throw new InvalidDataException("Unsupported language pack");
            var row=Packages.Object(item.Value);
            if(!Regex.IsMatch(Packages.Text(row,"file"),"^Macross30-language-"+Regex.Escape(item.Key)+@"-[0-9]+\.[0-9]+\.[0-9]+\.zip$") || !Regex.IsMatch(Packages.Text(row,"sha256"),"^[a-f0-9]{64}$") || Convert.ToInt64(row["bytes"])<=0 || Convert.ToInt64(row["bytes"])>1024L*1024*1024)throw new InvalidDataException("Invalid language artifact identity");
        }
        if(manifest.ContainsKey("base_language")) {
            var original=Packages.Object(manifest["base_language"]);
            if(Packages.Text(original,"locale")!="ja-JP" || Packages.Text(original,"elf_sha256")!=LocalBuild.OriginalHash || !Regex.IsMatch(Packages.Text(original,"plan_sha256"),"^[a-f0-9]{64}$") || !Regex.IsMatch(Packages.Text(original,"data_id"),"^[a-f0-9]{16}$"))throw new InvalidDataException("Invalid original game preparation identity");
        }
    }
    public static bool IsBaseLanguage(Dictionary<string,object> manifest,string locale) {return manifest!=null && manifest.ContainsKey("base_language") && Packages.Text(Packages.Object(manifest["base_language"]),"locale")==locale;}
    public static Dictionary<string,object> Artifacts(Dictionary<string,object> manifest,string locale) {
        var files=Packages.Object(manifest["files"]);
        if(IsBaseLanguage(manifest,locale))return new Dictionary<string,object>{{"program",files["program"]}};
        return new Dictionary<string,object>{{"program",files["program"]},{"data",manifest.ContainsKey("language_packs") ? Packages.Object(manifest["language_packs"])[locale] : files["data"]}};
    }
    // Status is independent for common runtime and each locale: 0 absent,
    // 1 installed but outdated/unverified, 2 verified against latest metadata.
    sealed class StatusHash { public long Size,Ticks; public string Hash; }
    static readonly Dictionary<string,StatusHash> statusHashes=new Dictionary<string,StatusHash>(StringComparer.OrdinalIgnoreCase);
    static string CachedStatusHash(string path) {
        path=Path.GetFullPath(path);var file=new FileInfo(path);
        if(!file.Exists)return "";
        lock(statusHashes) {
            StatusHash saved;
            if(statusHashes.TryGetValue(path,out saved) && saved.Size==file.Length && saved.Ticks==file.LastWriteTimeUtc.Ticks)return saved.Hash;
            long size=file.Length,ticks=file.LastWriteTimeUtc.Ticks;string hash=Packages.Hash(path);
            file.Refresh();if(!file.Exists || file.Length!=size || file.LastWriteTimeUtc.Ticks!=ticks)return "";
            if(statusHashes.Count>=64)statusHashes.Clear();
            statusHashes[path]=new StatusHash{Size=size,Ticks=ticks,Hash=hash};return hash;
        }
    }
    public static int LanguageStatus(string root,string locale,Dictionary<string,object> latest) {
        try {
            string profilePath=Path.Combine(root,"release-profile.json");if(!File.Exists(profilePath))return 0;
            var profile=Packages.Read(File.ReadAllText(profilePath));
            if(!profile.ContainsKey("language_views"))return Packages.Text(profile,"game_language")==locale ? 1 : 0;
            var views=Packages.Object(profile["language_views"]);if(!views.ContainsKey(locale))return 0;
            var view=Packages.Object(views[locale]);string dataRoot=Packages.Child(root,Packages.Text(view,"root"));
            string elf=Packages.Child(root,Packages.Text(view,"elf"));
            if(!Directory.Exists(dataRoot) || !File.Exists(elf) || !File.Exists(Path.Combine(dataRoot,"data/installed-version.txt")))return 0;
            if(!view.ContainsKey("files"))return 1;
            foreach(var value in Packages.Array(view["files"])) {
                var row=Packages.Object(value);var file=new FileInfo(Packages.Child(dataRoot,Packages.Text(row,"path")));
                if(!file.Exists)return 0;
                if(file.Length!=Convert.ToInt64(row["size"]) || (file.LastWriteTimeUtc.Ticks-621355968000000000L)*100L!=Convert.ToInt64(row["mtime_ns"]))return 1;
            }
            if(latest==null)return 1;
            if(File.ReadAllText(Path.Combine(dataRoot,"data/installed-version.txt")).Trim()!=Packages.Text(view,"data_id") || CachedStatusHash(elf)!=Packages.Text(view,"elf_sha256"))return 1;
            if(latest==null || !latest.ContainsKey("language_packs"))return 1;
            var expectedViews=Packages.Object(latest["language_packs"]);if(!IsBaseLanguage(latest,locale) && !expectedViews.ContainsKey(locale))return 1;
            var expected=Packages.Object(IsBaseLanguage(latest,locale) ? latest["base_language"] : expectedViews[locale]);
            return new[]{"data_id","elf_sha256","plan_sha256"}.All(key=>!String.IsNullOrEmpty(Packages.Text(expected,key)) && Packages.Text(view,key)==Packages.Text(expected,key)) ? 2 : 1;
        } catch {return 1;}
    }
    public static string LanguageVersion(string root,string locale,Dictionary<string,object> latest,bool installed) {
        if(locale=="ja-JP")return UiText.T("원본 일본어");
        try {
            Dictionary<string,object> expected=null;
            if(latest!=null && latest.ContainsKey("language_packs")){var packs=Packages.Object(latest["language_packs"]);if(packs.ContainsKey(locale))expected=Packages.Object(packs[locale]);}
            if(!installed)return expected==null?UiText.T("확인 중"):Regex.Match(Packages.Text(expected,"file"),@"-(\d+\.\d+\.\d+)\.zip$").Groups[1].Value;
            string path=Path.Combine(root,"release-profile.json");if(!File.Exists(path))return UiText.T("미설치");
            var profile=Packages.Read(File.ReadAllText(path));var views=Packages.Object(profile["language_views"]);if(!views.ContainsKey(locale))return UiText.T("미설치");
            var view=Packages.Object(views[locale]);string version=view.ContainsKey("version")?Packages.Text(view,"version"):"";if(!String.IsNullOrEmpty(version))return version;
            if(expected!=null && new[]{"data_id","elf_sha256","plan_sha256"}.All(key=>!String.IsNullOrEmpty(Packages.Text(expected,key)) && Packages.Text(view,key)==Packages.Text(expected,key)))return Regex.Match(Packages.Text(expected,"file"),@"-(\d+\.\d+\.\d+)\.zip$").Groups[1].Value;
            return UiText.T("확인 불가");
        }catch{return UiText.T("확인 불가");}
    }
    public static void RecordLanguageVersion(string root,string locale,string version) {
        Packages.VersionOf(version);string path=Packages.Child(root,"release-profile.json");Packages.NoLinks(root,"release-profile.json");
        var profile=Packages.Read(File.ReadAllText(path));var view=Packages.Object(Packages.Object(profile["language_views"])[locale]);view["version"]=version;
        string pending=Packages.Child(root,"release-profile.json.language-version-pending");Packages.NoLinks(root,"release-profile.json.language-version-pending");
        File.WriteAllText(pending,new System.Web.Script.Serialization.JavaScriptSerializer().Serialize(profile));File.Replace(pending,path,null);
    }
    public static string InstalledProgram(string root) {
        var profile=Packages.Read(File.ReadAllText(Path.Combine(root,"release-profile.json")));
        string relative=Packages.Text(profile,"program_cache");
        if(String.IsNullOrEmpty(relative) || Path.IsPathRooted(relative))throw new InvalidDataException("Installed program cache is missing");
        string program=Packages.Child(root,relative);Packages.NoLinks(root,relative);
        string manifestPath=Path.Combine(program,"program-manifest.json");Packages.NoLinks(root,relative+"/program-manifest.json");
        string expected=Packages.Text(profile,"program_manifest_sha256");
        if(!Regex.IsMatch(expected,"^[a-f0-9]{64}$") || CachedStatusHash(manifestPath)!=expected)throw new InvalidDataException("Installed program manifest verification failed");
        var manifest=Packages.Read(File.ReadAllText(manifestPath));
        var rows=Packages.Array(manifest["files"]);if(!rows.Any() || !rows.Any(value=>Packages.Text(Packages.Object(value),"path")=="tools/local_build/install_language.ps1"))throw new InvalidDataException("Installed program cache is empty");
        foreach(var value in rows) {
            var row=Packages.Object(value);string name=Packages.Text(row,"path"),path=Packages.Child(program,name);
            Packages.NoLinks(program,name);var file=new FileInfo(path);
            if(!file.Exists || file.Length!=Convert.ToInt64(row["bytes"]) || CachedStatusHash(path)!=Packages.Text(row,"sha256"))throw new InvalidDataException("Installed program cache file verification failed: "+name);
        }
        if(!File.Exists(Path.Combine(program,"tools/local_build/install_language.ps1")))throw new InvalidDataException("Installed program does not support language operations");
        return program;
    }
    public static bool CoreReady(string root) {
        try {var profile=Packages.Read(File.ReadAllText(Path.Combine(root,"release-profile.json")));return BaseReadyCached(root,profile) && CachedStatusHash(Path.Combine(root,"bin/Macross30Recompiled.exe"))==Packages.Text(profile,"exe_sha256") && !String.IsNullOrEmpty(InstalledProgram(root));}catch{return false;}
    }
    public static void RecordProgram(string root,string program,Dictionary<string,object> artifact) {
        string full=Path.GetFullPath(root).TrimEnd('\\','/'),cache=Path.GetFullPath(program);
        if(!cache.StartsWith(full+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("Program cache is outside installation");
        string relative=cache.Substring(full.Length+1).Replace('\\','/');Packages.NoLinks(root,relative);
        string path=Packages.Child(root,"release-profile.json");Packages.NoLinks(root,"release-profile.json");
        var profile=Packages.Read(File.ReadAllText(path));profile["program_cache"]=relative;
        profile["program_manifest_sha256"]=Packages.Hash(Path.Combine(cache,"program-manifest.json"));profile["program_archive_sha256"]=Packages.Text(artifact,"sha256");profile["program_artifact"]=artifact;
        string pending=Packages.Child(root,"release-profile.json.core-pending");Packages.NoLinks(root,"release-profile.json.core-pending");
        File.WriteAllText(pending,new System.Web.Script.Serialization.JavaScriptSerializer().Serialize(profile));File.Replace(pending,path,null);
    }
    static bool BaseReadyCached(string root,Dictionary<string,object> profile) {
        if(Packages.Text(profile,"mode")!=Mode || Packages.Text(profile,"original_elf_sha256")!=LocalBuild.OriginalHash || !Convert.ToBoolean(profile["original_generated_locally"]))return false;
        var view=Packages.Object(Packages.Object(profile["language_views"])["ja-JP"]);
        string dataRoot=Packages.Child(root,Packages.Text(view,"root"));
        foreach(var value in Packages.Array(view["files"])) {
            var row=Packages.Object(value);var file=new FileInfo(Packages.Child(dataRoot,Packages.Text(row,"path")));
            if(!file.Exists || file.Length!=Convert.ToInt64(row["size"]) || (file.LastWriteTimeUtc.Ticks-621355968000000000L)*100L!=Convert.ToInt64(row["mtime_ns"]))return false;
        }
        if(File.ReadAllText(Path.Combine(dataRoot,"data/installed-version.txt")).Trim()!=Packages.Text(view,"data_id") || CachedStatusHash(Packages.Child(root,Packages.Text(view,"elf")))!=Packages.Text(view,"elf_sha256"))return false;
        return new[]{"Macross30.cmd","tools/launch-helper.ps1","bin/SDL2.dll","bin/msvcp140.dll","bin/vcruntime140.dll","bin/vcruntime140_1.dll"}.All(p=>File.Exists(Path.Combine(root,p)));
    }
    public static int RuntimeStatus(string root,Dictionary<string,object> latest) {
        try {
            string exe=Path.Combine(root,"bin/Macross30Recompiled.exe"),profilePath=Path.Combine(root,"release-profile.json");
            if(!File.Exists(exe) || !File.Exists(profilePath))return 0;
            var profile=Packages.Read(File.ReadAllText(profilePath));string hash=CachedStatusHash(exe);
            if(String.IsNullOrEmpty(hash) || hash!=Packages.Text(profile,"exe_sha256") || latest==null)return 1;
            InstalledProgram(root);
            var expected=Packages.Object(Packages.Object(latest["files"])["program"]);
            string installed=Packages.Text(profile,"program_archive_sha256"),available=Packages.Text(expected,"sha256");
            return Regex.IsMatch(installed,"^[a-f0-9]{64}$") && installed==available ? 2 : 1;
        } catch {return 1;}
    }
    public static bool LanguageCurrent(string root,string locale,Dictionary<string,object> manifest) {
        if(!manifest.ContainsKey("language_packs") || !LocalBuild.Ready(root,locale))return false;
        try {
            var expected=Packages.Object(IsBaseLanguage(manifest,locale) ? manifest["base_language"] : Packages.Object(manifest["language_packs"])[locale]);
            var profile=Packages.Read(File.ReadAllText(Path.Combine(root,"release-profile.json")));
            var actual=Packages.Object(Packages.Object(profile["language_views"])[locale]);
            return new[]{"data_id","elf_sha256","plan_sha256"}.All(key=>Packages.Text(actual,key)==Packages.Text(expected,key));
        } catch {return false;}
    }
    public static Dictionary<string,object> BundledPackage(string directory) {return BundledPackage(directory,null);}
    public static Dictionary<string,object> BundledPackage(string directory,string locale) {
        string path=Path.Combine(directory,"TEST-INSTALL.json");if(!File.Exists(path))return null;
        var manifest=Packages.Read(File.ReadAllText(path));Validate(manifest);
        var rows=locale==null ? Packages.Object(manifest["files"]) : Artifacts(manifest,locale);
        foreach(var value in rows.Values) { var row=Packages.Object(value);Packages.VerifyArchive(Path.Combine(directory,Packages.Text(row,"file")),row); }
        return manifest;
    }
    public static void UpdateRuntime(string root,string program,string locale,string version) {CommitRuntime(root,program,locale,version,true);}
    public static void StageRuntime(string root,string program,string version) {CommitRuntime(root,program,"ja-JP",version,false);}
    static void CommitRuntime(string root,string program,string locale,string version,bool requireLanguage) {
        if(requireLanguage && !LocalBuild.Ready(root,locale))throw new InvalidDataException("Installed language is not ready");
        Packages.EnsureStopped(root);Packages.NoLinks(root,".local-build");Packages.NoLinks(root,"play/runner.lock");
        Directory.CreateDirectory(Path.Combine(root,"play"));
        using(var lease=new FileStream(Path.Combine(root,"play/runner.lock"),FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.None)) {
            var manifest=Packages.Read(File.ReadAllText(Path.Combine(program,"program-manifest.json")));
            var rows=Packages.Array(manifest["files"]).Select(Packages.Object).ToArray();
            string work=Packages.Child(root,".local-build/runtime-update-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(work);
            var staged=new List<KeyValuePair<string,string>>();var committed=new List<KeyValuePair<string,string>>();bool cleanWork=true;
            try {
                foreach(var row in rows) {
                    string name=Packages.Text(row,"path"),source=Packages.Child(program,name);Packages.VerifyArchive(source,row);
                    if(!name.StartsWith("bin/",StringComparison.Ordinal) && name!="THIRD-PARTY-NOTICES.txt" && name!="tools/play/launch-locale.ps1")continue;
                    string target=Packages.Child(root,name);Packages.NoLinks(root,name);Directory.CreateDirectory(Path.GetDirectoryName(target));
                    if(File.Exists(target) && Packages.Hash(target)==Packages.Text(row,"sha256"))continue;
                    string pending=Path.Combine(work,staged.Count+".new");File.Copy(source,pending);Packages.VerifyArchive(pending,row);
                    staged.Add(new KeyValuePair<string,string>(target,pending));
                }
                string profilePath=Packages.Child(root,"release-profile.json");Packages.NoLinks(root,"release-profile.json");
                var profile=File.Exists(profilePath)?Packages.Read(File.ReadAllText(profilePath)):new Dictionary<string,object>{{"mode",Mode},{"original_elf_sha256",LocalBuild.OriginalHash},{"original_generated_locally",false},{"generated_on_this_machine",false},{"downloaded_game_executable",true},{"language_views",new Dictionary<string,object>()}};
                profile["release_version"]=version;profile["program_files"]=rows.Where(row=>Packages.Text(row,"path").StartsWith("bin/",StringComparison.Ordinal)).ToArray();
                profile["exe_sha256"]=Packages.Text(rows.Single(row=>Packages.Text(row,"path")=="bin/Macross30Recompiled.exe"),"sha256");
                string next=Path.Combine(work,"profile.new");File.WriteAllText(next,new System.Web.Script.Serialization.JavaScriptSerializer().Serialize(profile));
                staged.Add(new KeyValuePair<string,string>(profilePath,next));
                foreach(var item in staged) {
                    string backup=File.Exists(item.Key) ? Path.Combine(work,committed.Count+".old") : null;
                    if(backup!=null)File.Replace(item.Value,item.Key,backup);else File.Move(item.Value,item.Key);
                    committed.Add(new KeyValuePair<string,string>(item.Key,backup));
                }
                if(requireLanguage && !LocalBuild.Ready(root,locale) || !requireLanguage && Packages.Hash(Path.Combine(root,"bin/Macross30Recompiled.exe"))!=Packages.Text(profile,"exe_sha256"))throw new InvalidDataException("Updated runtime verification failed");
            } catch(Exception failure) {
                var recoveryErrors=new List<Exception>();
                for(int i=committed.Count-1;i>=0;i--)try {
                    var item=committed[i];if(item.Value==null)File.Delete(item.Key);else File.Replace(item.Value,item.Key,null);
                } catch(Exception recovery) {recoveryErrors.Add(recovery);}
                if(recoveryErrors.Count>0) {cleanWork=false;recoveryErrors.Insert(0,failure);throw new AggregateException("Runtime rollback needs recovery; backups retained in "+work,recoveryErrors);}
                throw;
            } finally {if(cleanWork)Directory.Delete(work,true);}
            if(requireLanguage)LocalBuild.SelectLanguage(root,locale,UiText.Language);
        }
    }
    static bool Allowed(string path) {
        string name=Path.GetFileName(path).ToLowerInvariant(),extension=Path.GetExtension(path).ToLowerInvariant();
        if(path=="bin/Macross30Recompiled.exe")return true;
        // Shader-prep assets (tools/shader_cache/stage_program_assets.py): preparation tool,
        // seed manifest, portable PSO inventory and exact-source DXBC seed entries only.
        if(path=="bin/MacrossShaderPrep.exe" || path=="bin/assets/shader-seed-manifest-v3.json" || path=="bin/assets/rsx_pso_inventory_v1.bin" || path=="bin/assets/aircraft-hd-recipe.json" || path=="bin/assets/locale-base.json")return true;
        if(Regex.IsMatch(path,@"^bin/assets/rsx_shader_cache_v2/[VPB]_[0-9A-F]{16}_[0-9A-F]{8}\.bin$"))return true;
        if(path.StartsWith("base-game/",StringComparison.Ordinal))return new[]{"CollectLogs.cmd","DistInstaller.cs","Macross30.cmd","launch-tools/collect-logs.ps1","launch-tools/launch-helper.ps1","plans/ja-JP.tsv","recipe.json"}.Contains(path.Substring(10));
        if(extension==".dll")return path.StartsWith("bin/",StringComparison.Ordinal) && new[]{"sdl2.dll","avutil-61.dll","avcodec-63.dll","avformat-63.dll","swresample-7.dll","msvcp140.dll","vcruntime140.dll","vcruntime140_1.dll","msvcp140_1.dll","msvcp140_2.dll","msvcp140_atomic_wait.dll","msvcp140_codecvt_ids.dll","vcruntime140_threads.dll","concrt140.dll"}.Contains(name);
        return path=="program-manifest.json" || (path.StartsWith("tools/",StringComparison.Ordinal) || !path.Contains("/")) && new[]{".ps1",".py",".json",".txt",".md"}.Contains(extension);
    }
    public static string Extract(string root,string archive,Dictionary<string,object> row) {
        Packages.VerifyArchive(archive,row);
        string directory=Packages.Child(root,".local-build/program-"+Packages.Text(row,"sha256").Substring(0,16));Packages.NoLinks(root,".local-build");
        string relativeDirectory=directory.Substring(Path.GetFullPath(root).TrimEnd('\\','/').Length+1);Packages.NoLinks(root,relativeDirectory);Directory.CreateDirectory(directory);
        using(var zip=ZipFile.OpenRead(archive)) {
            if(zip.Entries.Count>1000)throw new InvalidDataException("Too many program files");
            var names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);long total=0;
            foreach(var entry in zip.Entries) {
                if(!entry.FullName.StartsWith("program/",StringComparison.Ordinal) || entry.FullName.EndsWith("/") || ((entry.ExternalAttributes>>16)&0xF000)==0xA000)throw new InvalidDataException("Invalid program archive path/link");
                string relative=entry.FullName.Substring(8);Packages.Child(directory,relative);
                if(!Allowed(relative) || !names.Add(relative))throw new InvalidDataException("Program package contains an unlisted file: "+relative);
                total+=entry.Length;if(total>1024L*1024*1024)throw new InvalidDataException("Program package too large");
            }
            if(!names.Contains("program-manifest.json") || !names.Contains("bin/Macross30Recompiled.exe") || !names.Contains("tools/local_build/install_prebuilt.ps1"))throw new InvalidDataException("Incomplete EBOOT installer package");
            foreach(var entry in zip.Entries) {
                string relative=entry.FullName.Substring(8),target=Packages.Child(directory,relative);Packages.NoLinks(directory,relative);Directory.CreateDirectory(Path.GetDirectoryName(target));
                using(var input=entry.Open()) {
                    byte[] magic=new byte[4];int size=input.Read(magic,0,4);
                    if(size==4 && (magic[0]==0x7f && magic[1]==69 && magic[2]==76 && magic[3]==70 || magic[0]==83 && magic[1]==67 && magic[2]==69 && magic[3]==0))throw new InvalidDataException("Full EBOOT/ELF is not permitted in the program package");
                }
                string pending=target+".pending";Packages.NoLinks(directory,relative+".pending");entry.ExtractToFile(pending,true);
                if(File.Exists(target) && Packages.Hash(target)==Packages.Hash(pending)){File.Delete(pending);continue;}
                if(File.Exists(target))File.Replace(pending,target,null);else File.Move(pending,target);
            }
        }
        // The Python installer verifies this per-file manifest again before copying the runtime.
        var manifest=Packages.Read(File.ReadAllText(Path.Combine(directory,"program-manifest.json")));
        foreach(var value in Packages.Array(manifest["files"])) {
            var file=Packages.Object(value);string relative=Packages.Text(file,"path");if(!Allowed(relative) || Packages.Hash(Packages.Child(directory,relative))!=Packages.Text(file,"sha256"))throw new InvalidDataException("Program file integrity mismatch");
        }
        return directory;
    }
}
}
