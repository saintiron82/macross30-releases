using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace MacrossLauncher {
public static class LocalBuild {
    public const string Mode="user-local-generate-and-compile";
    public const string OriginalHash="44d7112cbb5527314eb6192e59d861dacfcd4099f301a6e82ffc1681b0782b5b";
    public static bool VerifyCachedArchive(string path,Dictionary<string,object> row) {
        try {
            if(!File.Exists(path) || (File.GetAttributes(path)&FileAttributes.ReparsePoint)!=0) return false;
            Packages.VerifyArchive(path,row);return true;
        } catch(InvalidDataException) {return false;} catch(IOException) {return false;} catch(UnauthorizedAccessException) {return false;}
    }
    public static string ArchiveCachePath(string root,Dictionary<string,object> row) {
        string sha=Packages.Text(row,"sha256");if(!Regex.IsMatch(sha,"^[a-f0-9]{64}$"))throw new InvalidDataException("Invalid archive hash");
        Directory.CreateDirectory(root);Packages.NoLinks(root,".local-build/downloads");
        string directory=Packages.Child(root,".local-build/downloads");Directory.CreateDirectory(directory);
        string path=Packages.Child(directory,sha+".zip");Packages.NoLinks(directory,sha+".zip");return path;
    }
    public static string FindCachedArchive(string root,Dictionary<string,object> row) {
        string target=ArchiveCachePath(root,row);if(VerifyCachedArchive(target,row))return target;
        string state=Packages.Child(root,".local-build");
        foreach(string directory in Directory.GetDirectories(state).OrderBy(p=>p,StringComparer.OrdinalIgnoreCase)) {
            string name=Path.GetFileName(directory);if(!name.StartsWith("bundle-",StringComparison.Ordinal) && !name.StartsWith("work-",StringComparison.Ordinal))continue;
            Packages.NoLinks(state,name);
            foreach(string candidate in Directory.GetFiles(directory,"*.zip")) {
                Packages.NoLinks(directory,Path.GetFileName(candidate));
                if(new FileInfo(candidate).Length!=Convert.ToInt64(row["bytes"]) || !VerifyCachedArchive(candidate,row))continue;
                string pending=target+".pending-"+Guid.NewGuid().ToString("N");
                try {File.Copy(candidate,pending);Packages.VerifyArchive(pending,row);if(File.Exists(target))File.Replace(pending,target,null);else File.Move(pending,target);}
                finally {if(File.Exists(pending))File.Delete(pending);}
                return target;
            }
        }
        return null;
    }
    static bool SourcePath(string relative) {
        relative=relative.ToLowerInvariant();
        string extension=Path.GetExtension(relative).ToLowerInvariant();
        return (relative.StartsWith("native/macross30/",StringComparison.Ordinal) || relative.StartsWith("vendor/ps3recomp-master/",StringComparison.Ordinal) ||
                relative.StartsWith("tools/build/",StringComparison.Ordinal) || relative.StartsWith("tools/local_build/",StringComparison.Ordinal) || relative=="tools/play/inspect_iso.py") &&
            !new[]{".exe",".dll",".elf",".self",".bin",".obj",".o",".lib",".a",".prx",".sprx"}.Contains(extension) &&
            !new[]{"/recompiled/","/raw_jobs/","/spu_gameplay/","/spu_graphics/","/spu_posttask_native/","/spu/","/build/../"}.Any(s=>relative.Contains(s)) &&
            !relative.EndsWith("/geom_c5b0_capture_contract.h",StringComparison.OrdinalIgnoreCase) && !Path.GetFileName(relative).StartsWith("spu_recomp",StringComparison.OrdinalIgnoreCase);
    }
    static Dictionary<string,string> SourceFiles(string root,out Dictionary<string,object> manifest) {
        Packages.NoLinks(root,"source-manifest.json");manifest=Packages.Read(File.ReadAllText(Path.Combine(root,"source-manifest.json")));
        if(Packages.Text(manifest,"mode")!=Mode)throw new InvalidDataException("Source manifest mode mismatch");
        var files=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
        foreach(var value in Packages.Array(manifest["files"])) {
            var row=Packages.Object(value);string relative=Packages.Text(row,"path"),sha=Packages.Text(row,"sha256");
            string path=Packages.Child(root,relative);Packages.NoLinks(root,relative);
            if(!SourcePath(relative) || !Regex.IsMatch(sha,"^[a-f0-9]{64}$") || files.ContainsKey(relative))throw new InvalidDataException("Invalid source manifest path: "+relative);
            if(!File.Exists(path) || Packages.Hash(path)!=sha)throw new InvalidDataException("Local source cache changed: "+relative);
            files.Add(relative,sha);if(files.Count>5000)throw new InvalidDataException("Too many source files");
        }
        if(files.Count==0)throw new InvalidDataException("Empty source manifest");return files;
    }
    static void AtomicWrite(string path,byte[] bytes) {
        Directory.CreateDirectory(Path.GetDirectoryName(path));string pending=path+".pending-"+Guid.NewGuid().ToString("N");
        try {File.WriteAllBytes(pending,bytes);if(File.Exists(path))File.Replace(pending,path,null);else File.Move(pending,path);}
        finally {if(File.Exists(pending))File.Delete(pending);}
    }
    // Caller holds play/runner.lock. Generated code and Ninja's absolute paths stay in place.
    public static string PrepareWorkspace(string root,string stagedSource) {
        root=Path.GetFullPath(root);stagedSource=Path.GetFullPath(stagedSource);
        Packages.NoLinks(root,".local-build/workspace.json");
        Dictionary<string,object> incomingManifest;var incoming=SourceFiles(stagedSource,out incomingManifest);
        Packages.NoLinks(stagedSource,"local-source-origin.sha256");string incomingOrigin=File.ReadAllText(Path.Combine(stagedSource,"local-source-origin.sha256"));
        if(!Regex.IsMatch(incomingOrigin,"^[a-f0-9]{64}$"))throw new InvalidDataException("Staged source identity missing");
        string state=Packages.Child(root,".local-build");Directory.CreateDirectory(state);string pointer=Packages.Child(state,"workspace.json"),workspace=null;
        if(File.Exists(pointer)) {
            var saved=Packages.Read(File.ReadAllText(pointer));workspace=Packages.Child(root,Packages.Text(saved,"relative_path"));
            if(!workspace.StartsWith(state+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("Workspace outside local build folder");
        } else {
            var candidates=new List<string>();
            foreach(string bundle in Directory.GetDirectories(state,"bundle-*")) {
                Packages.NoLinks(state,Path.GetFileName(bundle));
                foreach(string source in Directory.GetDirectories(bundle,"source-*")) {
                    Packages.NoLinks(bundle,Path.GetFileName(source));
                    if(File.Exists(Path.Combine(source,"source-manifest.json")) && (File.Exists(Path.Combine(source,"local-generation.ready")) || File.Exists(Path.Combine(source,"native/build/CMakeCache.txt"))))candidates.Add(source);
                }
            }
            workspace=candidates.OrderByDescending(p=>Directory.GetLastWriteTimeUtc(p)).FirstOrDefault() ?? Packages.Child(state,"workspace");
        }
        string relativeWorkspace=workspace.Substring(root.TrimEnd('\\','/').Length+1).Replace('\\','/');Packages.NoLinks(root,relativeWorkspace);
        Directory.CreateDirectory(workspace);
        Dictionary<string,object> previousManifest=null;var previous=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
        if(File.Exists(Path.Combine(workspace,"source-manifest.json")))previous=SourceFiles(workspace,out previousManifest);
        else if(File.Exists(pointer))throw new InvalidDataException("Workspace manifest missing");
        string oldOrigin=null;
        if(previousManifest!=null) {Packages.NoLinks(workspace,"local-source-origin.sha256");oldOrigin=File.ReadAllText(Path.Combine(workspace,"local-source-origin.sha256"));}
        foreach(string name in incoming.Keys)if(!previous.ContainsKey(name) && File.Exists(Packages.Child(workspace,name)))throw new InvalidDataException("Untracked source conflicts with update: "+name);
        foreach(string metadata in new[]{"source-manifest.json","local-source-origin.sha256","legacy-source-manifest.json",".workspace-managed"})Packages.NoLinks(workspace,metadata);
        var writes=new Dictionary<string,byte[]>(StringComparer.OrdinalIgnoreCase);
        foreach(var file in incoming)if(!previous.ContainsKey(file.Key) || previous[file.Key]!=file.Value)writes[file.Key]=File.ReadAllBytes(Packages.Child(stagedSource,file.Key));
        writes["source-manifest.json"]=File.ReadAllBytes(Path.Combine(stagedSource,"source-manifest.json"));
        writes["local-source-origin.sha256"]=System.Text.Encoding.ASCII.GetBytes(incomingOrigin);
        writes[".workspace-managed"]=System.Text.Encoding.ASCII.GetBytes("source-workspace-v1\n");
        if(previousManifest!=null && !File.Exists(Path.Combine(workspace,"legacy-source-manifest.json"))) {
            var legacy=new Dictionary<string,object>{{"verified_origin",oldOrigin},{"files",previousManifest["files"]}};
            writes["legacy-source-manifest.json"]=System.Text.Encoding.UTF8.GetBytes(new JavaScriptSerializer().Serialize(legacy));
        }
        var removals=previous.Keys.Where(p=>!incoming.ContainsKey(p)).ToList();
        string backup=Packages.Child(workspace,".source-update-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(backup);
        var touched=new List<string>();var timestamps=new Dictionary<string,DateTime>();
        try {
            foreach(string name in removals.Concat(writes.Keys).Distinct(StringComparer.OrdinalIgnoreCase)) {
                string target=Packages.Child(workspace,name);Packages.NoLinks(workspace,name);
                if(File.Exists(target)) {
                    if(writes.ContainsKey(name) && File.ReadAllBytes(target).SequenceEqual(writes[name]))continue;
                    string saved=Packages.Child(backup,name);Directory.CreateDirectory(Path.GetDirectoryName(saved));File.Copy(target,saved);timestamps[name]=File.GetLastWriteTimeUtc(target);
                }
                touched.Add(name);
                if(writes.ContainsKey(name))AtomicWrite(target,writes[name]);else if(File.Exists(target))File.Delete(target);
            }
            AtomicWrite(pointer,System.Text.Encoding.UTF8.GetBytes(new JavaScriptSerializer().Serialize(new {relative_path=relativeWorkspace})));
        } catch {
            foreach(string name in touched.AsEnumerable().Reverse()) {
                string target=Packages.Child(workspace,name),saved=Packages.Child(backup,name);
                if(File.Exists(saved)){AtomicWrite(target,File.ReadAllBytes(saved));File.SetLastWriteTimeUtc(target,timestamps[name]);}
                else if(File.Exists(target))File.Delete(target);
            }
            throw;
        } finally {
            // The only recursively removed tree is this verified, newly created transaction directory.
            if(!backup.StartsWith(workspace.TrimEnd('\\')+"\\.source-update-",StringComparison.OrdinalIgnoreCase))throw new IOException("Invalid transaction cleanup path");
            Directory.Delete(backup,true);
        }
        return workspace;
    }
    public static void Validate(Dictionary<string,object> manifest) {
        string version=Packages.Text(manifest,"version");Packages.VersionOf(version);
        if(Packages.Text(manifest,"mode")!=Mode || !Convert.ToBoolean(manifest["source_only"]) || Packages.Text(manifest,"original_elf_sha256")!=OriginalHash)
            throw new InvalidDataException(UiText.T("사용자 원본 기반 로컬 빌드 패키지만 지원합니다."));
        var files=Packages.Object(manifest["files"]);
        string[] kinds={"source","data","third_party"};
        if(files.Count!=3 || kinds.Any(k=>!files.ContainsKey(k))) throw new InvalidDataException("Invalid local-build artifact list");
        foreach(string kind in kinds) {
            var row=Packages.Object(files[kind]);
            string expected=kind=="source" ? "Macross30-source-" : kind=="data" ? "Macross30-local-data-" : "Macross30-third-party-";
            if(!Regex.IsMatch(Packages.Text(row,"file"),"^"+Regex.Escape(expected)+@"[0-9]+\.[0-9]+\.[0-9]+\.zip$") || Convert.ToInt64(row["bytes"])<=0 || Convert.ToInt64(row["bytes"])>512L*1024*1024 || !Regex.IsMatch(Packages.Text(row,"sha256"),"^[a-f0-9]{64}$"))
                throw new InvalidDataException("Invalid local-build artifact identity");
        }
    }
    public static string Extract(string root,string kind,string archive,Dictionary<string,object> row) {
        Packages.VerifyArchive(archive,row);
        string directory=Path.Combine(root,kind=="source" ? "source-"+Packages.Text(row,"sha256").Substring(0,16) : kind+"-"+Guid.NewGuid().ToString("N"));
        Packages.NoLinks(root,Path.GetFileName(directory));
        if(kind=="source" && Directory.Exists(directory) && File.Exists(Path.Combine(directory,".workspace-managed")) &&
            (!File.Exists(Path.Combine(directory,"local-source-origin.sha256")) || File.ReadAllText(Path.Combine(directory,"local-source-origin.sha256"))!=Packages.Text(row,"sha256")))
            directory += "-staging-"+Guid.NewGuid().ToString("N");
        if(kind=="source" && Directory.Exists(directory)) {
            string origin=Path.Combine(directory,"local-source-origin.sha256");
            if(!File.Exists(origin) || File.ReadAllText(origin)!=Packages.Text(row,"sha256")) throw new InvalidDataException("Local source cache identity mismatch");
            Dictionary<string,object> checkedManifest;SourceFiles(directory,out checkedManifest);
            return directory;
        }
        Directory.CreateDirectory(directory);
        string prefix=kind=="source" ? "source/" : kind=="data" ? "payload/" : "runtime/";
        try {
            long total=0;var names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            using(var zip=ZipFile.OpenRead(archive)) {
                if(zip.Entries.Count>5000) throw new InvalidDataException("Too many local-build files");
                foreach(var entry in zip.Entries) {
                    if(!entry.FullName.StartsWith(prefix,StringComparison.Ordinal) || entry.FullName.EndsWith("/")) throw new InvalidDataException("Invalid local-build archive prefix");
                    string relative=entry.FullName.Substring(prefix.Length);string file=Packages.Child(directory,relative);
                    string extension=Path.GetExtension(relative).ToLowerInvariant();
                    if(new[]{".exe",".elf",".self",".bin",".obj",".o",".a",".lib",".prx",".sprx"}.Contains(extension) || Path.GetFileName(relative).StartsWith("spu_recomp",StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException(UiText.T("완성된 게임 실행 파일이나 생성 코드가 패키지에 있어 설치를 중단했습니다."));
                    if(kind=="source" && ((relative.EndsWith("/geom_c5b0_capture_contract.h",StringComparison.OrdinalIgnoreCase)) || relative.Contains("/recompiled/") || relative.Contains("/raw_jobs/") || relative.Contains("/spu_posttask_native/") || relative.Contains("/spu_gameplay/"))) throw new InvalidDataException("Generated game sources are not accepted");
                    if(extension==".dll" && (kind!="third_party" || !relative.StartsWith("bin/ffmpeg/",StringComparison.Ordinal) || !new[]{"avutil-61.dll","avcodec-63.dll","avformat-63.dll","swresample-7.dll"}.Contains(Path.GetFileName(relative)))) throw new InvalidDataException("Unlisted runtime library");
                    if(!names.Add(relative) || ((entry.ExternalAttributes>>16)&0xF000)==0xA000) throw new InvalidDataException("Duplicate local-build file or link");
                    total+=entry.Length;if(total>1024L*1024*1024) throw new InvalidDataException("Local-build expansion too large");
                    Directory.CreateDirectory(Path.GetDirectoryName(file));entry.ExtractToFile(file);
                    if(kind=="source") using(var input=File.OpenRead(file)) {
                        byte[] magic=new byte[4];input.Read(magic,0,4);
                        if((magic[0]==0x7f && magic[1]==69 && magic[2]==76 && magic[3]==70) || (magic[0]==77 && magic[1]==90)) throw new InvalidDataException("Binary payload disguised as build source");
                    }
                }
            }
            if(kind=="source") {Dictionary<string,object> checkedManifest;SourceFiles(directory,out checkedManifest);File.WriteAllText(Path.Combine(directory,"local-source-origin.sha256"),Packages.Text(row,"sha256"));}
            return directory;
        } catch { Directory.Delete(directory,true);throw; }
    }
    public static void SelectLanguage(string root,string locale,string ui) {
        if(!Ready(root,locale))throw new InvalidDataException("Selected language data is not installed");
        string state=Path.Combine(Path.GetFullPath(root),".local-build");Packages.NoLinks(root,".local-build/game-language.txt");Packages.NoLinks(root,".local-build/ui-language.txt");
        File.WriteAllText(Path.Combine(state,"game-language.txt"),locale,new System.Text.UTF8Encoding(false));
        File.WriteAllText(Path.Combine(state,"ui-language.txt"),UiText.Normalize(ui),new System.Text.UTF8Encoding(false));
    }
    public static bool Ready(string root,string locale) {
        try {
            var profile=Packages.Read(File.ReadAllText(Path.Combine(root,"release-profile.json")));
            string mode=Packages.Text(profile,"mode");
            if(mode!=Mode && mode!=PrebuiltInstall.Mode || Packages.Text(profile,"original_elf_sha256")!=OriginalHash)return false;
            if(mode==Mode && (!Convert.ToBoolean(profile["generated_on_this_machine"]) || Convert.ToBoolean(profile["downloaded_game_executable"])))return false;
            if(mode==PrebuiltInstall.Mode && (!profile.ContainsKey("original_generated_locally") || !Convert.ToBoolean(profile["original_generated_locally"])))return false;
            if(profile.ContainsKey("language_views")) {
                var views=Packages.Object(profile["language_views"]);if(!views.ContainsKey(locale))return false;
                var view=Packages.Object(views[locale]);string dataRoot=Packages.Child(root,Packages.Text(view,"root"));
                if(view.ContainsKey("files"))foreach(var value in Packages.Array(view["files"])) {
                    var row=Packages.Object(value);var file=new FileInfo(Packages.Child(dataRoot,Packages.Text(row,"path")));
                    if(!file.Exists || file.Length!=Convert.ToInt64(row["size"]) || (file.LastWriteTimeUtc.Ticks-621355968000000000L)*100L!=Convert.ToInt64(row["mtime_ns"]))return false;
                }
                if(File.ReadAllText(Path.Combine(dataRoot,"data/installed-version.txt")).Trim()!=Packages.Text(view,"data_id") || Packages.Hash(Packages.Child(root,Packages.Text(view,"elf")))!=Packages.Text(view,"elf_sha256"))return false;
                return new[]{"Macross30.cmd","tools/launch-helper.ps1","bin/SDL2.dll","bin/msvcp140.dll","bin/vcruntime140.dll","bin/vcruntime140_1.dll"}.All(p=>File.Exists(Path.Combine(root,p))) && Packages.Hash(Path.Combine(root,"bin/Macross30Recompiled.exe"))==Packages.Text(profile,"exe_sha256");
            }
            if(Packages.Text(profile,"game_language")!=locale)return false;
            return new[]{"Macross30.cmd","tools/launch-helper.ps1","bin/SDL2.dll","bin/msvcp140.dll","bin/vcruntime140.dll","bin/vcruntime140_1.dll"}.All(p=>File.Exists(Path.Combine(root,p))) &&
                File.ReadAllText(Path.Combine(root,".local-build/data-version.txt")).Trim()==File.ReadAllText(Path.Combine(root,"data/installed-version.txt")).Trim() &&
                Packages.Hash(Path.Combine(root,"bin/EBOOT.local.elf"))==Packages.Text(profile,"elf_sha256") &&
                Packages.Hash(Path.Combine(root,"bin/Macross30Recompiled.exe"))==Packages.Text(profile,"exe_sha256");
        } catch {return false;}
    }
}
}
