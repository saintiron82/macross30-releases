using System;
using System.IO;
using System.Diagnostics;
using System.Collections.Generic;
using System.Web.Script.Serialization;
using MacrossLauncher;
public class BridgeProgram {
    static object Execute(Dictionary<string,object> r) {
        string op=Packages.Text(r,"op"),root=Path.GetFullPath(Packages.Text(r,"root"));
        string locale=r.ContainsKey("locale")?Packages.Text(r,"locale"):"ja-JP";
        if(Array.IndexOf(GameLanguage.Codes,locale)<0)throw new InvalidDataException("Unsupported play language");
        var manifest=r.ContainsKey("manifest")&&r["manifest"]!=null?Packages.Object(r["manifest"]):null;
        if(manifest!=null)PrebuiltInstall.Validate(manifest);
        if(op=="self-update"){SelfUpdate.Run(Packages.Text(r,"plan"));return new {updated=true};}
        if(op=="process"){using(var process=Process.GetProcessById(Convert.ToInt32(r["pid"])))return new {started=process.StartTime.ToUniversalTime().Ticks};}
        if(op=="inspect") {
            string cache=null;try{cache=PrebuiltInstall.InstalledProgram(root);}catch{}
            string installed="";try{installed=Packages.InstalledVersion(root);}catch{}
            return new {core_ready=PrebuiltInstall.CoreReady(root),language_ready=LocalBuild.Ready(root,locale),runtime_status=PrebuiltInstall.RuntimeStatus(root,manifest),language_status=PrebuiltInstall.LanguageStatus(root,locale,manifest),installed_version=installed,language_version=PrebuiltInstall.LanguageVersion(root,locale,manifest,true),program=cache};
        }
        if(op=="validate")return new {valid=true};
        if(op=="stopped"){Packages.EnsureStopped(root);return new {stopped=true};}
        if(op=="extract")return new {path=PrebuiltInstall.Extract(root,Packages.Text(r,"archive"),Packages.Object(r["artifact"]))};
        if(op=="extract-language") {
            string work=Packages.Child(root,".local-build/language-packages");Packages.NoLinks(root,".local-build");Directory.CreateDirectory(work);
            return new {path=LocalBuild.Extract(work,"data",Packages.Text(r,"archive"),Packages.Object(r["artifact"]))};
        }
        if(op=="stage") {PrebuiltInstall.CheckRuntime();PrebuiltInstall.StageRuntime(root,Packages.Text(r,"program"),Packages.Text(r,"version"));PrebuiltInstall.RecordProgram(root,Packages.Text(r,"program"),Packages.Object(r["artifact"]));return new {staged=true};}
        if(op=="record") {PrebuiltInstall.RecordProgram(root,Packages.Text(r,"program"),Packages.Object(r["artifact"]));return new {recorded=true};}
        if(op=="record-language") {PrebuiltInstall.RecordLanguageVersion(root,locale,Packages.Text(r,"version"));return new {recorded=true};}
        if(op=="select") {Packages.EnsureStopped(root);LocalBuild.SelectLanguage(root,locale,Packages.Text(r,"ui"));return new {selected=true};}
        if(op=="launch") {
            Packages.EnsureStopped(root);LocalBuild.SelectLanguage(root,locale,Packages.Text(r,"ui"));
            var start=new ProcessStartInfo(Environment.GetEnvironmentVariable("COMSPEC")??"cmd.exe","/d /c \"\""+Path.Combine(root,"Macross30.cmd")+"\"\""){WorkingDirectory=root,UseShellExecute=false,CreateNoWindow=true,RedirectStandardInput=true};
            using(var p=Process.Start(start)){p.StandardInput.Close();p.WaitForExit();return new {exit_code=p.ExitCode};}
        }
        throw new InvalidDataException("Unknown bridge operation");
    }
    [STAThread] public static void Main(string[] args) {
        Console.OutputEncoding=new System.Text.UTF8Encoding(false);
        try {if(args.Length!=1)throw new ArgumentException("Request file is required");var result=Execute(Packages.Read(File.ReadAllText(args[0])));Console.WriteLine(new JavaScriptSerializer().Serialize(new {ok=true,result=result}));}
        catch(Exception e){Console.WriteLine(new JavaScriptSerializer().Serialize(new {ok=false,error=e.Message}));Environment.ExitCode=1;}
    }
}
