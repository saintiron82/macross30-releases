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
        Color background=Enabled ? BackColor : Color.FromArgb(BackColor.R*3/4,BackColor.G*3/4,BackColor.B*3/4);
        Color foreground=Enabled ? ForeColor : Color.FromArgb(220,228,238);
        using(var brush=new SolidBrush(background)) e.Graphics.FillRectangle(brush,ClientRectangle);
        int border=Enabled ? Math.Max(1,FlatAppearance.BorderSize) : 1;
        Color edge=Enabled && !FlatAppearance.BorderColor.IsEmpty ? FlatAppearance.BorderColor : Color.FromArgb(160,190,220);
        using(var pen=new Pen(edge,border)) e.Graphics.DrawRectangle(pen,border/2,border/2,Width-border,Height-border);
        TextRenderer.DrawText(e.Graphics,Text,Font,ClientRectangle,foreground,TextFormatFlags.HorizontalCenter|TextFormatFlags.VerticalCenter|TextFormatFlags.EndEllipsis);
        if(Focused && ShowFocusCues && Enabled) ControlPaint.DrawFocusRectangle(e.Graphics,Rectangle.Inflate(ClientRectangle,-4,-4),foreground,background);
    }
}

public sealed class InstallProgress : Form {
    readonly Label stage=new Label(), elapsed=new Label(), steps=new Label(), detailText=new Label(), progressText=new Label(), worklist=new Label();
    readonly string sourceRoot;
    readonly bool bootMode,languageMode;
    readonly DateTime generationStarted=DateTime.UtcNow;
    readonly string[] jobs={"notify","graphics","geom1","geom3","geom2","anim","jobcrt14","jobcrt14_b","jobcrt14_c","jobcrt14_d","geom6","geom7","geom4","geom5"};
    bool ppuDone; double phaseFraction,ppuLiftFraction,ppuEmitFraction; int overall;
    readonly TextBox tail=new TextBox();
    readonly ReadableButton close=new ReadableButton();
    readonly ProgressBar bar=new ProgressBar();
    readonly System.Windows.Forms.Timer timer=new System.Windows.Forms.Timer();
    readonly Stopwatch watch=Stopwatch.StartNew();
    readonly string logPath;
    bool finished;
    int phase=1;
    string detail="",currentWork="";
    readonly string[] names={"도구 준비","ISO 원본 준비","PPU·SPU 코드 생성","수정 사항 적용","실행 파일 컴파일","게임 데이터·한글 설치","최종 검증"};
    public InstallProgress(string path,string source=null,bool prebuilt=false,bool languageOnly=false) {
        sourceRoot=source;bootMode=prebuilt;languageMode=languageOnly;
        if(prebuilt)names=new[]{"도구 준비","ISO 원본 준비","실행 데이터 준비","공통 실행 파일 설치","원본 게임 데이터 준비","실행 환경 구성","게임 준비 검증"};
        if(languageOnly)names=new[]{"게임 준비 확인","원본 확인","언어 패치 준비","언어팩 설치","자막 설치","무결성 검증","완료"};
        logPath=path;Text=UiText.T(languageOnly?"언어팩 설치":"게임 설치 진행");ClientSize=new Size(880,680);MinimumSize=new Size(800,650);
        StartPosition=FormStartPosition.CenterParent;Font=new Font("맑은 고딕",10);BackColor=Color.FromArgb(22,29,42);ForeColor=Color.WhiteSmoke;
        MinimizeBox=false;MaximizeBox=false;
        var layout=new TableLayoutPanel {Dock=DockStyle.Fill,Padding=new Padding(20),ColumnCount=1,RowCount=8};
        foreach(int size in new[]{38,32,30,16,28,164})layout.RowStyles.Add(new RowStyle(SizeType.Absolute,size));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent,100));layout.RowStyles.Add(new RowStyle(SizeType.Absolute,48));
        stage.Dock=elapsed.Dock=steps.Dock=bar.Dock=tail.Dock=detailText.Dock=progressText.Dock=worklist.Dock=DockStyle.Fill;
        detailText.ForeColor=Color.Gainsboro;progressText.ForeColor=Color.LightSkyBlue;worklist.ForeColor=Color.Gainsboro;
        tail.Visible=false;
        stage.Font=new Font(Font.FontFamily,13,FontStyle.Bold);stage.ForeColor=Color.LightSkyBlue;bar.Style=ProgressBarStyle.Continuous;
        tail.Multiline=true;tail.ReadOnly=true;tail.ScrollBars=ScrollBars.Both;tail.WordWrap=false;tail.BackColor=Color.FromArgb(12,18,28);tail.ForeColor=Color.FromArgb(230,236,245);
        layout.Controls.Add(stage);layout.Controls.Add(detailText);layout.Controls.Add(elapsed);layout.Controls.Add(bar);layout.Controls.Add(progressText);layout.Controls.Add(steps);
        var body=new Panel {Dock=DockStyle.Fill};body.Controls.Add(worklist);body.Controls.Add(tail);layout.Controls.Add(body);
        var buttons=new FlowLayoutPanel {Dock=DockStyle.Fill};
        close.Text=UiText.T("닫기");close.Enabled=false;close.Size=new Size(110,36);close.ForeColor=Color.White;close.BackColor=Color.FromArgb(38,55,76);close.Click+=(s,e)=>Close();
        var logs=new ReadableButton {Text=UiText.T("전체 로그 열기"),Size=new Size(160,36),ForeColor=Color.White,BackColor=Color.FromArgb(38,55,76)};
        logs.Click+=(s,e)=>{if(File.Exists(logPath))Process.Start(new ProcessStartInfo("notepad.exe","\""+logPath+"\""){UseShellExecute=false});};
        var toggle=new ReadableButton {Text=UiText.T("상세 로그"),Size=new Size(130,36),ForeColor=Color.White,BackColor=Color.FromArgb(38,55,76)};
        toggle.Click+=(s,e)=>{tail.Visible=!tail.Visible;worklist.Visible=!tail.Visible;};
        buttons.Controls.Add(close);buttons.Controls.Add(toggle);buttons.Controls.Add(logs);layout.Controls.Add(buttons);Controls.Add(layout);
        timer.Interval=1000;timer.Tick+=(s,e)=>RefreshStatus();timer.Start();RefreshStatus();
        FormClosing+=(s,e)=>{if(!finished)e.Cancel=true;};
    }
    void RefreshStatus() {
        elapsed.Text=UiText.F("경과 시간: {0}분 {1}초 · 설치가 끝나면 게임 실행 버튼이 활성화됩니다.",(int)watch.Elapsed.TotalMinutes,watch.Elapsed.Seconds);
        if(finished)return;
        if(phase==3 && !bootMode)PollGeneration();
        stage.Text=UiText.F("{0}/7 · {1}",phase,UiText.T(names[phase-1]));detailText.Text=detail;
        overall=Math.Max(overall,Math.Min(99,(int)(100.0*(phase-1+phaseFraction)/7)));bar.Value=overall;
        progressText.Text=UiText.F("전체 진행률 {0}%",overall);
        if(phase!=3 || bootMode)worklist.Text=(currentWork.Length==0 ? "" : UiText.F("현재 작업: {0}",currentWork)+Environment.NewLine+Environment.NewLine)+UiText.T("진행 상황은 위에 표시됩니다. 기술 기록은 상세 로그에서 확인할 수 있습니다.");
        string summary="";for(int i=0;i<names.Length;i++)summary+=(i+1)+"/7  "+(i+1==phase ? "▶ " : i+1<phase ? "✓ " : "· ")+UiText.T(names[i])+Environment.NewLine;steps.Text=summary;
    }
    public void Receive(string line) {
        if(IsDisposed || Disposing)return;
        if(InvokeRequired){BeginInvoke(new Action<string>(Receive),line);return;}
        int previousPhase=phase;
        var boot=Regex.Match(line,@"^BOOT_STAGE ([1-7])/7\s*(.*)$");
        if(boot.Success){phase=Int32.Parse(boot.Groups[1].Value);phaseFraction=0;detail=UiText.T(names[phase-1]);}
        if(line.StartsWith("LOCAL_SETUP")){phase=1;detail=UiText.T("설치에 필요한 도구를 확인하고 준비합니다.");}
        else if(line.StartsWith("LOCAL_ORIGINAL")){phase=2;detail=UiText.T("선택한 ISO의 원본을 확인합니다.");}
        else if(line.StartsWith("LOCAL_GENERATE") || line.StartsWith("LOCAL GENERATE:")) {
            phase=line.Contains("patch_") || line.Contains("apply_hooks") || line.Contains("posttask.py") ? 4 : 3;
            if(line.Contains("lift_native_job.py"))ppuDone=true;
            detail=UiText.T(phase==4 ? "기존 성능 수정과 호환성 처리를 적용합니다." : ppuDone ? "SPU 프로그램을 생성합니다." : "PPU 프로그램을 생성합니다.");
        } else if(line.StartsWith("LOCAL_BUILD_COMPLETE")){phase=6;detail=UiText.T("게임 데이터와 언어 패치를 설치합니다.");}
        else if(line.StartsWith("LOCAL_GENERATION_INSTALLED") || line.StartsWith("LOCAL_INSTALL_COMPLETE")){phase=7;detail=UiText.T("설치한 파일의 무결성을 확인합니다.");}
        var count=Regex.Match(line,@"^\[(\d+)/(\d+)\]");
        if(count.Success) {
            int done,total;if(Int32.TryParse(count.Groups[1].Value,out done) && Int32.TryParse(count.Groups[2].Value,out total) && total>0) {
                if(line.Contains("Building") || line.Contains("Linking"))phase=5;
                else if(line.Contains("data/"))phase=languageMode?4:bootMode?5:6;
                if(phase==5 || phase==6 || languageMode&&phase==4) {
                    phaseFraction=Math.Min(1.0,(double)done/total);
                    detail=UiText.F("현재 단계 · {0}/{1}개 완료 ({2}%)",done,total,(int)(phaseFraction*100));
                    if(phase==5 && !bootMode)currentWork=UiText.T(line.Contains("Linking") ? "최종 실행 파일 만들기" : line.Contains("ppu_recomp") ? "게임의 PPU 처리 코드" : line.Contains("spu_") ? "게임의 SPU 처리 코드" : line.Contains("rsx_") || line.Contains("video") ? "그래픽 처리 구성 요소" : "실행에 필요한 구성 요소");
                    else currentWork=UiText.T(line.Contains("delta") ? "언어 패치 적용" : "ISO 데이터 설치");
                }
            }
        }
        var ppu=Regex.Match(line,@"(?:lifted|emitting)\s+(\d+)/(\d+)\s+functions");
        if(phase==3 && ppu.Success) {
            detail=UiText.F("PPU 함수 {0}/{1}개 처리",ppu.Groups[1].Value,ppu.Groups[2].Value);
            int processed,total;if(Int32.TryParse(ppu.Groups[1].Value,out processed) && Int32.TryParse(ppu.Groups[2].Value,out total) && total>0){if(line.Contains("emitting"))ppuEmitFraction=Math.Min(1.0,(double)processed/total);else ppuLiftFraction=Math.Min(1.0,(double)processed/total);}
        }
        if(previousPhase!=phase && !count.Success)phaseFraction=0;
        if(tail.TextLength>100000)tail.Text=tail.Text.Substring(tail.TextLength-70000);
        tail.AppendText(line+Environment.NewLine);RefreshStatus();
    }
    void PollGeneration() {
        if(String.IsNullOrEmpty(sourceRoot))return;
        try {
            int done=0;string text=UiText.T("SPU 프로그램별 진행")+Environment.NewLine;bool activeShown=false;
            for(int i=0;i<jobs.Length;i++) {
                string file=i==0 ? "native/macross30/spu_notify/report.json" : i==1 ? "native/macross30/spu_graphics/source.json" : "native/analysis/gameplay_job_audit_"+jobs[i]+".json";
                string path=Path.Combine(sourceRoot,file);bool complete=File.Exists(path) && File.GetLastWriteTimeUtc(path)>=generationStarted;
                if(i==0 && complete){string generated=Path.Combine(sourceRoot,"native/macross30/spu_notify/spu_recomp.c");complete=File.Exists(generated) && File.GetLastWriteTimeUtc(generated)>=File.GetLastWriteTimeUtc(path);}
                if(complete)done++;
                string status=complete ? "완료" : ppuDone && !activeShown ? "진행 중" : "대기";if(status=="진행 중")activeShown=true;
                text+=(i+1)+"/"+jobs.Length+"  "+jobs[i]+"  ·  "+UiText.T(status)+Environment.NewLine;
            }
            worklist.Text=text;
            phaseFraction=0.5*(ppuDone ? 1.0 : (ppuLiftFraction+ppuEmitFraction)/2)+0.5*done/jobs.Length;
            if(ppuDone){detail=UiText.F("SPU 프로그램 {0}/{1}개 완료",done,jobs.Length);}
        } catch(IOException) {} catch(UnauthorizedAccessException) {}
    }
    public Task RunAsync(Func<Task> operation,IWin32Window owner) {
        var completion=new TaskCompletionSource<bool>();
        Shown+=async (sender,args)=>{
            try {await operation();finished=true;timer.Stop();bar.Value=100;progressText.Text=UiText.F("전체 진행률 {0}%",100);await Task.Delay(400);completion.SetResult(true);Close();}
            catch(Exception error){error.Data["InstallProgressShown"]=true;finished=true;timer.Stop();elapsed.Text=UiText.F("경과 시간: {0}분 {1}초 · 설치를 완료하지 못했습니다.",(int)watch.Elapsed.TotalMinutes,watch.Elapsed.Seconds);bar.Style=ProgressBarStyle.Blocks;bar.Value=0;stage.ForeColor=Color.Salmon;var code=Regex.Match(error.Message,@"\((\d+)\)$");stage.Text=UiText.T("설치 실패")+(code.Success ? " · "+UiText.F("종료 코드 {0}",code.Groups[1].Value) : "");((TableLayoutPanel)steps.Parent).RowStyles[5].Height=48;steps.Text=UiText.T("아래 마지막 로그를 확인하세요. 전체 로그는 설치 폴더의 play/local-build.log에 저장됩니다.");try { if(File.Exists(logPath)) using(var stream=new FileStream(logPath,FileMode.Open,FileAccess.Read,FileShare.ReadWrite)) {if(stream.Length>64000)stream.Seek(-64000,SeekOrigin.End);using(var reader=new StreamReader(stream))tail.Text=reader.ReadToEnd();} } catch(IOException) {}
                worklist.Visible=false;tail.Visible=true;detailText.Text=UiText.T("전체 로그 열기로 자세한 내용을 확인하세요.");tail.AppendText(Environment.NewLine+error.ToString());close.Enabled=true;completion.SetException(error);}
        };
        ShowDialog(owner);return completion.Task;
    }
    protected override void Dispose(bool disposing) {if(disposing)timer.Dispose();base.Dispose(disposing);}
}
}
