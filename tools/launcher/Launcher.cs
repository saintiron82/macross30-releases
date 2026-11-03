// Windows/.NET Framework launcher; no Python or Git installation required.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace MacrossLauncher {
public static class Packages {
    public const string Repository = "saintiron82/macross30-releases";
    static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 4 * 1024 * 1024 };
    public static Dictionary<string, object> Read(string text) { return Json.Deserialize<Dictionary<string, object>>(text); }
    public static Dictionary<string, object> Object(object value) { return (Dictionary<string, object>)value; }
    public static IEnumerable<object> Array(object value) { return ((IEnumerable)value).Cast<object>(); }
    public static string Text(Dictionary<string, object> d, string key) { return Convert.ToString(d[key]); }
    public static string Hash(string path) {
        using (var sha = SHA256.Create()) using (var f = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "").ToLowerInvariant();
    }
    public static Version VersionOf(string value) {
        if (!Regex.IsMatch(value ?? "", @"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"))
            throw new InvalidDataException(UiText.T("버전은 0.1.0 형식이어야 합니다."));
        return new Version(value);
    }
    public static string ReleaseVersion(Dictionary<string, object> manifest) {
        string version = Text(manifest, "release_version"); VersionOf(version); return version;
    }
    public static string[] Names(string version) {
        VersionOf(version);
        return new[] { "Macross30-program-" + version + ".zip", "Macross30-korean-" + version + ".zip" };
    }
    public static void VerifyArchive(string path, Dictionary<string, object> row) {
        if (new FileInfo(path).Length != Convert.ToInt64(row["bytes"]) ||
            !String.Equals(Hash(path), Text(row, "sha256"), StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException(UiText.T("패키지 크기 또는 SHA-256이 다릅니다: ") + Path.GetFileName(path));
    }
    public static string Child(string root, string relative) {
        if (String.IsNullOrWhiteSpace(relative) || relative.IndexOfAny(new[] { ':', '\\', '\0' }) >= 0 ||
            relative.StartsWith("/") || relative.Split('/').Any(p => p == ".." || p == "." || p == "" ||
                p.EndsWith(".") || p.EndsWith(" ") || Regex.IsMatch(p, @"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)", RegexOptions.IgnoreCase)))
            throw new InvalidDataException(UiText.T("허용되지 않은 패키지 경로: ") + relative);
        string full = Path.GetFullPath(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
        if (!full.StartsWith(Path.GetFullPath(root).TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException(UiText.T("패키지 경로가 설치 폴더를 벗어납니다."));
        return full;
    }
    static void Owned(string relative) {
        string head = relative.Split('/')[0];
        if (relative.Contains("/")) {
            if (head != "bin" && head != "tools" && head != "korean")
                throw new InvalidDataException(UiText.T("패키지가 사용자 폴더를 변경하려고 합니다: ") + relative);
        } else if (!new[] { "Macross30.cmd", "Install.cmd", "VerifyData.cmd", "CollectLogs.cmd", "README-QA.txt",
                           "RELEASE-NOTES.txt", "THIRD-PARTY-NOTICES.txt", "program-manifest.json", "release-profile.json" }.Contains(relative))
            throw new InvalidDataException(UiText.T("허용되지 않은 최상위 파일: ") + relative);
    }
    static void Extract(string archive, string stage, HashSet<string> paths, ref long total) {
        using (var zip = ZipFile.OpenRead(archive)) {
            if (zip.Entries.Count > 4096) throw new InvalidDataException(UiText.T("패키지 파일 수 제한을 초과했습니다."));
            foreach (var entry in zip.Entries) {
                if (!entry.FullName.StartsWith("Macross30/", StringComparison.Ordinal))
                    throw new InvalidDataException(UiText.T("패키지 루트가 Macross30이 아닙니다."));
                string rel = entry.FullName.Substring(10);
                if (entry.FullName.EndsWith("/")) { Child(stage, rel.TrimEnd('/')); continue; }
                string destination = Child(stage, rel); Owned(rel);
                if (!paths.Add(rel) || ((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000)
                    throw new InvalidDataException(UiText.T("중복 파일 또는 링크가 포함됐습니다."));
                total += entry.Length;
                if (entry.Length > 1024L * 1024 * 1024 || total > 2L * 1024 * 1024 * 1024)
                    throw new InvalidDataException(UiText.T("패키지 압축 해제 용량 제한을 초과했습니다."));
                Directory.CreateDirectory(Path.GetDirectoryName(destination)); entry.ExtractToFile(destination);
            }
        }
    }
    static void CheckFile(string stage, Dictionary<string, object> row) {
        string path = Child(stage, Text(row, "path"));
        if (new FileInfo(path).Length != Convert.ToInt64(row["size"]) || Hash(path) != Text(row, "sha256"))
            throw new InvalidDataException(UiText.T("내부 파일 검증 실패: ") + Text(row, "path"));
    }
    static void CheckStage(string stage, string version, HashSet<string> paths) {
        var manifest = Read(File.ReadAllText(Path.Combine(stage, "program-manifest.json")));
        var profile = Read(File.ReadAllText(Path.Combine(stage, "release-profile.json")));
        var korean = Read(File.ReadAllText(Path.Combine(stage, "korean/korean-pack.json")));
        if (ReleaseVersion(manifest) != version || ReleaseVersion(profile) != version || ReleaseVersion(korean) != version)
            throw new InvalidDataException(UiText.T("프로그램팩과 한국어팩의 버전이 다릅니다."));
        var expected = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "program-manifest.json", "korean/korean-pack.json", "korean/data-version.txt", "korean/install-plan.tsv" };
        foreach (var value in Array(manifest["files"])) {
            var row = Object(value); CheckFile(stage, row); expected.Add(Text(row, "path"));
        }
        foreach (var value in Array(korean["korean_files"])) {
            var row = Object(value); string rel = Text(row, "delta");
            if (Hash(Child(stage, rel)) != Text(row, "delta_sha256")) throw new InvalidDataException(UiText.T("한국어 델타 검증 실패"));
            expected.Add(rel);
        }
        if (!expected.SetEquals(paths)) throw new InvalidDataException(UiText.T("패키지에 누락 또는 미등록 파일이 있습니다."));
        string data = File.ReadAllText(Path.Combine(stage, "korean/data-version.txt")).Trim();
        string plan = File.ReadAllText(Path.Combine(stage, "korean/install-plan.tsv")).Replace("\r\n", "\n");
        using (var sha = SHA256.Create()) {
            string id = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(plan))).Replace("-", "").ToLowerInvariant().Substring(0, 16);
            if (data != id || data != Text(manifest, "data_version") || data != Text(korean, "data_version"))
                throw new InvalidDataException(UiText.T("한국어 설치 계획의 콘텐츠 식별자가 다릅니다."));
        }
        if (Hash(Path.Combine(stage, "bin/EBOOT.ko.elf")) != Text(korean, "elf_sha256"))
            throw new InvalidDataException(UiText.T("실행 ELF와 한국어팩이 맞지 않습니다."));
    }
    public static bool DataReady(string root) {
        string wanted = Path.Combine(root, "korean/data-version.txt"), installed = Path.Combine(root, "data/installed-version.txt");
        return File.Exists(wanted) && File.Exists(installed) && File.ReadAllText(wanted).Trim() == File.ReadAllText(installed).Trim();
    }
    public static bool ProgramReady(string root) {
        return new[] { "Macross30.cmd", "bin/Macross30Recompiled.exe", "bin/EBOOT.ko.elf", "tools/install.ps1", "korean/korean-pack.json", "korean/data-version.txt" }
            .All(relative => File.Exists(Path.Combine(root,relative)));
    }
    public static Dictionary<string,object> FindLocalRelease(string directory) {
        var candidates=new List<Dictionary<string,object>>();
        foreach(string path in Directory.EnumerateFiles(directory,"RELEASE-*.json",SearchOption.TopDirectoryOnly)) {
            try {
                var manifest=Read(File.ReadAllText(path)); string version=ReleaseVersion(manifest);
                if(Names(version).All(name => File.Exists(Path.Combine(directory,name)))) candidates.Add(manifest);
            } catch (ArgumentException) { } catch (InvalidDataException) { } catch (KeyNotFoundException) { }
        }
        if(candidates.Count==0) throw new InvalidDataException(UiText.T("선택한 폴더에 설치 파일이 없습니다. 프로그램팩과 한국어팩을 받은 폴더를 선택하세요."));
        return candidates.OrderByDescending(m => VersionOf(ReleaseVersion(m))).First();
    }
    public static string InstalledVersion(string root) {
        string p = Path.Combine(root, "release-profile.json");
        if (!File.Exists(p)) return "설치 안 됨";
        var d = Read(File.ReadAllText(p)); return d.ContainsKey("release_version") ? ReleaseVersion(d) : "이전 배포본";
    }
    public static void NoLinks(string root, string relative) {
        string path = root;
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new IOException(UiText.T("설치 폴더가 링크입니다."));
        foreach (string part in relative.Split('/')) {
            path = Path.Combine(path, part);
            if ((File.Exists(path) || Directory.Exists(path)) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                throw new IOException(UiText.T("설치 경로에 링크가 있습니다: ") + path);
        }
    }
    public static void EnsureStopped(string root) {
        string exe = Path.GetFullPath(Path.Combine(root, "bin/Macross30Recompiled.exe"));
        foreach (var process in Process.GetProcessesByName("Macross30Recompiled")) using (process) {
            try {
                if (String.Equals(process.MainModule.FileName, exe, StringComparison.OrdinalIgnoreCase))
                    throw new IOException(UiText.T("이 설치 폴더의 게임을 먼저 종료하세요."));
            } catch (System.ComponentModel.Win32Exception) { throw new IOException(UiText.T("실행 중인 게임의 경로를 확인할 수 없습니다. 게임 종료 후 다시 시도하세요.")); }
              catch (InvalidOperationException) { /* process exited during inspection */ }
        }
    }
    public static void Apply(string root, string directory, Dictionary<string, object> release, Action<string> log) {
        root = Path.GetFullPath(root); string version = ReleaseVersion(release);
        EnsureStopped(root);
        var rows = Object(release["packages"]); string[] names = Names(version);
        foreach (string name in names) VerifyArchive(Path.Combine(directory, name), Object(rows[name]));
        Directory.CreateDirectory(root); NoLinks(root, "play/runner.lock"); Directory.CreateDirectory(Path.Combine(root, "play"));
        // The distribution's cmd launcher holds this same lock throughout the game.
        using (var lease = new FileStream(Path.Combine(root, "play/runner.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None)) {
            string work = Path.Combine(root, ".launcher-" + Guid.NewGuid().ToString("N"));
            string stage = Path.Combine(work, "stage"), backup = Path.Combine(work, "backup");
            Directory.CreateDirectory(stage);
            var changed = new List<string>(); var existed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            bool preserve = false;
            try {
                long total = 0; var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                foreach (string name in names) Extract(Path.Combine(directory, name), stage, paths, ref total);
                CheckStage(stage, version, paths);
                foreach (string rel in paths) NoLinks(root, rel);
                log(UiText.T("검증 완료. 프로그램팩과 한국어팩 ") + version + UiText.T(" 적용 중…"));
                foreach (string rel in paths.OrderBy(x => x == "release-profile.json" ? 1 : 0)) {
                    string destination = Child(root, rel), saved = Child(backup, rel);
                    Directory.CreateDirectory(Path.GetDirectoryName(destination));
                    string pending = destination + ".launcher-" + Guid.NewGuid().ToString("N");
                    try {
                        File.Copy(Child(stage, rel), pending);
                        if (File.Exists(destination)) {
                            Directory.CreateDirectory(Path.GetDirectoryName(saved));
                            File.Replace(pending, destination, saved); existed.Add(rel);
                        } else File.Move(pending, destination);
                        changed.Add(rel);
                    } finally { if (File.Exists(pending)) File.Delete(pending); }
                }
                log(UiText.T("두 팩 ") + version + UiText.T(" 갱신 완료. ") + (DataReady(root) ? UiText.T("게임을 실행할 수 있습니다.") : UiText.T("ISO로 게임 데이터를 설치/갱신하세요.")));
            } catch {
                try {
                    foreach (string rel in Enumerable.Reverse(changed)) {
                        if (existed.Contains(rel)) File.Copy(Child(backup, rel), Child(root, rel), true);
                        else File.Delete(Child(root, rel));
                    }
                } catch { preserve = true; log(UiText.T("자동 복구가 완료되지 않았습니다. 복구 파일 보존: ") + backup); throw; }
                throw;
            } finally { if (!preserve) Directory.Delete(work, true); }
        }
    }
}

public sealed class ReleaseFiles {
    public string Version;
    public Dictionary<string, string> Urls = new Dictionary<string, string>(StringComparer.Ordinal);
    public Dictionary<string, long> Sizes = new Dictionary<string, long>(StringComparer.Ordinal);
    public static List<ReleaseFiles> Parse(string source, string text) {
        var json = new JavaScriptSerializer { MaxJsonLength = 4 * 1024 * 1024 };
        var result = new List<ReleaseFiles>();
        foreach (var value in Packages.Array(json.DeserializeObject(text))) {
            var release = Packages.Object(value);
            if (Convert.ToBoolean(release["draft"]) || Convert.ToBoolean(release["prerelease"])) continue;
            string version = Packages.Text(release, "tag_name");
            if (version.StartsWith("v", StringComparison.Ordinal)) version = version.Substring(1);
            try { Packages.VersionOf(version); } catch { continue; }
            var entry = new ReleaseFiles { Version = version };
            foreach (var asset in Packages.Array(release["assets"])) {
                var row = Packages.Object(asset); string url = Packages.Text(row, "url");
                if (!url.StartsWith("https://api.github.com/repos/" + source + "/releases/assets/", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException(UiText.T("릴리스 자산 주소가 저장소와 다릅니다."));
                string name = Packages.Text(row, "name"); entry.Urls.Add(name, url); entry.Sizes.Add(name, Convert.ToInt64(row["size"]));
            }
            if (entry.Urls.ContainsKey("RELEASE-" + version + ".json") && Packages.Names(version).All(n => entry.Urls.ContainsKey(n))) result.Add(entry);
        }
        if (result.Select(r => r.Version).Distinct().Count() != result.Count) throw new InvalidDataException(UiText.T("같은 버전의 릴리스가 중복됐습니다."));
        return result.OrderByDescending(r => Packages.VersionOf(r.Version)).ToList();
    }
    public string ComparedTo(string installed) {
        if (installed == "설치 안 됨" || installed == "이전 배포본") return "설치 가능";
        int compare = Packages.VersionOf(Version).CompareTo(Packages.VersionOf(installed));
        return compare > 0 ? "새 버전" : compare < 0 ? "이전 버전" : "현재 설치 버전";
    }
    public bool CanDownload(string installed) {
        return installed == "설치 안 됨" || installed == "이전 배포본" || Packages.VersionOf(Version) < Packages.VersionOf(installed);
    }
}

class Downloads : WebClient {
    protected override WebRequest GetWebRequest(Uri address) {
        if (address.Scheme != "https") throw new InvalidDataException(UiText.T("HTTPS 주소만 사용합니다."));
        WebRequest request = base.GetWebRequest(address); request.Timeout = 30000;
        var http = request as HttpWebRequest; if (http != null) http.ReadWriteTimeout = 30000;
        return request;
    }
}

static class LauncherVisuals {
    public static readonly Color Surface=Color.FromArgb(24,33,48),Muted=Color.FromArgb(152,168,191),Text=Color.FromArgb(235,242,252);
    public static System.Drawing.Drawing2D.GraphicsPath Round(Rectangle r,int radius) {
        var path=new System.Drawing.Drawing2D.GraphicsPath();int d=Math.Min(radius*2,Math.Min(r.Width,r.Height));
        if(d<=0){path.AddRectangle(r);return path;}
        path.AddArc(r.Left,r.Top,d,d,180,90);path.AddArc(r.Right-d,r.Top,d,d,270,90);
        path.AddArc(r.Right-d,r.Bottom-d,d,d,0,90);path.AddArc(r.Left,r.Bottom-d,d,d,90,90);path.CloseFigure();return path;
    }
}
sealed class LauncherPanel : Panel {
    public LauncherPanel(){DoubleBuffered=true;BackColor=LauncherVisuals.Surface;Padding=new Padding(20,16,20,16);Margin=new Padding(0,0,0,12);}
    protected override void OnPaintBackground(PaintEventArgs e){e.Graphics.Clear(Parent==null?BackColor:Parent.BackColor);}
    protected override void OnPaint(PaintEventArgs e){e.Graphics.SmoothingMode=System.Drawing.Drawing2D.SmoothingMode.AntiAlias;using(var p=LauncherVisuals.Round(new Rectangle(0,0,Width-1,Height-1),14))using(var b=new SolidBrush(BackColor))using(var pen=new Pen(Color.FromArgb(43,56,76))){e.Graphics.FillPath(b,p);e.Graphics.DrawPath(pen,p);}}
}
sealed class LauncherAction : Button {
    public bool Primary;
    public LauncherAction(){SetStyle(ControlStyles.UserPaint|ControlStyles.OptimizedDoubleBuffer|ControlStyles.AllPaintingInWmPaint,true);Cursor=Cursors.Hand;}
    protected override void OnPaint(PaintEventArgs e){
        e.Graphics.SmoothingMode=System.Drawing.Drawing2D.SmoothingMode.AntiAlias;e.Graphics.Clear(Parent==null?LauncherVisuals.Surface:Parent.BackColor);
        Color fill=Primary?Color.FromArgb(49,121,235):BackColor;
        if(!Enabled)fill=Color.FromArgb(fill.R*3/4,fill.G*3/4,fill.B*3/4);
        using(var p=LauncherVisuals.Round(new Rectangle(1,1,Width-3,Height-3),9))using(var b=new SolidBrush(fill))using(var pen=new Pen(Primary?Color.FromArgb(93,160,255):Color.FromArgb(63,82,108),1)) {e.Graphics.FillPath(b,p);e.Graphics.DrawPath(pen,p);}
        TextRenderer.DrawText(e.Graphics,Text,Font,new Rectangle(10,0,Width-20,Height),Enabled?LauncherVisuals.Text:LauncherVisuals.Muted,TextFormatFlags.HorizontalCenter|TextFormatFlags.VerticalCenter|TextFormatFlags.EndEllipsis|TextFormatFlags.NoPrefix);
        if(Focused&&ShowFocusCues)ControlPaint.DrawFocusRectangle(e.Graphics,Rectangle.Inflate(ClientRectangle,-6,-6));
    }
}
public sealed class LauncherWindow : Form {
    TextBox folder = new TextBox(), output = new TextBox();
    Label state = new Label(), notice = new Label(), guide = new Label(),languageVersion=new Label();
    ProgressBar transferProgress=new ProgressBar();Label transferDetail=new Label();Button logsButton;
    List<Button> actions = new List<Button>();
    ComboBox language = new ComboBox(),playLanguage=new ComboBox();
    string gameLocale="ja-JP", isoPath="";
    string config;
    Dictionary<string,object> latestManifest;Dictionary<string,string> latestUrls;string latestLauncherVersion;
    DateTime catalogChecked=DateTime.MinValue;Button launcherUpdateButton,playButton;
    bool updatingSelf, loadingSettings,working,updateDeclined;
    readonly Dictionary<string,string> downloadedPacks=new Dictionary<string,string>();
    Func<Task> prepareGameOperation,playGameOperation;
    Func<string,bool> confirmPatchDownload;
    readonly ContextMenuStrip settingsMenu=new ContextMenuStrip();
    Action chooseIsoOperation;
    readonly string[] languageCodes = { "ko", "en", "ja" };
    public LauncherWindow(string settingsFile=null) {
        prepareGameOperation=UpdateGame;playGameOperation=Play;
        confirmPatchDownload=message=>MessageBox.Show(this,message,UiText.T("게임 업데이트"),MessageBoxButtons.YesNo,MessageBoxIcon.Question,MessageBoxDefaultButton.Button2)==DialogResult.Yes;
        config=settingsFile ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"Macross30Launcher","settings.json");
        Text = "Macross 30 Launcher " + SelfUpdate.Version; ClientSize = new Size(860, 630); MinimumSize = new Size(840, 660);
        StartPosition = FormStartPosition.CenterScreen; Font = new Font("맑은 고딕", 10); BackColor = Color.FromArgb(14, 21, 33); ForeColor = LauncherVisuals.Text;
        folder.Text = File.Exists(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Macross30.cmd")) ? AppDomain.CurrentDomain.BaseDirectory : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Macross30");
        loadingSettings = true;
        string loadSettings=config;
        if(settingsFile==null && !File.Exists(loadSettings))loadSettings=FindPreviousSettings();
        if (File.Exists(loadSettings)) try {
            var saved = Packages.Read(File.ReadAllText(loadSettings)); folder.Text = Packages.Text(saved, "folder");
            if(saved.ContainsKey("game_language")) gameLocale=GameLanguage.Normalize(Packages.Text(saved,"game_language"));
            if(saved.ContainsKey("iso")) isoPath=Packages.Text(saved,"iso");
            if (saved.ContainsKey("ui_language")) UiText.Language = UiText.Normalize(Packages.Text(saved,"ui_language"));
        } catch { }
        var layout=new TableLayoutPanel {Dock=DockStyle.Fill,Padding=new Padding(28),ColumnCount=1,RowCount=6};
        foreach(int h in new[]{100,95,96,90,72})layout.RowStyles.Add(new RowStyle(SizeType.Absolute,h));layout.RowStyles.Add(new RowStyle(SizeType.Percent,100));Controls.Add(layout);
        var header=new TableLayoutPanel {Dock=DockStyle.Fill,ColumnCount=2};header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));header.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,300));
        header.Controls.Add(new Label {Text="MACROSS 30",Dock=DockStyle.Fill,Font=new Font(Font.FontFamily,25,FontStyle.Bold),ForeColor=LauncherVisuals.Text,TextAlign=ContentAlignment.MiddleLeft});
        var uiRow=new FlowLayoutPanel {Dock=DockStyle.Fill,FlowDirection=FlowDirection.RightToLeft,Padding=new Padding(0,28,0,0)};
        language.DropDownStyle=ComboBoxStyle.DropDownList;language.Items.AddRange(new object[]{"한국어","English","日本語"});language.SelectedIndex=Array.IndexOf(languageCodes,UiText.Language);
        language.Width=132;language.Margin=new Padding(8,6,0,0);uiRow.Controls.Add(language);var uiCaption=Caption("Language",9,false);uiCaption.Tag=null;uiCaption.Dock=DockStyle.None;uiCaption.Size=new Size(110,32);uiCaption.TextAlign=ContentAlignment.MiddleRight;uiRow.Controls.Add(uiCaption);header.Controls.Add(uiRow);layout.Controls.Add(header);
        var statusCard=new LauncherPanel {Dock=DockStyle.Fill};state.Dock=DockStyle.Fill;state.TextAlign=ContentAlignment.MiddleLeft;state.ForeColor=LauncherVisuals.Text;statusCard.Controls.Add(state);layout.Controls.Add(statusCard);
        var localeCard=new LauncherPanel {Dock=DockStyle.Fill};var localeRow=new FlowLayoutPanel {Dock=DockStyle.Fill,WrapContents=false};localeRow.Controls.Add(new Label {Tag="게임 언어",Text=UiText.T("게임 언어"),ForeColor=LauncherVisuals.Muted,Size=new Size(112,44),TextAlign=ContentAlignment.MiddleLeft});
        playLanguage.DropDownStyle=ComboBoxStyle.DropDownList;playLanguage.Items.AddRange(new object[]{"한국어","English","日本語"});playLanguage.Size=new Size(260,36);playLanguage.Margin=new Padding(0,9,0,0);playLanguage.SelectedIndex=Array.IndexOf(GameLanguage.Codes,gameLocale);playLanguage.SelectedIndexChanged+=(sender,e)=>{gameLocale=GameLanguage.Codes[playLanguage.SelectedIndex];updateDeclined=false;SaveSettings();RefreshState();};localeRow.Controls.Add(playLanguage);languageVersion.Size=new Size(310,48);languageVersion.Margin=new Padding(18,2,0,0);languageVersion.ForeColor=LauncherVisuals.Muted;languageVersion.TextAlign=ContentAlignment.MiddleLeft;localeRow.Controls.Add(languageVersion);localeCard.Controls.Add(localeRow);layout.Controls.Add(localeCard);
        var progressCard=new LauncherPanel {Dock=DockStyle.Fill};var progressLayout=new TableLayoutPanel {Dock=DockStyle.Fill,ColumnCount=1,RowCount=2};progressLayout.RowStyles.Add(new RowStyle(SizeType.Absolute,30));progressLayout.RowStyles.Add(new RowStyle(SizeType.Percent,100));transferDetail.Dock=DockStyle.Fill;transferDetail.ForeColor=LauncherVisuals.Text;transferDetail.Text=UiText.T("대기 중");transferProgress.Dock=DockStyle.Fill;transferProgress.Height=16;progressLayout.Controls.Add(transferDetail);progressLayout.Controls.Add(transferProgress);progressCard.Controls.Add(progressLayout);layout.Controls.Add(progressCard);
        playButton=Button("게임 실행",async()=>await Busy(MainAction));playButton.AutoSize=false;playButton.MinimumSize=Size.Empty;playButton.Dock=DockStyle.Fill;playButton.Font=new Font(Font.FontFamily,14,FontStyle.Bold);((LauncherAction)playButton).Primary=true;layout.Controls.Add(playButton);
        var footer=new FlowLayoutPanel {Dock=DockStyle.Fill,WrapContents=true};
        logsButton=Button("로그 열기",()=>{string path=Path.Combine(folder.Text,"play");var viewer=new Form {Text=UiText.T("로그 열기"),Size=new Size(850,560),StartPosition=FormStartPosition.CenterParent};var text=new TextBox {Multiline=true,ReadOnly=true,ScrollBars=ScrollBars.Both,Dock=DockStyle.Fill,Text=output.Text};viewer.Controls.Add(text);viewer.Show(this);});footer.Controls.Add(logsButton);
        chooseIsoOperation=()=>{using(var dialog=new OpenFileDialog {Filter=UiText.T("원본 디스크 이미지 (*.iso)|*.iso")})if(dialog.ShowDialog(this)==DialogResult.OK){isoPath=dialog.FileName;SaveSettings();RefreshState();}};
        settingsMenu.Items.Add(UiText.T("ISO 변경"),null,(sender,e)=>BeginInvoke(new Action(()=>chooseIsoOperation())));
        settingsMenu.Items.Add(UiText.T("설치 위치"),null,(sender,e)=>BeginInvoke(new Action(()=>{using(var dialog=new FolderBrowserDialog()){dialog.SelectedPath=folder.Text;if(dialog.ShowDialog(this)==DialogResult.OK){folder.Text=dialog.SelectedPath;downloadedPacks.Clear();updateDeclined=false;SaveSettings();RefreshState();}}})));
        var settings=Button("설정",()=>settingsMenu.Show(Cursor.Position));footer.Controls.Add(settings);
        launcherUpdateButton=Button("런처 업데이트",async()=>await Busy(UpdateSelf));footer.Controls.Add(launcherUpdateButton);notice.Size=new Size(780,38);notice.ForeColor=LauncherVisuals.Muted;footer.Controls.Add(notice);layout.Controls.Add(footer);
        folder.TextChanged+=(sender,e)=>RefreshState();
        language.SelectedIndexChanged += (sender,e) => { UiText.Language=languageCodes[language.SelectedIndex]; ApplyLanguage(this); SaveSettings(); };
        loadingSettings = false; ApplyLanguage(this);
        if(settingsFile==null)Shown += async (sender,e) => await Busy(RefreshCatalog);
    }
    protected override void Dispose(bool disposing){if(disposing)settingsMenu.Dispose();base.Dispose(disposing);}
    static string FindPreviousSettings() {
        string local=Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"launcher-settings.json");if(File.Exists(local))return local;
        try {
            string parent=Directory.GetParent(AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar)).FullName;
            foreach(string directory in Directory.GetDirectories(parent,"Macross30-launcher-*").OrderByDescending(Directory.GetLastWriteTimeUtc)) {
                string path=Path.Combine(directory,"launcher-settings.json");if(!File.Exists(path))continue;
                try{var saved=Packages.Read(File.ReadAllText(path));if(File.Exists(Path.Combine(Packages.Text(saved,"folder"),"bin/Macross30Recompiled.exe")))return path;}catch{}
            }
        } catch{}
        return local;
    }
    void SaveSettings() {
        if(loadingSettings) return;
        try { Directory.CreateDirectory(Path.GetDirectoryName(config));File.WriteAllText(config,new JavaScriptSerializer().Serialize(new { folder=folder.Text, ui_language=UiText.Language, game_language=gameLocale, iso=isoPath }),new UTF8Encoding(false)); } catch { }
    }
    void ApplyLanguage(Control parent) {
        TranslateControls(parent);
        string family=UiText.Language=="ja" ? "Yu Gothic UI" : UiText.Language=="en" ? "Segoe UI" : "맑은 고딕";
        if(Font.FontFamily.Name!=family) Font=new Font(family,10);
        notice.Text=UiText.T("언어를 선택하고 플레이를 누르세요. 필요한 업데이트는 확인 후 적용합니다.");


        RefreshState();
    }
    void TranslateControls(Control parent) {
        foreach(Control child in parent.Controls) {
            if(child.Tag is string) {
                string key=(string)child.Tag;
                child.Text=key.StartsWith("런처 버전: ",StringComparison.Ordinal) ? UiText.T("런처 버전: ")+SelfUpdate.Version : UiText.T(key);
            }
            if(child.HasChildren) TranslateControls(child);
        }
    }
    Label Caption(string key,int size,bool bold) {return new Label {Tag=key,Text=UiText.T(key),Dock=DockStyle.Fill,Margin=Padding.Empty,TextAlign=ContentAlignment.MiddleLeft,Font=new Font(Font.FontFamily,size,bold?FontStyle.Bold:FontStyle.Regular),ForeColor=bold?LauncherVisuals.Text:LauncherVisuals.Muted};}
    Button Button(string label, Action click) {
        var b = new LauncherAction { Tag = label, Text = UiText.T(label), AutoSize = true, Height = 38, MinimumSize = new Size(120, 38), FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(38, 55, 76), ForeColor = Color.White, Margin = new Padding(0, 4, 12, 4) };
        b.FlatAppearance.BorderColor=Color.FromArgb(160,190,220);b.FlatAppearance.BorderSize=1;
        b.Click += (s,e) => click(); actions.Add(b); return b;
    }
    void Log(string message) { if (InvokeRequired) { BeginInvoke(new Action<string>(Log), message); return; } string text=UiText.T(message); notice.Text=text;transferDetail.Text=text; output.AppendText(DateTime.Now.ToString("HH:mm:ss") + "  " + text + Environment.NewLine); }
    public static Color StatusColor(int status) {return status==2 ? Color.FromArgb(30,112,65) : status==1 ? Color.FromArgb(132,99,0) : Color.FromArgb(153,44,52);}
    string StatusText(int status) {return UiText.T(status==2 ? "최신" : status==1 ? "업데이트 필요" : "미설치");}
    void RefreshButtons() {
        playLanguage.Enabled=!working;
        languageVersion.Text=gameLocale=="ja-JP"?UiText.T("언어팩 버전: 해당 없음 (원본 일본어)"):UiText.F("언어팩 버전: {0}\n최신 버전: {1}",PrebuiltInstall.LanguageVersion(folder.Text,gameLocale,latestManifest,true),PrebuiltInstall.LanguageVersion(folder.Text,gameLocale,latestManifest,false));
        if(playButton!=null){playButton.Text=UiText.T(working?"진행 중…":"플레이");playButton.Enabled=!working;}
        if(launcherUpdateButton!=null){int status=latestLauncherVersion!=null&&Packages.VersionOf(SelfUpdate.Version)>=Packages.VersionOf(latestLauncherVersion)?2:1;launcherUpdateButton.BackColor=StatusColor(status);launcherUpdateButton.Text=UiText.T("런처 업데이트")+" · "+(latestLauncherVersion==null?UiText.T("확인 중"):StatusText(status));launcherUpdateButton.Enabled=!working;}
    }
    void RefreshState() {
        RefreshButtons();
        try {
            string installed = Packages.InstalledVersion(folder.Text);
            state.Text=UiText.F("게임 버전: {0} · 최신 버전: {1}",PrebuiltInstall.RuntimeStatus(folder.Text,latestManifest)==0?UiText.T("미설치"):installed,latestManifest==null?UiText.T("확인 중"):Packages.Text(latestManifest,"version"));

        }
        catch { state.Text = UiText.T("설치 정보를 읽을 수 없습니다. 폴더를 확인하세요."); }
    }
    async Task Busy(Func<Task> operation) {
        if(working)return;working=true;foreach (var b in actions) b.Enabled = false;if(logsButton!=null)logsButton.Enabled=true; folder.Enabled = language.Enabled = false; notice.Text=UiText.T("작업 중입니다. 잠시 기다려주세요.");RefreshButtons();
        try { await operation(); } catch (WebException ex) {
            var response = ex.Response as HttpWebResponse;
            if (response != null && response.StatusCode == HttpStatusCode.NotFound)
                Log(UiText.T("설치 파일을 확인할 수 없습니다. 인터넷 연결을 확인한 뒤 다시 적용하세요."));
            else Log(UiText.T("다운로드 실패: ") + ex.Message);
            MessageBox.Show(this,notice.Text,UiText.T("설치 실패"),MessageBoxButtons.OK,MessageBoxIcon.Error);
        } catch (Exception ex) { Log(UiText.T("실패: ") + UiText.T(ex.Message)); if(!ex.Data.Contains("InstallProgressShown")) MessageBox.Show(this,UiText.T(ex.Message),UiText.T("설치 실패"),MessageBoxButtons.OK,MessageBoxIcon.Error); }
        finally {working=false; if (!updatingSelf) { foreach (var b in actions) b.Enabled = true; folder.Enabled = language.Enabled = true; RefreshState(); } }
    }
    Downloads Client(bool binary) {
        var c = new Downloads(); c.Encoding = Encoding.UTF8;
        c.Headers["User-Agent"] = "Macross30-Launcher/" + SelfUpdate.Version;
        c.Headers["Accept"] = binary ? "application/octet-stream" : "application/vnd.github+json";
        return c;
    }
    static async Task<T> Timed<T>(Downloads client, Task<T> task) {
        using (var deadline = new CancellationTokenSource(120000))
        using (deadline.Token.Register(() => client.CancelAsync())) {
            try { return await task; }
            catch (OperationCanceledException) { throw new TimeoutException(UiText.T("다운로드 제한 시간(2분)을 초과했습니다. 다시 시도하세요.")); }
        }
    }
    static async Task Timed(Downloads client, Task task) {
        using (var deadline = new CancellationTokenSource(600000))
        using (deadline.Token.Register(() => client.CancelAsync())) {
            try { await task; }
            catch (OperationCanceledException) { throw new TimeoutException(UiText.T("다운로드가 취소되었거나 제한 시간(10분)을 초과했습니다.")); }
        }
    }
    public static bool NeedsPackageUpdate(string installed, string available) {
        return installed=="설치 안 됨" || installed=="이전 배포본" || Packages.VersionOf(installed)<Packages.VersionOf(available);
    }
    bool NeedsAction(){return !PrebuiltInstall.CoreReady(folder.Text) || !LocalBuild.Ready(folder.Text,gameLocale) || downloadedPacks.ContainsKey(gameLocale) || !updateDeclined && latestManifest!=null && (PrebuiltInstall.RuntimeStatus(folder.Text,latestManifest)!=2 || gameLocale!="ja-JP" && PrebuiltInstall.LanguageStatus(folder.Text,gameLocale,latestManifest)!=2);}
    async Task MainAction(){
        Packages.EnsureStopped(Path.GetFullPath(folder.Text));
        await prepareGameOperation();if(updateDeclined)return;
        if(!File.Exists(Path.Combine(folder.Text,"bin/Macross30Recompiled.exe")))return;
        if(latestManifest!=null && PrebuiltInstall.RuntimeStatus(folder.Text,latestManifest)!=2)return;
        await PrepareOriginalForPlay();if(!PrebuiltInstall.CoreReady(folder.Text))return;
        if(gameLocale!="ja-JP" && downloadedPacks.ContainsKey(gameLocale))await ManageLanguage(gameLocale);
        if(!LocalBuild.Ready(folder.Text,gameLocale)){Log(UiText.T("선택한 언어의 다운로드·적용이 필요합니다."));return;}
        transferProgress.Value=100;Log(UiText.T("3/3 · 게임 실행"));await playGameOperation();
    }
    async Task UpdateGame() {
        updateDeclined=false;SaveSettings();if(latestManifest==null)await RefreshCatalog();if(latestManifest==null){Log(UiText.T("공개된 게임 패치가 아직 없습니다."));return;}
        bool core=PrebuiltInstall.RuntimeStatus(folder.Text,latestManifest)!=2;
        bool pack=gameLocale!="ja-JP" && PrebuiltInstall.LanguageStatus(folder.Text,gameLocale,latestManifest)!=2 && !downloadedPacks.ContainsKey(gameLocale);
        if(!core && !pack){Log(UiText.T("게임은 최신 상태입니다."));return;}
        long bytes=0;if(core)bytes+=Convert.ToInt64(Packages.Object(Packages.Object(latestManifest["files"])["program"])["bytes"]);
        if(pack)bytes+=Convert.ToInt64(Packages.Object(Packages.Object(latestManifest["language_packs"])[gameLocale])["bytes"]);
        string selectedName=gameLocale=="ko-KR"?"한국어":gameLocale=="en-US"?"English":"日本語";
        string items=(core?UiText.T("기본 게임 패치")+" "+Packages.Text(latestManifest,"version"):"")+(core&&pack?Environment.NewLine:"")+(pack?selectedName+" · "+UiText.T("언어 패치")+" ("+StatusText(PrebuiltInstall.LanguageStatus(folder.Text,gameLocale,latestManifest))+")":"");
        string question=UiText.F("다음을 최신 버전으로 준비한 뒤 플레이하시겠습니까?\n\n{0}\n\n다운로드: {1} MB",items,(bytes/1048576.0).ToString("0.0"));
        if(!confirmPatchDownload(question)){updateDeclined=true;Log(UiText.T("업데이트를 취소했습니다. 기존 설치본을 유지합니다."));return;}
        if(core){Log(UiText.T("1/3 · 기본 게임 패치"));await UpdateCorePackage(latestManifest,latestUrls);}
        if(pack){Log(UiText.T("2/3 · 언어 패치"));await ManageLanguage(gameLocale);}RefreshState();
    }
    static string ReleaseDownloadBase(){return "https://github.com/"+Packages.Repository+"/releases/latest/download/";}
    async Task RefreshCatalog() {
        if(latestManifest!=null && (DateTime.UtcNow-catalogChecked).TotalSeconds<30)return;
        Log(UiText.T("최신 설치 패키지를 확인하고 있습니다…"));
        string origin=ReleaseDownloadBase(),fresh="?check="+DateTime.UtcNow.Ticks;
        Dictionary<string,object> manifest,launcher;
        using(var client=Client(false))manifest=Packages.Read(await Timed(client,client.DownloadStringTaskAsync(origin+"BOOT-INSTALL.json"+fresh)));
        PrebuiltInstall.Validate(manifest);
        using(var client=Client(false))launcher=Packages.Read(await Timed(client,client.DownloadStringTaskAsync(origin+"LAUNCHER.json"+fresh)));
        SelfUpdate.ValidateManifest(launcher);latestLauncherVersion=Packages.Text(launcher,"version");
        var urls=new Dictionary<string,string>(StringComparer.Ordinal);
        foreach(var value in Packages.Object(manifest["files"]).Values){var row=Packages.Object(value);urls[Packages.Text(row,"file")]=origin+Packages.Text(row,"file");}
        if(manifest.ContainsKey("language_packs"))foreach(var value in Packages.Object(manifest["language_packs"]).Values){var row=Packages.Object(value);urls[Packages.Text(row,"file")]=origin+Packages.Text(row,"file");}
        string root=folder.Text;await Task.Run(()=>{PrebuiltInstall.RuntimeStatus(root,manifest);foreach(string locale in GameLanguage.Codes)PrebuiltInstall.LanguageStatus(root,locale,manifest);});
        latestManifest=manifest;latestUrls=urls;catalogChecked=DateTime.UtcNow;RefreshState();
    }
    async Task<string> FetchArtifact(string root,Dictionary<string,object> row,Dictionary<string,string> urls) {
        string name=Packages.Text(row,"file"),path=await Task.Run(()=>LocalBuild.FindCachedArchive(root,row));
        if(path!=null){Log(UiText.F("이미 받은 파일 재사용: {0}",name));return path;}
        path=LocalBuild.ArchiveCachePath(root,row);string partial=path+".partial";Log(name+UiText.T(" 다운로드 중…"));
        if(urls==null)File.Copy(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,name),partial,true);
        else using(var client=Client(true)){long size=Convert.ToInt64(row["bytes"]);client.DownloadProgressChanged+=(a,e)=>{if(e.BytesReceived>size)client.CancelAsync();transferProgress.Value=(int)Math.Min(100,e.BytesReceived*100/Math.Max(1,size));transferDetail.Text=String.Format("{0} · {1:0.0} / {2:0.0} MB · {3}%",name,e.BytesReceived/1048576.0,size/1048576.0,transferProgress.Value);};await Timed(client,client.DownloadFileTaskAsync(urls[name],partial));}
        Packages.VerifyArchive(partial,row);if(File.Exists(path))File.Delete(path);File.Move(partial,path);return path;
    }
    bool EnsureIso(string root,string locale,Dictionary<string,object> manifest) {
        if(File.Exists(isoPath))return true;
        bool dataReady=false;try {
            var profile=Packages.Read(File.ReadAllText(Path.Combine(root,"release-profile.json")));
            var actual=Packages.Object(Packages.Object(profile["language_views"])[locale]);
            var expected=Packages.Object(PrebuiltInstall.IsBaseLanguage(manifest,locale)?manifest["base_language"]:Packages.Object(manifest["language_packs"])[locale]);
            string original=Path.Combine(root,".local-build/original/EBOOT.elf");
            dataReady=LocalBuild.Ready(root,locale) && Packages.Text(actual,"plan_sha256")==Packages.Text(expected,"plan_sha256") && File.Exists(original) && Packages.Hash(original)==LocalBuild.OriginalHash;
        }catch{}
        if(dataReady)return true;
        using(var dialog=new OpenFileDialog {Filter=UiText.T("원본 디스크 이미지 (*.iso)|*.iso")}) {if(dialog.ShowDialog()!=DialogResult.OK)return false;isoPath=dialog.FileName;SaveSettings();return true;}
    }
    async Task UpdateCorePackage(Dictionary<string,object> manifest,Dictionary<string,string> urls) {
        PrebuiltInstall.CheckRuntime();string root=Path.GetFullPath(folder.Text);Directory.CreateDirectory(root);
        if(PrebuiltInstall.RuntimeStatus(root,manifest)==2){Log(UiText.T("게임은 최신 상태입니다."));return;}
        var row=Packages.Object(Packages.Object(manifest["files"])["program"]);string archive=await FetchArtifact(root,row,urls);
        string program=await Task.Run(()=>PrebuiltInstall.Extract(root,archive,row));
        try{Packages.EnsureStopped(root);}catch(IOException){Log(UiText.T("패치 다운로드 완료. 게임을 종료한 뒤 업데이트를 눌러 적용하세요."));return;}
        await Task.Run(()=>PrebuiltInstall.StageRuntime(root,program,Packages.Text(manifest,"version")));
        PrebuiltInstall.RecordProgram(root,program,row);Log(UiText.T("게임 업데이트가 완료되었습니다."));
    }
    async Task PrepareOriginalForPlay() {
        string root=Path.GetFullPath(folder.Text),program=PrebuiltInstall.InstalledProgram(root);
        if(PrebuiltInstall.CoreReady(root))return;
        if(!File.Exists(isoPath))using(var dialog=new OpenFileDialog {Title=UiText.T("게임 ISO 선택"),Filter=UiText.T("원본 디스크 이미지 (*.iso)|*.iso")}){if(dialog.ShowDialog()!=DialogResult.OK)return;isoPath=dialog.FileName;SaveSettings();}
        var profile=Packages.Read(File.ReadAllText(Path.Combine(root,"release-profile.json")));var artifact=Packages.Object(profile["program_artifact"]);
        string requested=gameLocale;await RunInstaller(root,program,Path.Combine(program,"base-game"),program,true,Packages.Text(profile,"release_version"),"ja-JP",false);
        PrebuiltInstall.RecordProgram(root,program,artifact);gameLocale=requested;SaveSettings();
    }
    void SelectInstalledLanguage(string locale) {
        string root=Path.GetFullPath(folder.Text);if(!PrebuiltInstall.CoreReady(root))throw new InvalidOperationException(UiText.T("먼저 게임을 준비하세요."));
        Packages.EnsureStopped(root);LocalBuild.SelectLanguage(root,locale,UiText.Language);gameLocale=locale;SaveSettings();ApplyLanguage(this);Log(UiText.T("사용 언어를 변경했습니다."));
    }
    async Task ManageLanguage(string locale) {
        gameLocale=locale;SaveSettings();ApplyLanguage(this);
        if(locale=="ja-JP"){if(PrebuiltInstall.CoreReady(folder.Text))SelectInstalledLanguage(locale);return;}
        int status=PrebuiltInstall.LanguageStatus(folder.Text,locale,latestManifest);
        if(LocalBuild.Ready(folder.Text,locale) && (latestManifest==null || status==2)){if(PrebuiltInstall.CoreReady(folder.Text))SelectInstalledLanguage(locale);return;}
        string payload;
        if(!downloadedPacks.TryGetValue(locale,out payload) || !Directory.Exists(payload)) {
            await RefreshCatalog();if(latestManifest==null)throw new InvalidOperationException(UiText.T("설치 패키지가 아직 준비되지 않았습니다. 잠시 후 다시 적용하세요."));
            string root=Path.GetFullPath(folder.Text);Directory.CreateDirectory(root);var row=Packages.Object(Packages.Object(latestManifest["language_packs"])[locale]);
            string archive=await FetchArtifact(root,row,latestUrls),work=Path.Combine(root,".local-build/language-packages");Directory.CreateDirectory(work);
            payload=await Task.Run(()=>LocalBuild.Extract(work,"data",archive,row));downloadedPacks[locale]=payload;
        }
        if(!PrebuiltInstall.CoreReady(folder.Text)){Log(UiText.T("언어팩 다운로드 완료. 게임 실행 시 적용됩니다."));RefreshState();return;}
        string target=Path.GetFullPath(folder.Text),installed=PrebuiltInstall.InstalledProgram(target);
        await RunInstaller(target,installed,payload,null,true,null,locale,true);PrebuiltInstall.RecordLanguageVersion(target,locale,PrebuiltInstall.LanguageVersion(target,locale,latestManifest,false));downloadedPacks.Remove(locale);SelectInstalledLanguage(locale);Log(UiText.T("언어팩 설치가 완료되었습니다."));
    }
    async Task RunLocalBuild(string root,string source,string payload,string runtime) {await RunInstaller(root,source,payload,runtime,false,null);}
    async Task RunInstaller(string root,string source,string payload,string runtime,bool prebuilt,string version,string installLocale=null,bool languageOnly=false) {
        string locale=installLocale??gameLocale;
        string entry=languageOnly ? "tools/local_build/install_language.ps1" : prebuilt ? "tools/local_build/install_prebuilt.ps1" : "tools/local_build/install.ps1";
        string script=Path.Combine(source,entry);
        if(!File.Exists(script)) throw new InvalidDataException("Local installer source missing");
        Log(UiText.T(languageOnly ? "선택한 언어팩만 설치합니다." : prebuilt ? "원본 게임과 공통 실행 파일을 준비합니다." : "처음 설치는 도구 준비와 코드 생성·컴파일에 시간이 걸립니다. 진행 창을 닫지 마세요."));
        Packages.NoLinks(root,"play/runner.lock");Directory.CreateDirectory(Path.Combine(root,"play"));
        using(var lease=new FileStream(Path.Combine(root,"play/runner.lock"),FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.None)) {
            if(!prebuilt && File.Exists(Path.Combine(source,"source-manifest.json")))source=await Task.Run(()=>LocalBuild.PrepareWorkspace(root,source));
            script=Path.Combine(source,entry);
            string arguments="-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+script+"\" -Root \""+root+"\" -Iso \""+isoPath+"\" -Payload \""+payload+"\" -Runtime \""+runtime+"\" -Language "+locale;
            if(languageOnly)arguments="-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+script+"\" -Root \""+root+"\" -Iso \""+isoPath+"\" -Payload \""+payload+"\" -Language "+locale;
            else if(prebuilt)arguments=arguments.Replace(" -Runtime "," -Program ")+" -ReleaseVersion "+version;
            var start=new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),"WindowsPowerShell/v1.0/powershell.exe"),arguments) { UseShellExecute=false,CreateNoWindow=true,WorkingDirectory=source,RedirectStandardOutput=true,RedirectStandardError=true };
            string logFile=Path.Combine(root,!prebuilt?"play/local-build.log":languageOnly?"play/language-"+locale+".log":"play/game-prepare.log");
            using(var progress=new InstallProgress(logFile,source,prebuilt,languageOnly)) {
                Func<Task> install=async ()=> {
                    using(var process=new Process {StartInfo=start})
                    using(var writer=new StreamWriter(new FileStream(logFile,FileMode.Create,FileAccess.Write,FileShare.Read),new UTF8Encoding(false))) {
                        var sync=new object();var clock=Stopwatch.StartNew();long lastFlush=0,lastDisplay=0;
                        DataReceivedEventHandler receive=(sender,e)=> {
                            if(e.Data==null)return;
                            lock(sync) {
                                writer.WriteLine(e.Data);
                                bool important=e.Data.StartsWith("BOOT_",StringComparison.Ordinal) || e.Data.StartsWith("LOCAL_",StringComparison.Ordinal) || e.Data.StartsWith("LOCAL GENERATE:",StringComparison.Ordinal) || Regex.IsMatch(e.Data,@"^\[\d+/\d+\]|(?:lifted|emitting)\s+\d+/\d+");
                                if(important || clock.ElapsedMilliseconds-lastFlush>=1000){writer.Flush();lastFlush=clock.ElapsedMilliseconds;}
                                if(important || clock.ElapsedMilliseconds-lastDisplay>=1000){string line=e.Data;BeginInvoke(new Action(()=>{var match=Regex.Match(line,@"^BOOT_STAGE ([1-7])/7\s*(.*)$");if(match.Success){transferProgress.Value=(Int32.Parse(match.Groups[1].Value)-1)*100/7;string[] stages=languageOnly?new[]{"게임 준비 확인","원본 확인","언어 패치 준비","언어팩 설치","자막 설치","무결성 검증","완료"}:new[]{"도구 준비","ISO 원본 준비","실행 데이터 준비","공통 실행 파일 설치","원본 게임 데이터 준비","실행 환경 구성","게임 준비 검증"};transferDetail.Text=match.Groups[1].Value+"/7 · "+UiText.T(stages[Int32.Parse(match.Groups[1].Value)-1]);}else if(line.StartsWith("BOOT_")||line.StartsWith("LOCAL_"))transferDetail.Text=UiText.T(line);output.AppendText(line+Environment.NewLine);}));lastDisplay=clock.ElapsedMilliseconds;}
                            }
                        };
                        process.OutputDataReceived+=receive;process.ErrorDataReceived+=receive;
                        process.Start();process.BeginOutputReadLine();process.BeginErrorReadLine();await Task.Run(()=>process.WaitForExit());
                        writer.Flush();
                        if(process.ExitCode!=0) throw new IOException(UiText.F("작업에 실패했습니다. 로그: {0}",Path.GetFileName(logFile))+" ("+process.ExitCode+")");
                    }
                    if(!LocalBuild.Ready(root,locale)) throw new IOException("Local generation completion verification failed");
                };await install();
            }
        }
        if(!LocalBuild.Ready(root,locale)) throw new IOException("Local generation completion verification failed");
        transferProgress.Value=100;Log(UiText.T("설치 적용이 완료되었습니다. 게임 실행을 누르세요."));
    }
    async Task UpdateSelf() {
        string source = Packages.Repository;
        if (!Regex.IsMatch(source,@"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")) throw new InvalidDataException(UiText.T("저장소 형식을 확인하세요."));
        Dictionary<string,object> best;
        string origin=ReleaseDownloadBase();using(var client=Client(false))best=Packages.Read(await Timed(client,client.DownloadStringTaskAsync(origin+"LAUNCHER.json?check="+DateTime.UtcNow.Ticks)));
        SelfUpdate.ValidateManifest(best);string url=origin+Packages.Text(best,"file");
        if(best!=null){latestLauncherVersion=Packages.Text(best,"version");RefreshState();}
        if (best == null) throw new InvalidOperationException(UiText.T("릴리스에 LAUNCHER.json과 런처 실행 파일이 아직 없습니다."));
        if (Packages.VersionOf(Packages.Text(best,"version")) <= Packages.VersionOf(SelfUpdate.Version)) { Log(UiText.T("런처는 최신 버전 ") + SelfUpdate.Version + UiText.T("입니다.")); return; }
        string target = Process.GetCurrentProcess().MainModule.FileName;
        if (Path.GetFileName(target) != "Macross30Launcher.exe") throw new IOException(UiText.T("런처 파일 이름을 Macross30Launcher.exe로 유지하세요."));
        string work = Path.Combine(Path.GetDirectoryName(target),".launcher-update-" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(work);
        bool handedOff = false;
        try {
            string pending = Path.Combine(work,"new.exe");
            Log(UiText.T("런처 ") + SelfUpdate.Version + " → " + Packages.Text(best,"version") + UiText.T(" 다운로드 중…"));
            using (var client = Client(true)) {
                long maximum = Convert.ToInt64(best["bytes"]);
                client.DownloadProgressChanged += (s,e) => { if (e.BytesReceived > maximum) client.CancelAsync(); };
                await Timed(client,client.DownloadFileTaskAsync(url,pending));
            }
            Packages.VerifyArchive(pending,best);
            string plan = Path.Combine(work,"plan.json"), helper = Path.Combine(work,"helper.exe"); File.Copy(target,helper);
            using (var current = Process.GetCurrentProcess())
                File.WriteAllText(plan,new JavaScriptSerializer().Serialize(new { target = target, pid = current.Id, started = current.StartTime.ToUniversalTime().Ticks, current_sha256 = Packages.Hash(target), sha256 = Packages.Text(best,"sha256") }),new UTF8Encoding(false));
            SaveSettings();
            Process.Start(new ProcessStartInfo(helper,"--self-update \"" + plan + "\"") { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = work });
            handedOff = true; updatingSelf = true; Close();
        } finally { if (!handedOff) Directory.Delete(work,true); }
    }
    async Task Play() {
        string root=Path.GetFullPath(folder.Text);Packages.EnsureStopped(root);
        if(!File.Exists(Path.Combine(root,"bin/Macross30Recompiled.exe"))){await UpdateGame();if(!File.Exists(Path.Combine(root,"bin/Macross30Recompiled.exe")))return;}
        if(!PrebuiltInstall.CoreReady(root) && !File.Exists(isoPath))using(var dialog=new OpenFileDialog {Title=UiText.T("게임 ISO 선택"),Filter=UiText.T("원본 디스크 이미지 (*.iso)|*.iso")}){if(dialog.ShowDialog()!=DialogResult.OK)return;isoPath=dialog.FileName;SaveSettings();}
        await PrepareOriginalForPlay();if(!PrebuiltInstall.CoreReady(root))return;
        if(gameLocale!="ja-JP" && (!LocalBuild.Ready(root,gameLocale) || downloadedPacks.ContainsKey(gameLocale)))await ManageLanguage(gameLocale);
        if(!LocalBuild.Ready(root,gameLocale))return;
        LocalBuild.SelectLanguage(root,gameLocale,UiText.Language);
        string cmd = Path.Combine(root,"Macross30.cmd");
        Log(UiText.T("게임 실행 중…"));
        var info = new ProcessStartInfo(Environment.GetEnvironmentVariable("COMSPEC") ?? "cmd.exe", "/d /c \"\"" + cmd + "\"\"") {
            WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true
        };
        using (var p = Process.Start(info)) {
            p.StandardInput.Close(); await Task.Run(() => p.WaitForExit());
            Log(UiText.T("게임 종료 (") + p.ExitCode + UiText.T("). 로그는 설치 폴더의 play에 있습니다."));
        }
    }
    protected override void OnFormClosing(FormClosingEventArgs e) {
        if (!updatingSelf && working) { e.Cancel = true; Log(UiText.T("현재 작업이 끝난 뒤 런처를 닫을 수 있습니다.")); }
        else SaveSettings();
        base.OnFormClosing(e);
    }
    [STAThread] public static void Main(string[] args) {
        if(args.Length==3 && args[0]=="--prepare-language") {
            try { GameLanguage.Prepare(Path.GetFullPath(args[1]),args[2]); } catch { Environment.ExitCode=1; } return;
        }
        if (args.Length == 2 && args[0] == "--self-update") {
            try { SelfUpdate.Run(args[1]); } catch (Exception ex) { MessageBox.Show(UiText.T(ex.Message),UiText.T("런처 업데이트 실패"),MessageBoxButtons.OK,MessageBoxIcon.Error); }
            return;
        }
        ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
        Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
        using (var form = new LauncherWindow(args.Length>=2 && args[0]=="--render-preview" ? Path.Combine(Path.GetTempPath(),"macross30-render-settings.json") : null)) {
            if (args.Length == 2 && args[0] == "--update-health") {
                string health = Path.GetFullPath(args[1]);
                form.Shown += (s,e) => File.WriteAllText(health,Packages.Hash(Process.GetCurrentProcess().MainModule.FileName));
            }
            if (args.Length >= 2 && args[0] == "--render-preview") {
                if(args.Length==3) { UiText.Language=UiText.Normalize(args[2]); form.language.SelectedIndex=Array.IndexOf(form.languageCodes,UiText.Language); form.ApplyLanguage(form); }
                form.Show(); form.PerformLayout(); Application.DoEvents();
                using (var bitmap = new Bitmap(form.Width, form.Height)) { form.DrawToBitmap(bitmap, new Rectangle(Point.Empty, form.Size)); bitmap.Save(args[1]); }
            } else Application.Run(form);
        }
    }
}
}
