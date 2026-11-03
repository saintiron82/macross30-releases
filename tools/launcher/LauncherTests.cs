using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Web.Script.Serialization;
using MacrossLauncher;

class LauncherTests {
    static JavaScriptSerializer json = new JavaScriptSerializer();
    static int tests;
    static IEnumerable<System.Windows.Forms.Control> Descendants(System.Windows.Forms.Control parent) { foreach(System.Windows.Forms.Control child in parent.Controls) { yield return child; foreach(var nested in Descendants(child))yield return nested; } }
    static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
    static void Reject(Action action, string message) {
        bool failed = false; try { action(); } catch { failed = true; }
        Check(failed, "Should reject: " + message); tests++;
    }
    static string Write(string root, string rel, string text) {
        string p = Path.Combine(root, rel); Directory.CreateDirectory(Path.GetDirectoryName(p)); File.WriteAllText(p, text, new UTF8Encoding(false)); return p;
    }
    static object Row(string root, string rel) { string p = Path.Combine(root, rel); return new { path = rel, size = new FileInfo(p).Length, sha256 = Packages.Hash(p) }; }
    static Dictionary<string, object> Fixture(string directory, string version, string extra) {
        Directory.CreateDirectory(directory); string tree = Path.Combine(directory, "tree");
        Write(tree, "Macross30.cmd", "new command"); Write(tree, "bin/EBOOT.ko.elf", "new ELF");
        Write(tree, "release-profile.json", json.Serialize(new { release_version = version }));
        Write(tree, "korean/install-plan.tsv", "# fixture plan\n");
        string id = Packages.Hash(Path.Combine(tree, "korean/install-plan.tsv")).Substring(0,16);
        Write(tree, "korean/data-version.txt", id + "\n");
        Write(tree, "korean/korean-pack.json", json.Serialize(new { release_version = version, data_version = id, elf_sha256 = Packages.Hash(Path.Combine(tree, "bin/EBOOT.ko.elf")), korean_files = new object[0] }));
        string[] program = { "Macross30.cmd", "bin/EBOOT.ko.elf", "release-profile.json" };
        Write(tree, "program-manifest.json", json.Serialize(new { release_version = version, data_version = id, files = program.Select(p => Row(tree,p)).ToArray() }));
        string[] names = Packages.Names("0.1.0");
        using (var zip = ZipFile.Open(Path.Combine(directory,names[0]),ZipArchiveMode.Create)) {
            foreach (string rel in program.Concat(new[] { "program-manifest.json" })) zip.CreateEntryFromFile(Path.Combine(tree,rel), "Macross30/" + rel);
            if (extra != null) { var e = zip.CreateEntry(extra); using(var w = new StreamWriter(e.Open())) w.Write("evil"); }
        }
        using(var zip = ZipFile.Open(Path.Combine(directory,names[1]),ZipArchiveMode.Create))
            foreach (string rel in new[] { "korean/korean-pack.json", "korean/install-plan.tsv", "korean/data-version.txt" }) zip.CreateEntryFromFile(Path.Combine(tree,rel), "Macross30/" + rel);
        var packages = new Dictionary<string,object>();
        foreach (string name in names) packages[name] = new { bytes = new FileInfo(Path.Combine(directory,name)).Length, sha256 = Packages.Hash(Path.Combine(directory,name)) };
        string manifest=json.Serialize(new { release_version = "0.1.0", packages = packages });
        Write(directory,"RELEASE-0.1.0.json",manifest);
        return Packages.Read(manifest);
    }
    [STAThread] static void Main(string[] args) {
        string temp = Path.Combine(Path.GetTempPath(),"macross-launcher-tests-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(temp);
        try {
            Check(Packages.VersionOf("0.10.0") > Packages.VersionOf("0.2.0"),"Numeric semver order"); tests++;
            string bundle=Path.Combine(temp,"bundle");Directory.CreateDirectory(bundle);
            Check(PrebuiltInstall.BundledPackage(bundle)==null,"Regular launcher has no bundled override");tests++;
            string bundleProgram=Write(bundle,"Macross30-program-0.3.0.zip","program fixture"),bundleData=Write(bundle,"Macross30-local-data-0.3.0.zip","data fixture");
            var bundleFiles=new Dictionary<string,object>();
            bundleFiles["program"]=new {file=Path.GetFileName(bundleProgram),bytes=new FileInfo(bundleProgram).Length,sha256=Packages.Hash(bundleProgram)};
            bundleFiles["data"]=new {file=Path.GetFileName(bundleData),bytes=new FileInfo(bundleData).Length,sha256=Packages.Hash(bundleData)};
            Write(bundle,"TEST-INSTALL.json",json.Serialize(new {mode=PrebuiltInstall.Mode,version="0.3.0",original_elf_sha256=LocalBuild.OriginalHash,files=bundleFiles}));
            Check(Packages.Text(PrebuiltInstall.BundledPackage(bundle),"version")=="0.3.0","Draft bundle automatically verifies both artifacts");tests++;
            File.AppendAllText(bundleData,"tampered");Reject(()=>PrebuiltInstall.BundledPackage(bundle),"tampered bundled artifact");
            string split=Path.Combine(temp,"split");Directory.CreateDirectory(split);
            string splitProgram=Write(split,"Macross30-program-0.3.1.zip","program"),splitJa=Write(split,"Macross30-language-ja-JP-0.3.1.zip","Japanese");
            var splitFiles=new Dictionary<string,object>{{"program",new {file=Path.GetFileName(splitProgram),bytes=new FileInfo(splitProgram).Length,sha256=Packages.Hash(splitProgram)}}};
            var splitLanguages=new Dictionary<string,object>{{"ja-JP",new {file=Path.GetFileName(splitJa),bytes=new FileInfo(splitJa).Length,sha256=Packages.Hash(splitJa)}},{"en-US",new {file="Macross30-language-en-US-0.3.1.zip",bytes=12,sha256=new string('a',64)}}};
            var splitManifest=Packages.Read(json.Serialize(new {mode=PrebuiltInstall.Mode,version="0.3.1",original_elf_sha256=LocalBuild.OriginalHash,files=splitFiles,language_packs=splitLanguages}));
            PrebuiltInstall.Validate(splitManifest);tests++;
            Check(Packages.Text(Packages.Object(PrebuiltInstall.Artifacts(splitManifest,"ja-JP")["data"]),"file")==Path.GetFileName(splitJa),"Only selected language archive planned");tests++;
            Write(split,"TEST-INSTALL.json",json.Serialize(splitManifest));
            Check(PrebuiltInstall.BundledPackage(split,"ja-JP")!=null,"Missing unselected English ZIP does not block Japanese");tests++;
            Reject(()=>PrebuiltInstall.BundledPackage(split,"en-US"),"selected missing language archive");
            var baseManifest=Packages.Read(json.Serialize(splitManifest));Packages.Object(baseManifest["language_packs"]).Remove("ja-JP");
            baseManifest["base_language"]=new {locale="ja-JP",data_id=new string('a',16),elf_sha256=LocalBuild.OriginalHash,plan_sha256=new string('a',64)};
            // Serializer round-trip makes the nested original-game row a Dictionary.
            baseManifest=Packages.Read(json.Serialize(baseManifest));PrebuiltInstall.Validate(baseManifest);
            Check(PrebuiltInstall.Artifacts(baseManifest,"ja-JP").Count==1,"Original Japanese requires only the common program artifact");tests++;
            File.Delete(splitJa);Write(split,"TEST-INSTALL.json",json.Serialize(baseManifest));
            Check(PrebuiltInstall.BundledPackage(split,"ja-JP")!=null,"Original Japanese has no separate language ZIP");tests++;
            string installed=Path.Combine(temp,"runtime-only"),programCache=Path.Combine(temp,"runtime-cache");
            foreach(string name in new[]{"Macross30.cmd","tools/launch-helper.ps1","bin/SDL2.dll","bin/msvcp140.dll","bin/vcruntime140.dll","bin/vcruntime140_1.dll"})Write(installed,name,"preserved");
            string priorExe=Write(installed,"bin/Macross30Recompiled.exe","old runtime"),priorElf=Write(installed,"bin/EBOOT.ja-JP.elf","local ELF");
            Write(installed,"data/languages/ja-JP/data/installed-version.txt","language-id");
            Write(installed,".local-build/game-language.txt","ja-JP");
            var installedViews=new Dictionary<string,object>{{"ja-JP",new {root="data/languages/ja-JP",data_id="language-id",plan_sha256="plan",elf="bin/EBOOT.ja-JP.elf",elf_sha256=Packages.Hash(priorElf),files=new object[0]}}};
            Write(installed,"release-profile.json",json.Serialize(new {mode=PrebuiltInstall.Mode,original_elf_sha256=LocalBuild.OriginalHash,original_generated_locally=true,exe_sha256=Packages.Hash(priorExe),language_views=installedViews,release_version="0.3.0"}));
            Check(LocalBuild.Ready(installed,"ja-JP"),"Runtime-only fixture is installed");tests++;
            var expectedLanguage=Packages.Read(json.Serialize(new {language_packs=new Dictionary<string,object>{{"ja-JP",new {data_id="language-id",plan_sha256="plan",elf_sha256=Packages.Hash(priorElf)}}}}));
            Check(PrebuiltInstall.LanguageCurrent(installed,"ja-JP",expectedLanguage),"Per-language identity survives global version updates");tests++;
            var unchangedElf=File.GetLastWriteTimeUtc(priorElf);var unchangedCommand=File.GetLastWriteTimeUtc(Path.Combine(installed,"Macross30.cmd"));
            string nextExe=Write(programCache,"bin/Macross30Recompiled.exe","new runtime");
            Write(programCache,"program-manifest.json",json.Serialize(new {files=new[]{new {path="bin/Macross30Recompiled.exe",bytes=new FileInfo(nextExe).Length,sha256=Packages.Hash(nextExe)}}}));
            PrebuiltInstall.UpdateRuntime(installed,programCache,"ja-JP","0.3.1");
            Check(LocalBuild.Ready(installed,"ja-JP") && Packages.InstalledVersion(installed)=="0.3.1","Runtime-only patch stays playable");tests++;
            Check(File.GetLastWriteTimeUtc(priorElf)==unchangedElf && File.GetLastWriteTimeUtc(Path.Combine(installed,"Macross30.cmd"))==unchangedCommand,"Runtime-only patch leaves ELF and language commands untouched");tests++;
            string stableExeHash=Packages.Hash(priorExe);File.WriteAllText(nextExe,"next runtime that must roll back");
            string nextDll=Write(programCache,"bin/SDL2.dll","next DLL");
            Write(programCache,"program-manifest.json",json.Serialize(new {files=new[]{new {path="bin/Macross30Recompiled.exe",bytes=new FileInfo(nextExe).Length,sha256=Packages.Hash(nextExe)},new {path="bin/SDL2.dll",bytes=new FileInfo(nextDll).Length,sha256=Packages.Hash(nextDll)}}}));
            using(var locked=new FileStream(Path.Combine(installed,"bin/SDL2.dll"),FileMode.Open,FileAccess.Read,FileShare.Read))Reject(()=>PrebuiltInstall.UpdateRuntime(installed,programCache,"ja-JP","0.3.2"),"runtime update locked library");
            Check(LocalBuild.Ready(installed,"ja-JP") && Packages.Hash(priorExe)==stableExeHash && Packages.InstalledVersion(installed)=="0.3.1","Failed runtime patch restores EXE and profile");tests++;
            string self = Path.Combine(temp,"self"); Directory.CreateDirectory(self);
            string target = Write(self,"Macross30Launcher.exe","old launcher"), pending = Write(self,"new.exe","new launcher"), backup = Path.Combine(self,"old.exe");
            string oldHash = Packages.Hash(target), newHash = Packages.Hash(pending);
            SelfUpdate.ReplaceAndStart(target,pending,backup,oldHash,newHash,() => Check(File.ReadAllText(target)=="new launcher","New launcher visible before start"));
            Check(File.ReadAllText(backup)=="old launcher","Self update backup"); tests++;
            File.Delete(backup); Write(self,"new.exe","failed launcher");
            Reject(() => SelfUpdate.ReplaceAndStart(target,pending,backup,newHash,Packages.Hash(pending),() => { throw new IOException("Simulated startup failure"); }),"self startup rollback");
            Check(File.ReadAllText(target)=="new launcher","Self rollback restored prior version"); tests++;
            Write(self,"new.exe","tampered");
            Reject(() => SelfUpdate.ReplaceAndStart(target,pending,backup,newHash,oldHash,() => {}),"self tamper");
            var selfManifest = Packages.Read(json.Serialize(new { version="0.1.1", file="Macross30Launcher-0.1.1.exe", bytes=40000, sha256=newHash }));
            SelfUpdate.ValidateManifest(selfManifest); tests++;
            selfManifest["file"]="../other.exe"; Reject(() => SelfUpdate.ValidateManifest(selfManifest),"self manifest path");
            var selfPlan = Packages.Read(json.Serialize(new { target=target }));
            string planPath = Path.Combine(self,".launcher-update-" + new string('a',32),"plan.json"); Directory.CreateDirectory(Path.GetDirectoryName(planPath));
            SelfUpdate.ValidatePlan(planPath,selfPlan);tests++;
            Reject(() => SelfUpdate.ValidatePlan(Path.Combine(self,"plan.json"),selfPlan),"self plan escape");
            string originalLanguage=UiText.Language;
            foreach(string code in new[]{"ko","en","ja"}) {
                UiText.Language=code;
                Check(!String.IsNullOrWhiteSpace(UiText.Guide) && UiText.Guide.Contains("ISO") && UiText.Guide.Contains(UiText.T("적용")),"Embedded guide " + code); tests++;
                foreach(string key in new[]{"설치 · 업데이트","게임 실행","적용","설치 폴더","안내"}) {
                    Check(!String.IsNullOrWhiteSpace(UiText.T(key)) && (code=="ko" || UiText.T(key)!=key),"UI translation " + code + key); tests++;
                }
            }
            Check(UiText.Normalize("unsupported")=="ko" && UiText.Normalize("en")=="en" && UiText.Normalize("ja")=="ja","Language fallback");tests++;
            UiText.Language=originalLanguage;
            string settingsPath=Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"launcher-settings.json");
            byte[] savedSettings=File.Exists(settingsPath) ? File.ReadAllBytes(settingsPath) : null;
            try {
                using(var window=new LauncherWindow(settingsPath)) {
                    var flags=System.Reflection.BindingFlags.NonPublic|System.Reflection.BindingFlags.Instance;
                    var combo=(System.Windows.Forms.ComboBox)typeof(LauncherWindow).GetField("language",flags).GetValue(window);
                    combo.SelectedIndex=1;Check(UiText.Language=="en" && combo.Text=="English","Live English selection");tests++;
                    combo.SelectedIndex=2;Check(UiText.Language=="ja" && combo.Text=="日本語","Live Japanese selection");tests++;
                    Check(!Descendants(window).Any(c=>new[]{"고급 메뉴","토큰 (선택)","로컬 패치 적용","GitHub 저장소","파일 목록 새로고침","선택 버전 다운로드"}.Contains(c.Tag as string)),"Manual patch and authentication controls removed recursively");tests++;
                    Check(Packages.Text(Packages.Read(File.ReadAllText(settingsPath)),"ui_language")=="ja","Language saved immediately");tests++;
                    typeof(LauncherWindow).GetField("gameLocale",flags).SetValue(window,"ja-JP");
                    typeof(LauncherWindow).GetMethod("SaveSettings",flags).Invoke(window,new object[0]);
                    Check(Packages.Text(Packages.Read(File.ReadAllText(settingsPath)),"game_language")=="ja-JP","Game language stored independently");tests++;
                    var cleanSettings=Packages.Read(File.ReadAllText(settingsPath));Check(!cleanSettings.ContainsKey("token") && !cleanSettings.ContainsKey("repository"),"No token or repository persisted");tests++;
                }
                UiText.Language="ko";
                using(var window=new LauncherWindow(settingsPath)) { Check(UiText.Language=="ja","Saved UI language restored");tests++;
                    Check((string)typeof(LauncherWindow).GetField("gameLocale",System.Reflection.BindingFlags.NonPublic|System.Reflection.BindingFlags.Instance).GetValue(window)=="ja-JP","Saved game language restored");tests++;
                }
            } finally {
                if(savedSettings==null) File.Delete(settingsPath); else File.WriteAllBytes(settingsPath,savedSettings);
                UiText.Language=originalLanguage;
            }
            Check(LauncherWindow.NeedsPackageUpdate("설치 안 됨","0.1.0"),"Wizard first install");tests++;
            Check(LauncherWindow.NeedsPackageUpdate("0.1.0","0.1.1"),"Wizard update installed lower");tests++;
            Check(!LauncherWindow.NeedsPackageUpdate("0.1.1","0.1.0"),"Wizard avoids downgrade");tests++;
            Check(!LauncherWindow.NeedsPackageUpdate("0.1.1","0.1.1"),"Wizard avoids repeat download");tests++;
            Check(LauncherWindow.NeedsPackageUpdate("0.2.0","0.10.0"),"Wizard numeric version compare");tests++;
            var older = new ReleaseFiles { Version = "0.2.0" };
            Check(older.CanDownload("0.10.0") && older.ComparedTo("0.10.0") == "이전 버전", "Older download"); tests++;
            Check(!older.CanDownload("0.2.0") && older.ComparedTo("0.2.0") == "현재 설치 버전", "Same version blocked"); tests++;
            Check(!older.CanDownload("0.1.0") && older.ComparedTo("0.1.0") == "새 버전", "Newer version blocked"); tests++;
            Check(older.CanDownload("설치 안 됨") && older.CanDownload("이전 배포본"), "First installation allowed"); tests++;
            Func<string,bool,bool,bool,object> releaseFixture = (version,draft,prerelease,complete) => new {
                tag_name = "v" + version, draft = draft, prerelease = prerelease,
                assets = Packages.Names(version).Concat(complete ? new[] { "RELEASE-" + version + ".json" } : new string[0]).Select((name,index) => new { name = name, size = 100, url = "https://api.github.com/repos/owner/repo/releases/assets/" + index }).ToArray()
            };
            var catalog = ReleaseFiles.Parse("owner/repo", json.Serialize(new[] { releaseFixture("0.2.0",false,false,true), releaseFixture("0.10.0",false,false,true), releaseFixture("0.1.0",true,false,true), releaseFixture("0.3.0",false,true,true), releaseFixture("0.4.0",false,false,false) }));
            Check(catalog.Count == 2 && catalog[0].Version == "0.10.0" && catalog[1].Version == "0.2.0", "Catalog sorts numerically and excludes draft/prerelease/incomplete"); tests++;
            Reject(() => ReleaseFiles.Parse("owner/repo", json.Serialize(new[] { releaseFixture("0.2.0",false,false,true), releaseFixture("0.2.0",false,false,true) })), "Duplicate catalog versions");
            Reject(() => ReleaseFiles.Parse("other/repo", json.Serialize(new[] { releaseFixture("0.2.0",false,false,true) })), "Foreign asset URL");
            foreach(string version in new[] { "01.0.0", "0.1", "../0.1.0", "0.1.0-beta" }) Reject(() => Packages.VersionOf(version),"version");
            foreach(string rel in new[] { "../play/save", "bin/../../escape", "C:/escape", "bin/x:stream", "bin/CON.txt", "bin/foo.", "bin\\..\\x" }) Reject(() => Packages.Child(temp,rel),"path");
            string directory = Path.Combine(temp,"good"), root = Path.Combine(temp,"installed");
            var release = Fixture(directory,"0.1.0",null);
            Check(Packages.ReleaseVersion(Packages.FindLocalRelease(directory))=="0.1.0","Local folder automatically finds release metadata");tests++;
            string empty=Path.Combine(temp,"empty");Directory.CreateDirectory(empty);Reject(() => Packages.FindLocalRelease(empty),"Local incomplete folder");
            Write(root,"play/settings.ini","keep settings"); Write(root,"play/gamedata/save","keep save");
            Write(root,"data/original","keep data"); Write(root,"firmware/original","keep firmware");
            Packages.Apply(root,directory,release,s => {});
            Check(Packages.InstalledVersion(root)=="0.1.0" && !Packages.DataReady(root),"Installed version and ISO required");
            foreach(string rel in new[] { "play/settings.ini", "play/gamedata/save", "data/original", "firmware/original" }) Check(File.ReadAllText(Path.Combine(root,rel)).StartsWith("keep"),"Protected " + rel);
            tests++;
            using(var locked = new FileStream(Path.Combine(root,"play/runner.lock"),FileMode.Open,FileAccess.ReadWrite,FileShare.None)) Reject(() => Packages.Apply(root,directory,release,s=>{}),"game lease");
            Write(root,"Macross30.cmd","old command"); Write(root,"bin/EBOOT.ko.elf","old ELF");
            using(var locked = new FileStream(Path.Combine(root,"bin/EBOOT.ko.elf"),FileMode.Open,FileAccess.Read,FileShare.Read)) Reject(() => Packages.Apply(root,directory,release,s=>{}),"locked output");
            Check(File.ReadAllText(Path.Combine(root,"Macross30.cmd"))=="old command" && File.ReadAllText(Path.Combine(root,"bin/EBOOT.ko.elf"))=="old ELF","Rollback preserves original files"); tests++;
            File.AppendAllText(Path.Combine(directory,Packages.Names("0.1.0")[0]),"tamper");
            Reject(() => Packages.Apply(root,directory,release,s=>{}),"bad archive hash");
            var mismatch = Fixture(Path.Combine(temp,"mismatch"),"0.2.0",null);
            Reject(() => Packages.Apply(root,Path.Combine(temp,"mismatch"),mismatch,s=>{}),"mismatched pack version");
            foreach(string extra in new[] { "Macross30/play/save", "Macross30/../outside", "Macross30/bin/extra", "Macross30/BIN/eboot.ko.elf" }) {
                string dir = Path.Combine(temp,Guid.NewGuid().ToString("N")); var bad = Fixture(dir,"0.1.0",extra);
                Reject(() => Packages.Apply(root,dir,bad,s=>{}),"untrusted zip " + extra);
            }
            if(args.Length==1) {
                string real = Path.Combine(temp,"real");
                var actual=Packages.Read(File.ReadAllText(Path.Combine(args[0],"RELEASE-0.1.0.json")));
                Packages.Apply(real,args[0],actual,Console.WriteLine);
                Check(Packages.InstalledVersion(real)=="0.1.0" && File.Exists(Path.Combine(real,"bin/Macross30Recompiled.exe")),"Actual two-pack application");tests++;
                Check(Packages.ProgramReady(real),"Actual installed program ready");tests++;
                var languageCatalog=Packages.Read(File.ReadAllText("C:/GameDev/Macross-language-0.1.0/LANGUAGES.json"));
                var japanese=GameLanguage.Select(languageCatalog,"ja-JP","0.1.0");tests++;
                Reject(() => GameLanguage.Select(languageCatalog,"en-US","0.1.0"),"English unavailable without fallback");
                Reject(() => GameLanguage.Select(languageCatalog,"ja-JP","0.2.0"),"Wrong language catalog version");
                var addon=Packages.Object(japanese["asset"]);
                GameLanguage.ApplyAddon(real,"ja-JP","C:/GameDev/Macross-language-0.1.0/"+Packages.Text(addon,"file"),addon);
                GameLanguage.Prepare(real,"ja-JP");
                string japaneseFolder=GameLanguage.Folder(real,"ja-JP");
                var japaneseMeta=Packages.Read(File.ReadAllText(Path.Combine(japaneseFolder,"language.json")));
                Check(Packages.Hash(Path.Combine(japaneseFolder,"EBOOT.elf"))==Packages.Text(japaneseMeta,"elf_sha256"),"Japanese ELF exact reconstruction");tests++;
                string wrapper=File.ReadAllText(Path.Combine(japaneseFolder,"Macross30.cmd"));
                Check(wrapper.Contains("MACROSS_BATTLE_SUBTITLES=0") && !wrapper.Contains("MACROSS_BATTLE_SUBTITLES=1") && !wrapper.Contains("bin\\subtitles"),"Japanese excludes Korean caption paths");tests++;
                Directory.CreateDirectory(Path.Combine(real,"data"));
                File.WriteAllText(Path.Combine(real,"data/installed-version.txt"),Packages.Text(japaneseMeta,"data_id"));
                Check(GameLanguage.Ready(real,"ja-JP") && !GameLanguage.Ready(real,"ko-KR"),"Language installation identities isolated");tests++;
                File.Delete(Path.Combine(real,"Macross30.cmd"));Check(!Packages.ProgramReady(real),"Missing launcher requires repair");tests++;
            }
            Console.WriteLine("PASS " + tests + " launcher cases; no game started");
        } catch(Exception ex) { Console.Error.WriteLine(ex.GetType().FullName + ": " + ex.Message); throw; } finally { Directory.Delete(temp,true); }
    }
}
