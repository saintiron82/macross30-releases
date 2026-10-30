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

public sealed class LauncherWindow : Form {
    TextBox folder = new TextBox(), repo = new TextBox(), token = new TextBox(), output = new TextBox();
    Label state = new Label(), notice = new Label(), guide = new Label();
    List<Button> actions = new List<Button>();
    ListView files = new ListView();
    ComboBox language = new ComboBox();
    List<Button> languageButtons=new List<Button>();
    List<Button> gameButtons=new List<Button>(); string gameLocale="ko-KR", isoPath="";
    Panel advanced = new Panel(); Button advancedButton;
    string config = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "launcher-settings.json");
    bool updatingSelf, loadingSettings;
    readonly string[] languageCodes = { "ko", "en", "ja" };
    public LauncherWindow() {
        Text = "Macross 30 Launcher " + SelfUpdate.Version; ClientSize = new Size(940, 730); MinimumSize = new Size(940, 760);
        StartPosition = FormStartPosition.CenterScreen; Font = new Font("맑은 고딕", 10); BackColor = Color.FromArgb(22, 29, 42); ForeColor = Color.WhiteSmoke;
        folder.Text = File.Exists(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Macross30.cmd")) ? AppDomain.CurrentDomain.BaseDirectory : Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Macross30");
        repo.Text = Packages.Repository; token.UseSystemPasswordChar = true;
        loadingSettings = true;
        if (File.Exists(config)) try {
            var saved = Packages.Read(File.ReadAllText(config)); folder.Text = Packages.Text(saved, "folder"); repo.Text = Packages.Text(saved, "repository"); if(repo.Text=="saintiron82/macross30-recomp") repo.Text=Packages.Repository;
            if(saved.ContainsKey("game_language")) gameLocale=GameLanguage.Normalize(Packages.Text(saved,"game_language"));
            if(saved.ContainsKey("iso")) isoPath=Packages.Text(saved,"iso");
            if (saved.ContainsKey("ui_language")) UiText.Language = UiText.Normalize(Packages.Text(saved,"ui_language"));
        } catch { }
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(24), ColumnCount = 1, RowCount = 9 };
        foreach (int height in new[] { 44, 40, 40, 270, 48, 58, 34, 48 }) layout.RowStyles.Add(new RowStyle(SizeType.Absolute,height));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent,100)); Controls.Add(layout);
        var heading=new TableLayoutPanel { Dock=DockStyle.Fill, ColumnCount=2 };
        heading.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100)); heading.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,260));
        heading.Controls.Add(new Label { Text = "MACROSS 30", Font = new Font(Font.FontFamily,23,FontStyle.Bold), Dock = DockStyle.Fill });
        heading.Controls.Add(new Label { Tag="런처 버전: "+SelfUpdate.Version, Text=UiText.T("런처 버전: ")+SelfUpdate.Version, Font=new Font(Font.FontFamily,13,FontStyle.Bold), Dock=DockStyle.Fill, TextAlign=ContentAlignment.MiddleRight, ForeColor=Color.LightSkyBlue });
        layout.Controls.Add(heading);
        var languageRow = new FlowLayoutPanel { Dock = DockStyle.Fill };
        languageRow.Controls.Add(new Label { Tag = "런처 언어", Text = UiText.T("런처 언어"), Width = 110, Height = 30, TextAlign = ContentAlignment.MiddleLeft });
        language.DropDownStyle = ComboBoxStyle.DropDownList; language.ForeColor=Color.Black; language.BackColor=Color.White; language.Width = 230; language.Items.AddRange(new object[] { "한국어", "English", "日本語" });
        language.DrawMode=DrawMode.OwnerDrawFixed; language.ItemHeight=22;
        language.DrawItem += (sender,e) => {
            e.DrawBackground(); string text=e.Index>=0 ? Convert.ToString(language.Items[e.Index]) : Convert.ToString(language.SelectedItem);
            using(var brush=new SolidBrush(e.ForeColor)) e.Graphics.DrawString(text,language.Font,brush,e.Bounds.Left+3,e.Bounds.Top+2);
            e.DrawFocusRectangle();
        };
        language.SelectedIndex = Array.IndexOf(languageCodes,UiText.Language);
        for(int i=0;i<languageCodes.Length;i++) {
            int index=i;
            var choice=Button(Convert.ToString(language.Items[i]),() => language.SelectedIndex=index);
            choice.AutoSize=false; choice.MinimumSize=new Size(100,30); choice.Size=new Size(100,30); choice.Margin=new Padding(0,2,8,0);
            languageButtons.Add(choice); languageRow.Controls.Add(choice);
        }
        layout.Controls.Add(languageRow);
        var gameRow=new FlowLayoutPanel { Dock=DockStyle.Fill };
        gameRow.Controls.Add(new Label { Tag="게임 언어", Text=UiText.T("게임 언어"), Width=110, Height=30, TextAlign=ContentAlignment.MiddleLeft });
        string[] gameNames={"한국어","English","日本語"};
        for(int i=0;i<GameLanguage.Codes.Length;i++) {
            int index=i;
            var choice=Button(gameNames[i],() => { gameLocale=GameLanguage.Codes[index]; SaveSettings(); ApplyLanguage(this); });
            choice.AutoSize=false;choice.MinimumSize=new Size(100,30);choice.Size=new Size(100,30);choice.Margin=new Padding(0,2,8,0);
            gameButtons.Add(choice);gameRow.Controls.Add(choice);
        }
        layout.Controls.Add(gameRow);
        guide.Dock = DockStyle.Fill; guide.Padding = new Padding(12); guide.BackColor = Color.FromArgb(30,43,60); guide.Text = UiText.Guide; layout.Controls.Add(guide);
        layout.Controls.Add(Field("설치 폴더", folder, "찾기", () => {
            using (var d = new FolderBrowserDialog()) { d.SelectedPath = folder.Text; if (d.ShowDialog() == DialogResult.OK) folder.Text = d.SelectedPath; } SaveSettings(); RefreshState();
        }));
        var primary = new FlowLayoutPanel { Dock = DockStyle.Fill };
        primary.Controls.Add(Button("적용", async () => await Busy(InstallWizard)));
        primary.Controls.Add(Button("게임 실행", async () => await Busy(Play))); layout.Controls.Add(primary);
        state.Dock = DockStyle.Fill; state.ForeColor = Color.LightSkyBlue; layout.Controls.Add(state);
        notice.Dock = DockStyle.Fill; notice.Text = UiText.T("준비되었습니다. 위 안내에 따라 시작하세요."); layout.Controls.Add(notice);
        var lower = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
        lower.RowStyles.Add(new RowStyle(SizeType.Absolute,44)); lower.RowStyles.Add(new RowStyle(SizeType.Percent,100));
        advancedButton = Button("고급 메뉴", ShowAdvanced); lower.Controls.Add(advancedButton); layout.Controls.Add(lower);
        advanced.Dock = DockStyle.Fill; advanced.Visible = false; lower.Controls.Add(advanced);
        var details = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 5 };
        foreach (int height in new[] { 40, 40, 48, 48 }) details.RowStyles.Add(new RowStyle(SizeType.Absolute,height));
        details.RowStyles.Add(new RowStyle(SizeType.Percent,100)); advanced.Controls.Add(details);
        details.Controls.Add(Field("GitHub 저장소",repo,null,null)); details.Controls.Add(Field("토큰 (선택)",token,null,null));
        var extra = new FlowLayoutPanel { Dock = DockStyle.Fill };


        extra.Controls.Add(Button("로컬 패치 적용",async () => await Busy(UpdateLocal)));
        extra.Controls.Add(Button("런처 업데이트",async () => await Busy(UpdateSelf))); details.Controls.Add(extra);
        var utilities = new FlowLayoutPanel { Dock = DockStyle.Fill };

        utilities.Controls.Add(Button("로그 폴더",() => { string path=Path.Combine(folder.Text,"play"); if(Directory.Exists(path)) Process.Start("explorer.exe","\""+path+"\""); })); details.Controls.Add(utilities);
        output.Multiline = true; output.ReadOnly = true; output.ScrollBars = ScrollBars.Vertical; output.Dock = DockStyle.Fill;
        output.BackColor = Color.FromArgb(14,20,30); output.ForeColor = Color.Gainsboro; details.Controls.Add(output);
        repo.TextChanged += (sender,e) => files.Items.Clear(); folder.TextChanged += (sender,e) => RefreshState();
        language.SelectedIndexChanged += (sender,e) => { UiText.Language=languageCodes[language.SelectedIndex]; ApplyLanguage(this); SaveSettings(); };
        loadingSettings = false; ApplyLanguage(this);
    }
    void ShowAdvanced() {
        using(var dialog=new Form { Text=UiText.T("고급 메뉴"), ClientSize=new Size(920,600), MinimumSize=new Size(900,600), StartPosition=FormStartPosition.CenterParent, Font=Font, BackColor=BackColor, ForeColor=ForeColor, Padding=new Padding(16) }) {
            advanced.Visible=true; dialog.Controls.Add(advanced); ApplyLanguage(advanced);
            dialog.FormClosing += (sender,e) => { if(actions.Any(button => !button.Enabled) && !updatingSelf) e.Cancel=true; };
            dialog.ShowDialog(this); dialog.Controls.Remove(advanced); advanced.Visible=false;
        }
    }
    void SaveSettings() {
        if(loadingSettings) return;
        try { File.WriteAllText(config,new JavaScriptSerializer().Serialize(new { folder=folder.Text, repository=repo.Text, ui_language=UiText.Language, game_language=gameLocale, iso=isoPath }),new UTF8Encoding(false)); } catch { }
    }
    void ApplyLanguage(Control parent) {
        TranslateControls(parent);
        string family=UiText.Language=="ja" ? "Yu Gothic UI" : UiText.Language=="en" ? "Segoe UI" : "맑은 고딕";
        if(Font.FontFamily.Name!=family) Font=new Font(family,10);
        guide.Text=UiText.Guide; notice.Text=UiText.T("준비되었습니다. 위 안내에 따라 시작하세요.");
        for(int i=0;i<languageButtons.Count;i++) languageButtons[i].BackColor=languageCodes[i]==UiText.Language ? Color.FromArgb(35,105,160) : Color.FromArgb(38,55,76);
        for(int i=0;i<gameButtons.Count;i++) gameButtons[i].BackColor=GameLanguage.Codes[i]==gameLocale ? Color.FromArgb(35,105,160) : Color.FromArgb(38,55,76);
        string[] columns={"버전","파일 이름","크기","설치 버전 비교"};
        for(int i=0;i<files.Columns.Count;i++) files.Columns[i].Text=UiText.T(columns[i]);
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
    Control Field(string label, TextBox field, string button, Action click) {
        var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3 };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120)); panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100)); panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, button == null ? 0 : 76));
        panel.Controls.Add(new Label { Tag = label, Text = UiText.T(label), Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }); field.Dock = DockStyle.Fill; panel.Controls.Add(field);
        if (button != null) { var browse = Button(button, click); browse.AutoSize = false; browse.MinimumSize = Size.Empty; browse.Width = 70; browse.Height = 30; browse.Dock = DockStyle.None; browse.Margin = new Padding(2, 0, 0, 0); panel.Controls.Add(browse); } return panel;
    }
    Button Button(string label, Action click) {
        var b = new ReadableButton { Tag = label, Text = UiText.T(label), AutoSize = true, Height = 38, MinimumSize = new Size(120, 38), FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(38, 55, 76), ForeColor = Color.White, Margin = new Padding(0, 4, 12, 4) };
        b.Click += (s,e) => click(); actions.Add(b); return b;
    }
    void Log(string message) { if (InvokeRequired) { BeginInvoke(new Action<string>(Log), message); return; } string text=UiText.T(message); notice.Text=text; output.AppendText(DateTime.Now.ToString("HH:mm:ss") + "  " + text + Environment.NewLine); }
    void RefreshState() {
        try {
            string installed = Packages.InstalledVersion(folder.Text);
            state.Text = UiText.F("설치 버전: {0} · {1}", UiText.T(installed), UiText.T(LocalBuild.Ready(folder.Text,gameLocale) ? "실행 준비됨" : "적용을 눌러주세요"));
            foreach (ListViewItem row in files.Items) {
                var release = (ReleaseFiles)row.Tag; row.SubItems[3].Text = UiText.T(release.ComparedTo(installed));
                row.ForeColor = release.CanDownload(installed) ? Color.Gainsboro : Color.Gray;
            }
        }
        catch { state.Text = UiText.T("설치 정보를 읽을 수 없습니다. 폴더를 확인하세요."); }
    }
    async Task Busy(Func<Task> operation) {
        foreach (var b in actions) b.Enabled = false; files.Enabled = folder.Enabled = repo.Enabled = token.Enabled = language.Enabled = false; notice.Text=UiText.T("작업 중입니다. 잠시 기다려주세요.");
        try { await operation(); } catch (WebException ex) {
            var response = ex.Response as HttpWebResponse;
            if (response != null && response.StatusCode == HttpStatusCode.NotFound)
                Log(UiText.T("접근 가능한 GitHub 릴리스가 없습니다. 저장소·토큰을 확인하거나 로컬 패치 적용을 사용하세요."));
            else Log(UiText.T("다운로드 실패: ") + ex.Message);
            MessageBox.Show(this,notice.Text,UiText.T("설치 실패"),MessageBoxButtons.OK,MessageBoxIcon.Error);
        } catch (Exception ex) { Log(UiText.T("실패: ") + UiText.T(ex.Message)); if(!ex.Data.Contains("InstallProgressShown")) MessageBox.Show(this,ex.ToString(),UiText.T("설치 실패"),MessageBoxButtons.OK,MessageBoxIcon.Error); }
        finally { if (!updatingSelf) { foreach (var b in actions) b.Enabled = true; files.Enabled = folder.Enabled = repo.Enabled = token.Enabled = language.Enabled = true; RefreshState(); } }
    }
    Downloads Client(bool binary) {
        var c = new Downloads(); c.Encoding = Encoding.UTF8;
        c.Headers["User-Agent"] = "Macross30-Launcher/" + SelfUpdate.Version;
        c.Headers["Accept"] = binary ? "application/octet-stream" : "application/vnd.github+json";
        if (!String.IsNullOrWhiteSpace(token.Text)) c.Headers["Authorization"] = "Bearer " + token.Text.Trim();
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
        using (var deadline = new CancellationTokenSource(120000))
        using (deadline.Token.Register(() => client.CancelAsync())) {
            try { await task; }
            catch (OperationCanceledException) { throw new TimeoutException(UiText.T("다운로드가 취소되었거나 제한 시간(2분)을 초과했습니다.")); }
        }
    }
    public static bool NeedsPackageUpdate(string installed, string available) {
        return installed=="설치 안 됨" || installed=="이전 배포본" || Packages.VersionOf(installed)<Packages.VersionOf(available);
    }
    async Task InstallWizard() {
        SaveSettings();
        if(gameLocale=="en-US") throw new InvalidOperationException(UiText.T("선택한 게임 언어팩은 아직 준비 중입니다. 다른 게임 언어를 선택하세요."));
        string source=repo.Text.Trim();
        if(!Regex.IsMatch(source,@"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")) throw new InvalidDataException(UiText.T("저장소 형식을 확인하세요."));
        Log(UiText.T("사용자 원본 기반 로컬 빌드 패키지를 확인하고 있습니다…"));
        Dictionary<string,object> manifest=null;Dictionary<string,string> urls=null;
        using(var client=Client(false)) {
            string raw=await Timed(client,client.DownloadStringTaskAsync("https://api.github.com/repos/"+source+"/releases?per_page=100"));
            foreach(var value in Packages.Array(new JavaScriptSerializer().DeserializeObject(raw))) {
                var release=Packages.Object(value);if(Convert.ToBoolean(release["draft"]) || Convert.ToBoolean(release["prerelease"])) continue;
                var assets=Packages.Array(release["assets"]).Select(Packages.Object).ToList();
                var info=assets.FirstOrDefault(a => Packages.Text(a,"name")=="LOCAL-BUILD.json");if(info==null) continue;
                var candidateUrls=new Dictionary<string,string>(StringComparer.Ordinal);
                foreach(var asset in assets) {
                    string url=Packages.Text(asset,"url");
                    if(!url.StartsWith("https://api.github.com/repos/"+source+"/releases/assets/",StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Local build asset origin mismatch");
                    candidateUrls.Add(Packages.Text(asset,"name"),url);
                }
                Dictionary<string,object> candidate;
                using(var data=Client(true)) candidate=Packages.Read(await Timed(data,data.DownloadStringTaskAsync(candidateUrls["LOCAL-BUILD.json"])));
                LocalBuild.Validate(candidate);
                if(manifest==null || Packages.VersionOf(Packages.Text(candidate,"version"))>Packages.VersionOf(Packages.Text(manifest,"version"))) {manifest=candidate;urls=candidateUrls;}
            }
        }
        if(manifest==null) throw new InvalidOperationException(UiText.T("새 로컬 빌드 패키지는 준비 중입니다. 완성된 게임 실행 파일은 대신 다운로드하지 않습니다."));
        if(LocalBuild.Ready(folder.Text,gameLocale) && Packages.VersionOf(Packages.InstalledVersion(folder.Text))>=Packages.VersionOf(Packages.Text(manifest,"version"))) {Log(UiText.T("이 PC에서 생성한 게임 파일이 준비되어 있습니다."));return;}
        if(!File.Exists(isoPath)) using(var dialog=new OpenFileDialog { Filter=UiText.T("원본 디스크 이미지 (*.iso)|*.iso"),Title=UiText.T("본인 소유의 Macross 30 ISO 선택") }) {
            if(dialog.ShowDialog()!=DialogResult.OK) return;isoPath=dialog.FileName;SaveSettings();
        }
        string root=Path.GetFullPath(folder.Text);Directory.CreateDirectory(root);Packages.EnsureStopped(root);Packages.NoLinks(root,".local-build");
        var rows=Packages.Object(manifest["files"]);
        string work=Path.Combine(root,".local-build","bundle-"+Packages.Text(manifest,"version")+"-"+Packages.Text(Packages.Object(rows["source"]),"sha256").Substring(0,12));Directory.CreateDirectory(work);
        var parts=new Dictionary<string,string>();
        foreach(string kind in new[]{"source","data","third_party"}) {
            var row=Packages.Object(rows[kind]);string name=Packages.Text(row,"file");
            if(!urls.ContainsKey(name)) throw new InvalidDataException("Local build asset missing");
            string path=Path.Combine(work,name);Log(name+UiText.T(" 다운로드 중…"));
            using(var client=Client(true)) {
                long size=Convert.ToInt64(row["bytes"]);client.DownloadProgressChanged+=(sender,e)=>{if(e.BytesReceived>size)client.CancelAsync();};
                await Timed(client,client.DownloadFileTaskAsync(urls[name],path));
            }
            parts[kind]=await Task.Run(()=>LocalBuild.Extract(work,kind,path,row));
        }
        await RunLocalBuild(root,parts["source"],parts["data"],parts["third_party"]);
    }
    async Task RunLocalBuild(string root,string source,string payload,string runtime) {
        string script=Path.Combine(source,"tools/local_build/install.ps1");
        if(!File.Exists(script)) throw new InvalidDataException("Local installer source missing");
        Log(UiText.T("처음 설치는 도구 준비와 코드 생성·컴파일에 시간이 걸립니다. 진행 창을 닫지 마세요."));
        Packages.NoLinks(root,"play/runner.lock");Directory.CreateDirectory(Path.Combine(root,"play"));
        using(var lease=new FileStream(Path.Combine(root,"play/runner.lock"),FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.None)) {
            string arguments="-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+script+"\" -Root \""+root+"\" -Iso \""+isoPath+"\" -Payload \""+payload+"\" -Runtime \""+runtime+"\" -Language "+gameLocale;
            var start=new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),"WindowsPowerShell/v1.0/powershell.exe"),arguments) { UseShellExecute=false,CreateNoWindow=true,WorkingDirectory=source,RedirectStandardOutput=true,RedirectStandardError=true };
            string logFile=Path.Combine(root,"play/local-build.log");
            using(var progress=new InstallProgress(logFile,source)) {
                await progress.RunAsync(async ()=> {
                    using(var process=new Process {StartInfo=start})
                    using(var writer=new StreamWriter(new FileStream(logFile,FileMode.Create,FileAccess.Write,FileShare.Read),new UTF8Encoding(false))) {
                        var sync=new object();var clock=Stopwatch.StartNew();long lastFlush=0,lastDisplay=0;
                        DataReceivedEventHandler receive=(sender,e)=> {
                            if(e.Data==null)return;
                            lock(sync) {
                                writer.WriteLine(e.Data);
                                bool important=e.Data.StartsWith("LOCAL_",StringComparison.Ordinal) || e.Data.StartsWith("LOCAL GENERATE:",StringComparison.Ordinal) || Regex.IsMatch(e.Data,@"^\[\d+/\d+\]|(?:lifted|emitting)\s+\d+/\d+");
                                if(important || clock.ElapsedMilliseconds-lastFlush>=1000){writer.Flush();lastFlush=clock.ElapsedMilliseconds;}
                                if(important || clock.ElapsedMilliseconds-lastDisplay>=1000){progress.Receive(e.Data);lastDisplay=clock.ElapsedMilliseconds;}
                            }
                        };
                        process.OutputDataReceived+=receive;process.ErrorDataReceived+=receive;
                        process.Start();process.BeginOutputReadLine();process.BeginErrorReadLine();await Task.Run(()=>process.WaitForExit());
                        writer.Flush();
                        if(process.ExitCode!=0) throw new IOException(UiText.T("로컬 생성 또는 빌드에 실패했습니다. 고급 메뉴의 로그 폴더에서 local-build.log를 확인하세요.")+" ("+process.ExitCode+")");
                    }
                    if(!LocalBuild.Ready(root,gameLocale)) throw new IOException("Local generation completion verification failed");
                },this);
            }
        }
        if(!LocalBuild.Ready(root,gameLocale)) throw new IOException("Local generation completion verification failed");
        Log(UiText.T("이 PC에서 생성·컴파일한 게임 설치가 완료되었습니다. 게임 실행을 누르세요."));
    }
    async Task UpdateLocal() {
        using(var dialog=new FolderBrowserDialog { Description=UiText.T("설치 파일을 받은 폴더를 선택하세요") }) {
            if(dialog.ShowDialog()!=DialogResult.OK)return;
            string path=Path.Combine(dialog.SelectedPath,"LOCAL-BUILD.json");
            if(!File.Exists(path)) throw new InvalidDataException(UiText.T("사용자 원본 기반 로컬 빌드 패키지만 지원합니다."));
            var manifest=Packages.Read(File.ReadAllText(path));LocalBuild.Validate(manifest);
            if(gameLocale=="en-US") throw new InvalidOperationException(UiText.T("선택한 게임 언어팩은 아직 준비 중입니다. 다른 게임 언어를 선택하세요."));
            if(!File.Exists(isoPath)) using(var iso=new OpenFileDialog {Filter=UiText.T("원본 디스크 이미지 (*.iso)|*.iso")}) {if(iso.ShowDialog()!=DialogResult.OK)return;isoPath=iso.FileName;SaveSettings();}
            string root=Path.GetFullPath(folder.Text);Directory.CreateDirectory(root);Packages.EnsureStopped(root);Packages.NoLinks(root,".local-build");
            string work=Path.Combine(root,".local-build","work-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(work);
            var parts=new Dictionary<string,string>();foreach(string kind in new[]{"source","data","third_party"}) {
                var row=Packages.Object(Packages.Object(manifest["files"])[kind]);parts[kind]=await Task.Run(()=>LocalBuild.Extract(work,kind,Path.Combine(dialog.SelectedPath,Packages.Text(row,"file")),row));
            }
            await RunLocalBuild(root,parts["source"],parts["data"],parts["third_party"]);
        }
    }
    async Task UpdateSelf() {
        string source = repo.Text.Trim();
        if (!Regex.IsMatch(source,@"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")) throw new InvalidDataException(UiText.T("저장소 형식을 확인하세요."));
        Dictionary<string,object> best = null; string url = null;
        using (var client = Client(false)) {
            string raw = await Timed(client,client.DownloadStringTaskAsync("https://api.github.com/repos/" + source + "/releases?per_page=100"));
            foreach (var value in Packages.Array(new JavaScriptSerializer().DeserializeObject(raw))) {
                var release = Packages.Object(value);
                if (Convert.ToBoolean(release["draft"]) || Convert.ToBoolean(release["prerelease"])) continue;
                var assets = Packages.Array(release["assets"]).Select(Packages.Object).ToList();
                var info = assets.FirstOrDefault(a => Packages.Text(a,"name") == "LAUNCHER.json");
                if (info == null) continue;
                string infoUrl = Packages.Text(info,"url");
                if (!infoUrl.StartsWith("https://api.github.com/repos/" + source + "/releases/assets/",StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException(UiText.T("런처 자산 주소가 저장소와 다릅니다."));
                Dictionary<string,object> manifest;
                using (var data = Client(true)) manifest = Packages.Read(await Timed(data,data.DownloadStringTaskAsync(infoUrl)));
                SelfUpdate.ValidateManifest(manifest);
                var asset = assets.FirstOrDefault(a => Packages.Text(a,"name") == Packages.Text(manifest,"file"));
                if (asset == null || Convert.ToInt64(asset["size"]) != Convert.ToInt64(manifest["bytes"])) throw new InvalidDataException(UiText.T("런처 파일 또는 크기 정보가 맞지 않습니다."));
                string fileUrl = Packages.Text(asset,"url");
                if (!fileUrl.StartsWith("https://api.github.com/repos/" + source + "/releases/assets/",StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException(UiText.T("런처 다운로드 주소가 저장소와 다릅니다."));
                if (best == null || Packages.VersionOf(Packages.Text(manifest,"version")) > Packages.VersionOf(Packages.Text(best,"version"))) { best = manifest; url = fileUrl; }
            }
        }
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
        string root = Path.GetFullPath(folder.Text);
        if (!LocalBuild.Ready(root,gameLocale)) throw new InvalidOperationException(UiText.T("동일 버전의 두 팩을 적용하고 ISO 설치를 완료하세요."));
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
        if (!updatingSelf && actions.Any(b => !b.Enabled)) { e.Cancel = true; Log(UiText.T("현재 작업이 끝난 뒤 런처를 닫을 수 있습니다.")); }
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
        using (var form = new LauncherWindow()) {
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
