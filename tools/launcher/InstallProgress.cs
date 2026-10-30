using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace MacrossLauncher {
public sealed class ReadableButton : Button {
    public ReadableButton() { SetStyle(ControlStyles.UserPaint|ControlStyles.OptimizedDoubleBuffer|ControlStyles.AllPaintingInWmPaint,true); }
    protected override void OnPaint(PaintEventArgs e) {
        Color background=Enabled ? BackColor : Color.FromArgb(40,52,68);
        Color foreground=Enabled ? ForeColor : Color.FromArgb(220,228,238);
        using(var brush=new SolidBrush(background)) e.Graphics.FillRectangle(brush,ClientRectangle);
        using(var pen=new Pen(Enabled ? Color.FromArgb(160,190,220) : Color.FromArgb(110,132,155))) e.Graphics.DrawRectangle(pen,0,0,Width-1,Height-1);
        TextRenderer.DrawText(e.Graphics,Text,Font,ClientRectangle,foreground,TextFormatFlags.HorizontalCenter|TextFormatFlags.VerticalCenter|TextFormatFlags.EndEllipsis);
        if(Focused && ShowFocusCues && Enabled) ControlPaint.DrawFocusRectangle(e.Graphics,Rectangle.Inflate(ClientRectangle,-4,-4),foreground,background);
    }
}

public sealed class InstallProgress : Form {
    readonly Label stage=new Label(), elapsed=new Label(), steps=new Label();
    readonly TextBox tail=new TextBox();
    readonly ReadableButton close=new ReadableButton();
    readonly ProgressBar bar=new ProgressBar();
    readonly System.Windows.Forms.Timer timer=new System.Windows.Forms.Timer();
    readonly Stopwatch watch=Stopwatch.StartNew();
    readonly string logPath;
    bool finished;
    int phase=1;
    string detail="";
    readonly string[] names={"도구 준비","ISO 원본 준비","PPU·SPU 코드 생성","수정 사항 적용","실행 파일 컴파일","게임 데이터·한글 설치","최종 검증"};
    public InstallProgress(string path) {
        logPath=path;Text=UiText.T("게임 설치 진행");ClientSize=new Size(880,650);MinimumSize=new Size(800,600);
        StartPosition=FormStartPosition.CenterParent;Font=new Font("맑은 고딕",10);BackColor=Color.FromArgb(22,29,42);ForeColor=Color.WhiteSmoke;
        MinimizeBox=false;MaximizeBox=false;
        var layout=new TableLayoutPanel {Dock=DockStyle.Fill,Padding=new Padding(20),ColumnCount=1,RowCount=6};
        foreach(int size in new[]{64,32,34,124})layout.RowStyles.Add(new RowStyle(SizeType.Absolute,size));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent,100));layout.RowStyles.Add(new RowStyle(SizeType.Absolute,48));
        stage.Dock=elapsed.Dock=steps.Dock=bar.Dock=tail.Dock=DockStyle.Fill;
        stage.Font=new Font(Font.FontFamily,13,FontStyle.Bold);stage.ForeColor=Color.LightSkyBlue;bar.Style=ProgressBarStyle.Marquee;
        tail.Multiline=true;tail.ReadOnly=true;tail.ScrollBars=ScrollBars.Both;tail.WordWrap=false;tail.BackColor=Color.FromArgb(12,18,28);tail.ForeColor=Color.FromArgb(230,236,245);
        layout.Controls.Add(stage);layout.Controls.Add(elapsed);layout.Controls.Add(bar);layout.Controls.Add(steps);layout.Controls.Add(tail);
        var buttons=new FlowLayoutPanel {Dock=DockStyle.Fill};
        close.Text=UiText.T("닫기");close.Enabled=false;close.Size=new Size(110,36);close.ForeColor=Color.White;close.BackColor=Color.FromArgb(38,55,76);close.Click+=(s,e)=>Close();
        var logs=new ReadableButton {Text=UiText.T("전체 로그 열기"),Size=new Size(160,36),ForeColor=Color.White,BackColor=Color.FromArgb(38,55,76)};
        logs.Click+=(s,e)=>{if(File.Exists(logPath))Process.Start(new ProcessStartInfo("notepad.exe","\""+logPath+"\""){UseShellExecute=false});};
        buttons.Controls.Add(close);buttons.Controls.Add(logs);layout.Controls.Add(buttons);Controls.Add(layout);
        timer.Interval=1000;timer.Tick+=(s,e)=>RefreshStatus();timer.Start();RefreshStatus();
        FormClosing+=(s,e)=>{if(!finished)e.Cancel=true;};
    }
    void RefreshStatus() {
        elapsed.Text=UiText.F("경과 시간: {0}분 {1}초 · 설치가 끝나면 게임 실행 버튼이 활성화됩니다.",(int)watch.Elapsed.TotalMinutes,watch.Elapsed.Seconds);
        if(finished)return;
        stage.Text=UiText.F("{0}/7 · {1}",phase,UiText.T(names[phase-1]))+(detail.Length==0 ? "" : "\r\n"+detail);
        string summary="";for(int i=0;i<names.Length;i++)summary+=(i+1==phase ? "▶ " : i+1<phase ? "✓ " : "· ")+UiText.T(names[i])+(i%2==0 ? "     " : "\r\n");steps.Text=summary;
    }
    public void Receive(string line) {
        if(IsDisposed || Disposing)return;
        if(InvokeRequired){BeginInvoke(new Action<string>(Receive),line);return;}
        if(line.StartsWith("LOCAL_SETUP"))phase=1;
        else if(line.StartsWith("LOCAL_ORIGINAL"))phase=2;
        else if(line.StartsWith("LOCAL_GENERATE") || line.StartsWith("LOCAL GENERATE:")) {
            phase=line.Contains("patch_") || line.Contains("apply_hooks") || line.Contains("posttask.py") ? 4 : 3;
            detail=UiText.T(line.Contains("ppu_lifter") ? "PPU 코드를 생성하고 있습니다. 시간이 걸릴 수 있습니다." : line.Contains("lift_") ? "SPU 코드를 생성하고 있습니다. 시간이 걸릴 수 있습니다." : "사용자 원본에서 게임 코드를 생성하고 있습니다…");
        } else if(Regex.IsMatch(line,@"^\[\d+/\d+\].*(Building|Linking)")){phase=5;detail=line;}
        else if(Regex.IsMatch(line,@"^\[\d+/\d+\].*data/")){phase=6;detail=line;}
        else if(line.StartsWith("LOCAL_BUILD_COMPLETE")){phase=6;detail="";}
        else if(line.StartsWith("LOCAL_GENERATION_INSTALLED") || line.StartsWith("LOCAL_INSTALL_COMPLETE")){phase=7;detail="";}
        if(tail.TextLength>100000)tail.Text=tail.Text.Substring(tail.TextLength-70000);
        tail.AppendText(line+Environment.NewLine);RefreshStatus();
    }
    public Task RunAsync(Func<Task> operation,IWin32Window owner) {
        var completion=new TaskCompletionSource<bool>();
        Shown+=async (sender,args)=>{
            try {await operation();finished=true;timer.Stop();completion.SetResult(true);Close();}
            catch(Exception error){error.Data["InstallProgressShown"]=true;finished=true;timer.Stop();elapsed.Text=UiText.F("경과 시간: {0}분 {1}초 · 설치를 완료하지 못했습니다.",(int)watch.Elapsed.TotalMinutes,watch.Elapsed.Seconds);bar.Style=ProgressBarStyle.Blocks;bar.Value=0;stage.ForeColor=Color.Salmon;stage.Text=UiText.T("설치 실패")+" · "+error.Message;steps.Text=UiText.T("아래 마지막 로그를 확인하세요. 전체 로그는 설치 폴더의 play/local-build.log에 저장됩니다.");try { if(File.Exists(logPath)) using(var stream=new FileStream(logPath,FileMode.Open,FileAccess.Read,FileShare.ReadWrite)) {if(stream.Length>64000)stream.Seek(-64000,SeekOrigin.End);using(var reader=new StreamReader(stream))tail.Text=reader.ReadToEnd();} } catch(IOException) {}
                tail.AppendText(Environment.NewLine+error.ToString());close.Enabled=true;completion.SetException(error);}
        };
        ShowDialog(owner);return completion.Task;
    }
    protected override void Dispose(bool disposing) {if(disposing)timer.Dispose();base.Dispose(disposing);}
}
}
