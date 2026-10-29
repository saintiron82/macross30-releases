using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.RegularExpressions;

namespace MacrossLauncher {
public static class LocalBuild {
    public const string Mode="user-local-generate-and-compile";
    public const string OriginalHash="44d7112cbb5527314eb6192e59d861dacfcd4099f301a6e82ffc1681b0782b5b";
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
            if(Packages.Text(row,"file")!=expected+version+".zip" || Convert.ToInt64(row["bytes"])<=0 || Convert.ToInt64(row["bytes"])>512L*1024*1024 || !Regex.IsMatch(Packages.Text(row,"sha256"),"^[a-f0-9]{64}$"))
                throw new InvalidDataException("Invalid local-build artifact identity");
        }
    }
    public static string Extract(string root,string kind,string archive,Dictionary<string,object> row) {
        Packages.VerifyArchive(archive,row);
        string directory=Path.Combine(root,kind=="source" ? "source-"+Packages.Text(row,"sha256").Substring(0,16) : kind+"-"+Guid.NewGuid().ToString("N"));
        if(kind=="source" && Directory.Exists(directory)) {
            string origin=Path.Combine(directory,"local-source-origin.sha256");
            if(!File.Exists(origin) || File.ReadAllText(origin)!=Packages.Text(row,"sha256")) throw new InvalidDataException("Local source cache identity mismatch");
            var sourceManifest=Packages.Read(File.ReadAllText(Path.Combine(directory,"source-manifest.json")));
            foreach(var value in Packages.Array(sourceManifest["files"])) {
                var file=Packages.Object(value);if(Packages.Hash(Packages.Child(directory,Packages.Text(file,"path")))!=Packages.Text(file,"sha256")) throw new InvalidDataException("Local source cache changed");
            }
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
            if(kind=="source") File.WriteAllText(Path.Combine(directory,"local-source-origin.sha256"),Packages.Text(row,"sha256"));
            return directory;
        } catch { Directory.Delete(directory,true);throw; }
    }
    public static bool Ready(string root,string locale) {
        try {
            var profile=Packages.Read(File.ReadAllText(Path.Combine(root,"release-profile.json")));
            if(Packages.Text(profile,"mode")!=Mode || !Convert.ToBoolean(profile["generated_on_this_machine"]) || Convert.ToBoolean(profile["downloaded_game_executable"]) || Packages.Text(profile,"game_language")!=locale || Packages.Text(profile,"original_elf_sha256")!=OriginalHash) return false;
            return new[]{"Macross30.cmd","tools/launch-helper.ps1","bin/SDL2.dll","bin/msvcp140.dll","bin/vcruntime140.dll","bin/vcruntime140_1.dll"}.All(p=>File.Exists(Path.Combine(root,p))) &&
                File.ReadAllText(Path.Combine(root,".local-build/data-version.txt")).Trim()==File.ReadAllText(Path.Combine(root,"data/installed-version.txt")).Trim() &&
                Packages.Hash(Path.Combine(root,"bin/EBOOT.local.elf"))==Packages.Text(profile,"elf_sha256") &&
                Packages.Hash(Path.Combine(root,"bin/Macross30Recompiled.exe"))==Packages.Text(profile,"exe_sha256");
        } catch {return false;}
    }
}
}
