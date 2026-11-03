using System;using System.IO;using System.Collections.Generic;using System.Web.Script.Serialization;using MacrossLauncher;
class PackageStatusTests {
static Dictionary<string,object> D(params object[] p){var d=new Dictionary<string,object>();for(int i=0;i<p.Length;i+=2)d[(string)p[i]]=p[i+1];return d;}
static void Check(int expected,int got,string label){if(expected!=got)throw new Exception(label+": expected "+expected+" got "+got);}
[STAThread] static void Main(){try{Run();}catch(Exception e){Console.WriteLine(e.GetType().Name+": "+e.Message);Environment.ExitCode=1;}}
static void Run(){string root=Path.Combine(Path.GetTempPath(),"macross-status-test-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);try{
string stage=Path.Combine(root,"stage"),source=Path.Combine(root,"stage-source");Directory.CreateDirectory(stage);Directory.CreateDirectory(Path.Combine(source,"bin"));string stagedExe=Path.Combine(source,"bin/Macross30Recompiled.exe");File.WriteAllText(stagedExe,"runtime-before-ISO");
File.WriteAllText(Path.Combine(source,"program-manifest.json"),new JavaScriptSerializer().Serialize(D("files",new object[]{D("path","bin/Macross30Recompiled.exe","bytes",new FileInfo(stagedExe).Length,"sha256",Packages.Hash(stagedExe))})));
PrebuiltInstall.StageRuntime(stage,source,"0.3.1");if(Packages.Hash(Path.Combine(stage,"bin/Macross30Recompiled.exe"))!=Packages.Hash(stagedExe)||PrebuiltInstall.CoreReady(stage)||Directory.Exists(Path.Combine(stage,".local-build/original")))throw new Exception("Common patch staging must work before ISO without claiming original readiness");
Check(0,PrebuiltInstall.LanguageStatus(root,"ko-KR",null),"absent locale");Check(0,PrebuiltInstall.RuntimeStatus(root,null),"absent runtime");
Directory.CreateDirectory(Path.Combine(root,"bin"));File.WriteAllText(Path.Combine(root,"bin/Macross30Recompiled.exe"),"fixture-runtime");
Directory.CreateDirectory(Path.Combine(root,"ko/data"));File.WriteAllText(Path.Combine(root,"ko/data/installed-version.txt"),"ko-v1");File.WriteAllText(Path.Combine(root,"ko/game.elf"),"fixture-elf");
string elfHash=Packages.Hash(Path.Combine(root,"ko/game.elf")),exeHash=Packages.Hash(Path.Combine(root,"bin/Macross30Recompiled.exe"));
var file=new FileInfo(Path.Combine(root,"ko/data/installed-version.txt"));
var view=D("root","ko","elf","ko/game.elf","data_id","ko-v1","elf_sha256",elfHash,"plan_sha256","plan1","files",new object[]{D("path","data/installed-version.txt","size",file.Length,"mtime_ns",(file.LastWriteTimeUtc.Ticks-621355968000000000L)*100L)});
var profile=D("release_version","0.3.0","exe_sha256",exeHash,"language_views",D("ko-KR",view));
profile["mode"]=PrebuiltInstall.Mode;profile["original_elf_sha256"]=LocalBuild.OriginalHash;profile["original_generated_locally"]=true;
Packages.Object(profile["language_views"])["ja-JP"]=view;
foreach(string name in new[]{"Macross30.cmd","tools/launch-helper.ps1","bin/SDL2.dll","bin/msvcp140.dll","bin/vcruntime140.dll","bin/vcruntime140_1.dll"}){string target=Path.Combine(root,name);Directory.CreateDirectory(Path.GetDirectoryName(target));File.WriteAllText(target,"fixture");}
string cache=Path.Combine(root,".local-build/program-fixture");Directory.CreateDirectory(Path.Combine(cache,"tools/local_build"));
string installer=Path.Combine(cache,"tools/local_build/install_language.ps1");File.WriteAllText(installer,"fixture-language-installer");
File.WriteAllText(Path.Combine(cache,"program-manifest.json"),new JavaScriptSerializer().Serialize(D("files",new object[]{D("path","tools/local_build/install_language.ps1","bytes",new FileInfo(installer).Length,"sha256",Packages.Hash(installer))})));
profile["program_cache"]=".local-build/program-fixture";profile["program_manifest_sha256"]=Packages.Hash(Path.Combine(cache,"program-manifest.json"));profile["program_archive_sha256"]=new string('a',64);
Action save=()=>File.WriteAllText(Path.Combine(root,"release-profile.json"),new JavaScriptSerializer().Serialize(profile));save();
var expected=D("data_id","ko-v1","elf_sha256",elfHash,"plan_sha256","plan1");var program=D("exe_sha256",exeHash,"sha256",new string('a',64));
var latest=D("version","0.3.0","language_packs",D("ko-KR",expected),"files",D("program",program));
expected["file"]="Macross30-language-ko-KR-0.3.1.zip";
if(PrebuiltInstall.LanguageVersion(root,"ko-KR",latest,true)!="0.3.1" || PrebuiltInstall.LanguageVersion(root,"ko-KR",latest,false)!="0.3.1")throw new Exception("Legacy matching language version recovery");
view["version"]="0.2.0";save();if(PrebuiltInstall.LanguageVersion(root,"ko-KR",latest,true)!="0.2.0"||PrebuiltInstall.LanguageVersion(root,"ko-KR",latest,false)!="0.3.1")throw new Exception("Installed and latest language versions must remain independent");
view.Remove("version");save();
Check(2,PrebuiltInstall.LanguageStatus(root,"ko-KR",latest),"current locale");Check(0,PrebuiltInstall.LanguageStatus(root,"en-US",latest),"independent missing other locale");
Check(2,PrebuiltInstall.RuntimeStatus(root,latest),"current runtime");Check(1,PrebuiltInstall.LanguageStatus(root,"ko-KR",null),"unknown remote locale");Check(1,PrebuiltInstall.RuntimeStatus(root,null),"unknown remote runtime");
expected["data_id"]="ko-v2";Check(1,PrebuiltInstall.LanguageStatus(root,"ko-KR",latest),"outdated locale");Check(2,PrebuiltInstall.RuntimeStatus(root,latest),"locale update leaves runtime current");expected["data_id"]="ko-v1";
program["sha256"]=new string('b',64);Check(1,PrebuiltInstall.RuntimeStatus(root,latest),"outdated runtime archive");Check(2,PrebuiltInstall.LanguageStatus(root,"ko-KR",latest),"runtime update leaves locale current");program["sha256"]=new string('a',64);latest["version"]="9.0.0";Check(2,PrebuiltInstall.RuntimeStatus(root,latest),"archive identity ignores unrelated version");
if(!PrebuiltInstall.CoreReady(root) || PrebuiltInstall.InstalledProgram(root)!=Path.GetFullPath(cache))throw new Exception("Verified core and installed cache");
profile.Remove("program_cache");save();Check(1,PrebuiltInstall.RuntimeStatus(root,latest),"legacy missing cache is yellow");if(PrebuiltInstall.CoreReady(root))throw new Exception("Legacy core without cache");profile["program_cache"]=".local-build/program-fixture";save();
Packages.Object(profile["language_views"]).Remove("ko-KR");save();Check(2,PrebuiltInstall.RuntimeStatus(root,latest),"missing KO leaves core current");if(!PrebuiltInstall.CoreReady(root))throw new Exception("Core independent of KO");Packages.Object(profile["language_views"])["ko-KR"]=view;save();
File.WriteAllText(installer,"tampered cache installer");Check(1,PrebuiltInstall.RuntimeStatus(root,latest),"tampered cached file");if(PrebuiltInstall.CoreReady(root))throw new Exception("Tampered cache core");File.WriteAllText(installer,"fixture-language-installer");
latest["version"]="0.3.0";
using(var window=new LauncherWindow(Path.Combine(root,"settings-fixture.json"))) {
var flags=System.Reflection.BindingFlags.NonPublic|System.Reflection.BindingFlags.Instance;
var folder=(System.Windows.Forms.TextBox)typeof(LauncherWindow).GetField("folder",flags).GetValue(window);folder.Text=root;
typeof(LauncherWindow).GetField("latestManifest",flags).SetValue(window,latest);
var menu=(System.Windows.Forms.ContextMenuStrip)typeof(LauncherWindow).GetField("settingsMenu",flags).GetValue(window);
var handle=window.Handle;int isoChoices=0;typeof(LauncherWindow).GetField("chooseIsoOperation",flags).SetValue(window,(Action)(()=>isoChoices++));
for(int attempt=0;attempt<3;attempt++){menu.Show(new System.Drawing.Point(0,0));menu.Items[0].PerformClick();menu.Close();System.Windows.Forms.Application.DoEvents();if(menu.IsDisposed)throw new Exception("Settings menu disposed during ISO selection");}if(isoChoices!=3)throw new Exception("Repeated ISO selection did not execute");
var refresh=typeof(LauncherWindow).GetMethod("RefreshState",flags);refresh.Invoke(window,null);
var launcherUpdate=(System.Windows.Forms.Button)typeof(LauncherWindow).GetField("launcherUpdateButton",flags).GetValue(window);
foreach(string remote in new[]{(string)null,SelfUpdate.Version,"99.0.0"}){typeof(LauncherWindow).GetField("latestLauncherVersion",flags).SetValue(window,remote);refresh.Invoke(window,null);if(!launcherUpdate.Enabled)throw new Exception("Launcher update must remain available for retry/recheck");}
var selection=(System.Windows.Forms.ComboBox)typeof(LauncherWindow).GetField("playLanguage",flags).GetValue(window);
selection.SelectedIndex=0;
int prompts=0;program["bytes"]=1024L;program["sha256"]=new string('b',64);
typeof(LauncherWindow).GetField("catalogChecked",flags).SetValue(window,DateTime.UtcNow);
typeof(LauncherWindow).GetField("confirmPatchDownload",flags).SetValue(window,(Func<string,bool>)(message=>{prompts++;return false;}));
string before=File.ReadAllText(Path.Combine(root,"release-profile.json"));
((System.Threading.Tasks.Task)typeof(LauncherWindow).GetMethod("UpdateGame",flags).Invoke(window,null)).GetAwaiter().GetResult();
if(prompts!=1||before!=File.ReadAllText(Path.Combine(root,"release-profile.json")))throw new Exception("Declined update must ask once and preserve installed files");
program["sha256"]=new string('a',64);
((System.Threading.Tasks.Task)typeof(LauncherWindow).GetMethod("UpdateGame",flags).Invoke(window,null)).GetAwaiter().GetResult();if(prompts!=1)throw new Exception("Current patch must not request download approval");
selection.SelectedIndex=1;if(prompts!=1 || (string)typeof(LauncherWindow).GetField("gameLocale",flags).GetValue(window)!="en-US")throw new Exception("Dropdown selection must only save chosen language");selection.SelectedIndex=0;
var play=(System.Windows.Forms.Button)typeof(LauncherWindow).GetField("playButton",flags).GetValue(window);
typeof(LauncherWindow).GetField("working",flags).SetValue(window,true);refresh.Invoke(window,null);
var logs=(System.Windows.Forms.Button)typeof(LauncherWindow).GetField("logsButton",flags).GetValue(window);if(play.Enabled||!logs.Enabled||selection.Enabled)throw new Exception("Work must preserve logs and prevent concurrent play/language changes");
typeof(LauncherWindow).GetField("working",flags).SetValue(window,false);refresh.Invoke(window,null);
var sequence=new List<string>();typeof(LauncherWindow).GetField("playGameOperation",flags).SetValue(window,(Func<System.Threading.Tasks.Task>)(()=>{sequence.Add("play");return System.Threading.Tasks.Task.FromResult(0);}));
var click=typeof(System.Windows.Forms.Button).GetMethod("OnClick",System.Reflection.BindingFlags.NonPublic|System.Reflection.BindingFlags.Instance);
click.Invoke(play,new object[]{EventArgs.Empty});if(sequence.Count!=1||sequence[0]!="play")throw new Exception("Current installation must play on first click");
sequence.Clear();program["sha256"]=new string('b',64);
typeof(LauncherWindow).GetField("prepareGameOperation",flags).SetValue(window,(Func<System.Threading.Tasks.Task>)(()=>{sequence.Add("update");program["sha256"]=new string('a',64);return System.Threading.Tasks.Task.FromResult(0);}));
click.Invoke(play,new object[]{EventArgs.Empty});if(String.Join(",",sequence)!="update,play")throw new Exception("One Play click must update then launch without second click");
sequence.Clear();typeof(LauncherWindow).GetField("prepareGameOperation",flags).SetValue(window,(Func<System.Threading.Tasks.Task>)(()=>{typeof(LauncherWindow).GetField("updateDeclined",flags).SetValue(window,true);return System.Threading.Tasks.Task.FromResult(0);}));
click.Invoke(play,new object[]{EventArgs.Empty});if(sequence.Count!=0)throw new Exception("Declined update must not continue into setup or Play");
profile.Remove("program_cache");save();refresh.Invoke(window,null);if(!selection.Enabled||!play.Enabled)throw new Exception("First launch must permit language selection and Play/ISO setup");
var closing=new System.Windows.Forms.FormClosingEventArgs(System.Windows.Forms.CloseReason.UserClosing,false);
typeof(LauncherWindow).GetMethod("OnFormClosing",flags).Invoke(window,new object[]{closing});if(closing.Cancel)throw new Exception("Disabled language controls prevented normal closing");
profile["program_cache"]=".local-build/program-fixture";save();
}
File.WriteAllText(Path.Combine(root,"ko/game.elf"),"changed-elf-longer");Check(1,PrebuiltInstall.LanguageStatus(root,"ko-KR",latest),"hash cache invalidates on stat change");File.Delete(Path.Combine(root,"ko/game.elf"));Check(0,PrebuiltInstall.LanguageStatus(root,"ko-KR",latest),"missing ELF");
Console.WriteLine("PASS: independent core/language states, verified installed cache, legacy/tamper handling language dropdown, single-click update then play, approval rejection, reusable ISO menu");
}finally{Directory.Delete(root,true);}}}
