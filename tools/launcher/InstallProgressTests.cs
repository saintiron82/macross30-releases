using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading.Tasks;
using System.Windows.Forms;
using MacrossLauncher;

class InstallProgressTests {
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void MainCore(string output) {
        string root=Path.Combine(output,"fixture-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
        try {
            string source=Path.Combine(root,"source");Directory.CreateDirectory(Path.Combine(source,"tools/local_build"));
            File.WriteAllText(Path.Combine(source,"tools/local_build/install.ps1"),"Write-Output 'LOCAL GENERATE: vendor/ps3recomp-master/tools/ppu_lifter.py'\nStart-Sleep -Milliseconds 600\nWrite-Output 'fixture-final-error-evidence'\nexit 17\n");
            using(var owner=new LauncherWindow(Path.Combine(root,"settings.json")))using(var timer=new System.Windows.Forms.Timer()) {
                owner.StartPosition=FormStartPosition.Manual;owner.Location=new Point(-3000,-3000);owner.Show();
                bool closeBlocked=false,sawFailure=false,sawPhase=false;int ticks=0;
                timer.Interval=100;timer.Tick+=(sender,args)=>{
                    ticks++;
                    var dialog=Application.OpenForms.OfType<InstallProgress>().FirstOrDefault();if(dialog==null)return;
                    var flags=BindingFlags.NonPublic|BindingFlags.Instance;
                    var close=(Button)typeof(InstallProgress).GetField("close",flags).GetValue(dialog);
                    var stage=(Label)typeof(InstallProgress).GetField("stage",flags).GetValue(dialog);
                    var tail=(TextBox)typeof(InstallProgress).GetField("tail",flags).GetValue(dialog);
                    if(stage.Text.Contains("3/7") && !sawPhase){sawPhase=true;using(var image=new Bitmap(dialog.Width,dialog.Height)){dialog.DrawToBitmap(image,new Rectangle(Point.Empty,dialog.Size));image.Save(Path.Combine(output,"phase.png"));}}
                    if(!close.Enabled && !closeBlocked){dialog.Close();closeBlocked=dialog.Visible;}
                    if(close.Enabled){
                        sawFailure=stage.Text.Contains("17") && tail.Text.Contains("fixture-final-error-evidence");
                        using(var image=new Bitmap(dialog.Width,dialog.Height)){dialog.DrawToBitmap(image,new Rectangle(Point.Empty,dialog.Size));image.Save(Path.Combine(output,"failure.png"));}
                        close.PerformClick();timer.Stop();
                    }
                    if(ticks>100)throw new Exception("Progress fixture timeout");
                };timer.Start();
                var method=typeof(LauncherWindow).GetMethod("RunLocalBuild",BindingFlags.NonPublic|BindingFlags.Instance);
                var task=(Task)method.Invoke(owner,new object[]{root,source,root,root});bool failed=false;
                try{task.GetAwaiter().GetResult();}catch(IOException){failed=true;}
                Check(failed && sawFailure && sawPhase && closeBlocked,"Failure/modal/phase/close safety");
                Check(File.ReadAllText(Path.Combine(root,"play/local-build.log")).Contains("fixture-final-error-evidence"),"Full log persisted");
                using(var success=new InstallProgress(Path.Combine(root,"success.log"))){
                    success.RunAsync(async ()=>{success.Receive("LOCAL_GENERATE: fixture");await Task.Delay(30);},owner).GetAwaiter().GetResult();
                    Check(!success.Visible,"Success closes progress dialog");
                }
                using(var build=new InstallProgress(Path.Combine(root,"build.log"))) {
                    var flags=BindingFlags.NonPublic|BindingFlags.Instance;
                    build.RunAsync(async ()=>{
                        build.Receive("[296/320] Building C object CMakeFiles/game.dir/runtime.c.obj");
                        var title=(Label)typeof(InstallProgress).GetField("stage",flags).GetValue(build);
                        var detail=(Label)typeof(InstallProgress).GetField("detailText",flags).GetValue(build);
                        var bar=(ProgressBar)typeof(InstallProgress).GetField("bar",flags).GetValue(build);
                        var log=(TextBox)typeof(InstallProgress).GetField("tail",flags).GetValue(build);
                        Check(!title.Text.Contains("Building") && detail.Text.Contains("296/320"),"Friendly compile counter");
                        Check(bar.Value>0 && bar.Value<100 && !log.Visible,"Determinate overall progress and hidden raw log");
                        await Task.Delay(300);
                        using(var image=new Bitmap(build.Width,build.Height)){build.DrawToBitmap(image,new Rectangle(Point.Empty,build.Size));image.Save(Path.Combine(output,"compile.png"));}
                        build.Receive("[320/320] Linking CXX executable game.exe");
                        Check(bar.Value<100,"Compile completion must not claim final success");
                        await Task.Delay(20);
                    },owner).GetAwaiter().GetResult();
                    Check(((ProgressBar)typeof(InstallProgress).GetField("bar",flags).GetValue(build)).Value==100,"Verified success reaches 100 percent");
                }
                foreach(var b in owner.Controls.Cast<Control>().SelectMany(Walk).OfType<Button>())b.Enabled=false;
                using(var image=new Bitmap(owner.Width,owner.Height)){owner.DrawToBitmap(image,new Rectangle(Point.Empty,owner.Size));image.Save(Path.Combine(output,"disabled.png"));}
                owner.Hide();
            }
            Console.WriteLine("PASS actual child-process failure, modal close guard, legacy/new generation markers, final log, success close; no game started");
        } finally {Directory.Delete(root,true);}
    }
    static System.Collections.Generic.IEnumerable<Control> Walk(Control control){yield return control;foreach(Control child in control.Controls)foreach(var descendant in Walk(child))yield return descendant;}
    [STAThread]static void Main(string[] args){Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);MainCore(args[0]);}
}
