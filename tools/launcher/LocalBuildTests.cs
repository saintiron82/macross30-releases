using System;
using System.IO;
using System.IO.Compression;
using System.Collections.Generic;
using System.Web.Script.Serialization;
using MacrossLauncher;

class LocalBuildTests {
    static int count;
    static void Check(bool value) { if(!value) throw new Exception("Local-build assertion failed");count++; }
    static void Reject(Action action) { bool failed=false;try {action();}catch {failed=true;}Check(failed); }
    static string SourceFixture(string path,Dictionary<string,string> content,string origin) {
        Directory.CreateDirectory(path);var rows=new List<object>();
        foreach(var file in content) {
            string target=Packages.Child(path,file.Key);Directory.CreateDirectory(Path.GetDirectoryName(target));File.WriteAllText(target,file.Value);
            rows.Add(new {path=file.Key,bytes=new FileInfo(target).Length,sha256=Packages.Hash(target)});
        }
        File.WriteAllText(Path.Combine(path,"source-manifest.json"),new JavaScriptSerializer().Serialize(new {mode=LocalBuild.Mode,files=rows}));
        File.WriteAllText(Path.Combine(path,"local-source-origin.sha256"),origin);return path;
    }
    static void WorkspaceCases(string temporary) {
        string game=Path.Combine(temporary,"game");Directory.CreateDirectory(game);
        var first=new Dictionary<string,string>{{"native/macross30/runtime.cpp","old runtime"},{"native/macross30/shared.h","unchanged"},{"tools/build/obsolete.py","old tool"}};
        string staged1=SourceFixture(Path.Combine(temporary,"stage1"),first,new string('a',64));
        string workspace=LocalBuild.PrepareWorkspace(game,staged1);
        Check(workspace==Path.GetFullPath(Path.Combine(game,".local-build/workspace")));
        string header=Path.Combine(workspace,"native/macross30/shared.h");var oldTime=new DateTime(2001,1,2,3,4,5,DateTimeKind.Utc);File.SetLastWriteTimeUtc(header,oldTime);
        Directory.CreateDirectory(Path.Combine(workspace,"native/build"));File.WriteAllText(Path.Combine(workspace,"native/build/cache.obj"),"compiled object");
        Directory.CreateDirectory(Path.Combine(workspace,"native/macross30/recompiled"));File.WriteAllText(Path.Combine(workspace,"native/macross30/recompiled/ppu.cpp"),"local generated code");
        var second=new Dictionary<string,string>{{"native/macross30/runtime.cpp","new runtime"},{"native/macross30/shared.h","unchanged"},{"tools/build/new.py","new tool"}};
        string staged2=SourceFixture(Path.Combine(temporary,"stage2"),second,new string('b',64));
        Check(LocalBuild.PrepareWorkspace(game,staged2)==workspace);
        Check(File.GetLastWriteTimeUtc(header)==oldTime);
        Check(File.ReadAllText(Path.Combine(workspace,"native/macross30/runtime.cpp"))=="new runtime");
        Check(!File.Exists(Path.Combine(workspace,"tools/build/obsolete.py")) && File.Exists(Path.Combine(workspace,"tools/build/new.py")));
        Check(File.ReadAllText(Path.Combine(workspace,"native/build/cache.obj"))=="compiled object");
        Check(File.ReadAllText(Path.Combine(workspace,"native/macross30/recompiled/ppu.cpp"))=="local generated code");
        var legacy=Packages.Read(File.ReadAllText(Path.Combine(workspace,"legacy-source-manifest.json")));
        Check(Packages.Text(legacy,"verified_origin")==new string('a',64));
        File.AppendAllText(header,"tamper");Reject(()=>LocalBuild.PrepareWorkspace(game,staged1));
        Check(File.ReadAllText(Path.Combine(workspace,"native/macross30/runtime.cpp"))=="new runtime");
        File.WriteAllText(header,"unchanged");File.SetLastWriteTimeUtc(header,oldTime);
        string bad=SourceFixture(Path.Combine(temporary,"bad-stage"),new Dictionary<string,string>{{"tools/build/check.py","test"}},new string('c',64));
        var badManifest=Packages.Read(File.ReadAllText(Path.Combine(bad,"source-manifest.json")));
        badManifest["files"]=new[]{new {path="../escape.cpp",bytes=0,sha256=new string('0',64)}};
        File.WriteAllText(Path.Combine(bad,"source-manifest.json"),new JavaScriptSerializer().Serialize(badManifest));
        Reject(()=>LocalBuild.PrepareWorkspace(game,bad));
        Check(File.GetLastWriteTimeUtc(header)==oldTime);
        badManifest["files"]=new[]{new {path="native/build/cache.obj",bytes=0,sha256=new string('0',64)}};
        File.WriteAllText(Path.Combine(bad,"source-manifest.json"),new JavaScriptSerializer().Serialize(badManifest));
        Reject(()=>LocalBuild.PrepareWorkspace(game,bad));
        Check(File.ReadAllText(Path.Combine(workspace,"native/build/cache.obj"))=="compiled object");
        var failing=new Dictionary<string,string>(second);failing["native/macross30/runtime.cpp"]="must roll back";failing["tools/local_build/block/new.py"]="new code";
        string failingStage=SourceFixture(Path.Combine(temporary,"failing-stage"),failing,new string('d',64));
        Directory.CreateDirectory(Path.Combine(workspace,"tools/local_build"));File.WriteAllText(Path.Combine(workspace,"tools/local_build/block"),"untracked blocker");
        string manifestBefore=File.ReadAllText(Path.Combine(workspace,"source-manifest.json"));
        DateTime runtimeBefore=File.GetLastWriteTimeUtc(Path.Combine(workspace,"native/macross30/runtime.cpp"));
        Reject(()=>LocalBuild.PrepareWorkspace(game,failingStage));
        Check(File.ReadAllText(Path.Combine(workspace,"native/macross30/runtime.cpp"))=="new runtime");
        Check(File.GetLastWriteTimeUtc(Path.Combine(workspace,"native/macross30/runtime.cpp"))==runtimeBefore);
        Check(File.ReadAllText(Path.Combine(workspace,"source-manifest.json"))==manifestBefore);
        Check(Directory.GetDirectories(workspace,".source-update-*").Length==0);
        string link=Path.Combine(workspace,"native/macross30/linked"),linkTarget=Path.Combine(workspace,"junction-target");Directory.CreateDirectory(linkTarget);
        using(var command=System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe","/d /c mklink /J \""+link+"\" \""+linkTarget+"\""){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true})) {
            command.WaitForExit();
            if(command.ExitCode==0)try {
                var linked=new Dictionary<string,string>(second);linked["native/macross30/linked/new.cpp"]="must not escape through junction";
                string linkedStage=SourceFixture(Path.Combine(temporary,"linked-stage"),linked,new string('e',64));
                Reject(()=>LocalBuild.PrepareWorkspace(game,linkedStage));Check(!File.Exists(Path.Combine(linkTarget,"new.cpp")));
                Check(File.ReadAllText(Path.Combine(workspace,"native/macross30/runtime.cpp"))=="new runtime");
            }finally {Directory.Delete(link,false);}
        }
        string oldGame=Path.Combine(temporary,"old-game"),bundle=Path.Combine(oldGame,".local-build/bundle-legacy");Directory.CreateDirectory(bundle);
        string archive=Path.Combine(bundle,"Macross30-source-0.2.0.zip");
        using(var zip=ZipFile.Open(archive,ZipArchiveMode.Create)) {
            foreach(string name in first.Keys) zip.CreateEntryFromFile(Packages.Child(staged1,name),"source/"+name);
            zip.CreateEntryFromFile(Path.Combine(staged1,"source-manifest.json"),"source/source-manifest.json");
        }
        var row=new Dictionary<string,object>{{"file",Path.GetFileName(archive)},{"sha256",Packages.Hash(archive)},{"bytes",new FileInfo(archive).Length}};
        string oldSource=Path.GetFullPath(LocalBuild.Extract(bundle,"source",archive,row));
        Directory.CreateDirectory(Path.Combine(oldSource,"native/build"));File.WriteAllText(Path.Combine(oldSource,"native/build/CMakeCache.txt"),"absolute cached paths");
        File.WriteAllText(Path.Combine(oldSource,"local-generation.ready"),LocalBuild.OriginalHash);
        Check(LocalBuild.PrepareWorkspace(oldGame,staged2)==oldSource);
        Check(File.ReadAllText(Path.Combine(oldSource,"native/build/CMakeCache.txt"))=="absolute cached paths");
        legacy=Packages.Read(File.ReadAllText(Path.Combine(oldSource,"legacy-source-manifest.json")));Check(Packages.Text(legacy,"verified_origin")==Packages.Text(row,"sha256"));
        string oldStaging=LocalBuild.Extract(bundle,"source",archive,row);
        Check(oldStaging!=oldSource && File.ReadAllText(Path.Combine(oldStaging,"native/macross30/runtime.cpp"))=="old runtime");
        Check(File.ReadAllText(Path.Combine(oldSource,"native/macross30/runtime.cpp"))=="new runtime");
        string cached=LocalBuild.FindCachedArchive(oldGame,row);Check(cached==LocalBuild.ArchiveCachePath(oldGame,row) && LocalBuild.VerifyCachedArchive(cached,row));
        File.SetLastWriteTimeUtc(cached,oldTime);Check(LocalBuild.FindCachedArchive(oldGame,row)==cached && File.GetLastWriteTimeUtc(cached)==oldTime);
        File.WriteAllText(cached,"corrupt");Check(!LocalBuild.VerifyCachedArchive(cached,row));
        Check(LocalBuild.FindCachedArchive(oldGame,row)==cached && LocalBuild.VerifyCachedArchive(cached,row));
        File.WriteAllText(archive,"corrupt");File.WriteAllText(cached,"corrupt");Check(LocalBuild.FindCachedArchive(oldGame,row)==null);
        string outside=Path.Combine(temporary,"outside");Directory.CreateDirectory(outside);
        string pointer=Path.Combine(game,".local-build/workspace.json");File.WriteAllText(pointer,new JavaScriptSerializer().Serialize(new {relative_path="../outside"}));
        Reject(()=>LocalBuild.PrepareWorkspace(game,staged1));Check(File.GetLastWriteTimeUtc(header)==oldTime);
    }
    static void Main(string[] args) {
        string root=Path.Combine(Path.GetTempPath(),"macross-local-tests-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
        try {
            if(args.Length>0 && args[0]=="--workspace-only") {try{WorkspaceCases(root);}catch(Exception ex){Console.WriteLine("FAIL type="+ex.GetType().FullName+" message="+ex.Message+" checks="+count);Console.WriteLine(ex.StackTrace);Environment.ExitCode=1;return;}Console.WriteLine("PASS "+count+" workspace/cache cases; no game or user data touched");return;}
            var manifest=Packages.Read(File.ReadAllText(Path.Combine(args[0],"LOCAL-BUILD.json")));
            LocalBuild.Validate(manifest);count++;
            var files=Packages.Object(manifest["files"]);
            string source=null;
            foreach(string kind in new[]{"source","data","third_party"}) {
                var row=Packages.Object(files[kind]);string output=LocalBuild.Extract(root,kind,Path.Combine(args[0],Packages.Text(row,"file")),row);
                Check(Directory.Exists(output));if(kind=="source")source=output;
            }
            var sourceRow=Packages.Object(files["source"]);
            Check(source==LocalBuild.Extract(root,"source",Path.Combine(args[0],Packages.Text(sourceRow,"file")),sourceRow));
            File.AppendAllText(Path.Combine(source,"tools/local_build/prepare.py"),"\n# tampered");
            Reject(()=>LocalBuild.Extract(root,"source",Path.Combine(args[0],Packages.Text(sourceRow,"file")),sourceRow));
            manifest["source_only"]=false;Reject(()=>LocalBuild.Validate(manifest));manifest["source_only"]=true;
            manifest["mode"]="precompiled";Reject(()=>LocalBuild.Validate(manifest));
            int index=0;
            foreach(string entry in new[]{"source/bin/game.exe","source/bin/EBOOT.elf","source/native/macross30/recompiled/x.cpp","source/../escape.txt","source/native/macross30/raw_jobs/x.txt","source/native/macross30/spu_posttask_native/guard.c","source/spu_recomp.c","source/masked.txt","source/native/macross30/geom_c5b0_capture_contract.h"}) {
                string zip=Path.Combine(root,"bad"+(index++)+".zip");
                using(var archive=ZipFile.Open(zip,ZipArchiveMode.Create))using(var writer=new StreamWriter(archive.CreateEntry(entry).Open()))writer.Write("MZ-binary");
                var row=new Dictionary<string,object>{{"sha256",Packages.Hash(zip)},{"bytes",new FileInfo(zip).Length}};
                Reject(()=>LocalBuild.Extract(root,"source",zip,row));
            }
            Check(!LocalBuild.Ready(root,"ko-KR"));
            if(args.Length>1) {Check(LocalBuild.Ready(args[1],"ko-KR"));Check(!LocalBuild.Ready(args[1],"ja-JP"));}
            Console.WriteLine("PASS "+count+" local-build cases; actual source/data/runtime archives verified; no game started");
        } finally {Directory.Delete(root,true);}
    }
}
