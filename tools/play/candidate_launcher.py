"""Pre-launch settings for an explicitly pinned private game candidate.

The profile supplies the existing private runner, candidate, and local shader
cache. Opening this window never starts or controls the game.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import ttk, messagebox

SCALES = {
    '1280 × 720': '1', '1600 × 900': '1.25', '1920 × 1080': '1.5',
    '2560 × 1440': '2', '3200 × 1800': '2.5', '3840 × 2160 (4K)': '3',
}
DEFAULTS = dict(render_scale='3', fullscreen=False, vsync=True,
                subtitles=True, hd_textures=True, prepare_shaders=True)
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def load_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def validate_options(value):
    value = {key: item for key, item in value.items() if key != 'bloom'}
    if set(value) != set(DEFAULTS) or value['render_scale'] not in SCALES.values():
        raise ValueError('Invalid candidate settings')
    if any(type(value[key]) is not bool for key in DEFAULTS if key != 'render_scale'):
        raise ValueError('Invalid checkbox setting')
    return dict(value)


def save_options(path, value):
    value = validate_options(value)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_suffix('.pending')
    pending.write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    os.replace(pending, path)


def verify_candidate(profile):
    exe = Path(profile['candidate']) / 'Macross30Recompiled.exe'
    with exe.open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    if actual != profile['exe_sha256']:
        raise ValueError('통합본 실행 파일이 변경됐습니다. 후보 파일을 확인해 주세요.')
    for name in ('runner', 'python'):
        if not Path(profile[name]).is_file():
            raise FileNotFoundError(profile[name])
    resolve_game_elf(profile)


def resolve_game_elf(profile):
    """Select the reviewed translation input, never an older measurement ELF."""
    path = Path(profile['game_elf']).resolve(strict=True)
    with path.open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    if actual != profile['game_elf_sha256']:
        raise ValueError('번역 파일 해시가 다릅니다. 실행 입력을 확인해 주세요.')
    return path


def ensure_stopped():
    result = subprocess.run(
        ['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
         '@(Get-Process -Name Macross30Recompiled -ErrorAction SilentlyContinue).Count'],
        capture_output=True, text=True, creationflags=NO_WINDOW, check=True)
    if int(result.stdout.strip()) != 0:
        raise RuntimeError('게임이 실행 중입니다. 직접 종료한 후 다시 눌러 주세요.')


def launch_command(profile):
    return [profile['python'], profile['runner'], 'run',
            '--candidate', profile['candidate'], '--label', 'user-configured',
            '--cache-dir', profile['cache_dir'], '--options', profile['settings_file'], '--manual']


def prepare(profile, emit):
    tool = Path(profile['candidate']) / 'MacrossShaderPrep.exe'
    cache = Path(profile['cache_dir'])
    cache.mkdir(parents=True, exist_ok=True)
    process = subprocess.Popen(
        [str(tool), '--cache-dir', str(cache), '--max-seconds', '180'],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        encoding='utf-8', errors='replace', creationflags=NO_WINDOW)
    events = []
    for line in process.stdout:
        try:
            event = json.loads(line)
        except ValueError:
            continue
        events.append(event)
        if event.get('event') in ('progress', 'checkpoint', 'done'):
            emit(f"셰이더 준비 {event.get('processed', 0)} / {event.get('total', 0)}")
    code = process.wait()
    process.stdout.close()
    (cache.parent/'manual-launcher-prep.json').write_text(
        json.dumps(dict(exit_code=code, events=events), indent=2)+'\n', encoding='utf-8')
    if code:
        raise RuntimeError(f'셰이더 준비를 완료하지 못했습니다. (코드 {code})')
    last = events[-1] if events else {}
    return f"준비 완료: {last.get('prepared', '?')} / {last.get('total', '?')}개 파이프라인"


class Window(tk.Tk):
    def __init__(self, profile):
        super().__init__()
        self.profile = profile
        self.events = queue.Queue()
        self.busy = False
        self.title('Macross 30 통합 테스트 · 실행 전 설정')
        self.geometry('620x700')
        self.resizable(False, False)
        self.option_add('*Font', ('Malgun Gothic', 11))
        settings = Path(profile['settings_file'])
        values = validate_options(load_json(settings)) if settings.exists() else dict(DEFAULTS)
        self.variables = {key: tk.BooleanVar(value=values[key]) for key in DEFAULTS if key != 'render_scale'}
        self.resolution = tk.StringVar(value=next(label for label, scale in SCALES.items()
                                                if scale == values['render_scale']))
        body = ttk.Frame(self, padding=24)
        body.pack(fill='both', expand=True)
        ttk.Label(body, text='통합본 설정 후 직접 테스트', font=('Malgun Gothic', 16, 'bold')).pack(anchor='w')
        ttk.Label(body, text='HD 텍스처 · 셰이더 사전 준비 · 노출/톤 처리 수정',
                  wraplength=560).pack(anchor='w', pady=(8, 20))
        row = ttk.Frame(body)
        row.pack(fill='x', pady=6)
        ttk.Label(row, text='렌더 해상도').pack(side='left')
        resolution_control = ttk.Combobox(row, textvariable=self.resolution, values=list(SCALES),
                                         state='readonly', width=27)
        resolution_control.pack(side='right')
        self.editors = [(resolution_control, 'readonly')]
        for key, text in (('fullscreen', '전체화면'), ('vsync', '수직 동기화 (VSync)'),
                          ('subtitles', '자막 표시'),
                          ('hd_textures', 'HD 텍스처 사용'),
                          ('prepare_shaders', '게임 실행 전에 셰이더·파이프라인 준비')):
            control = ttk.Checkbutton(body, text=text, variable=self.variables[key])
            control.pack(anchor='w', pady=6)
            self.editors.append((control, 'normal'))
        ttk.Label(body, text='30 FPS 고정 · 제한 해제는 수정 중', foreground='#666666').pack(anchor='w', pady=8)
        ttk.Label(body, text='새 설정은 다음 실행부터 적용됩니다.\n'
                  '처음 만나는 셰이더는 게임 중 추가 컴파일될 수 있습니다.\n'
                  '자동 조작·자동 측정 없이 직접 플레이합니다.', wraplength=560).pack(anchor='w', pady=10)
        self.status = tk.StringVar(value='설정을 선택하고 실행을 눌러 주세요.')
        ttk.Label(body, textvariable=self.status, wraplength=560, foreground='#225588').pack(anchor='w', pady=10)
        row = ttk.Frame(body)
        row.pack(fill='x', pady=8)
        self.save_button = ttk.Button(row, text='설정 저장', command=self.save)
        self.save_button.pack(side='left')
        self.prepare_button = ttk.Button(row, text='셰이더만 준비', command=lambda: self.work(False))
        self.prepare_button.pack(side='left', padx=8)
        self.play_button = ttk.Button(row, text='설정 적용 후 게임 실행', command=lambda: self.work(True))
        self.play_button.pack(side='right')
        self.protocol('WM_DELETE_WINDOW', self.close)
        self.after(100, self.drain)
        self.after_idle(self.lift)

    def values(self):
        return validate_options(dict(render_scale=SCALES[self.resolution.get()],
                                     **{key: value.get() for key, value in self.variables.items()}))

    def save(self):
        try:
            save_options(self.profile['settings_file'], self.values())
            self.status.set('저장했습니다. 다음 실행부터 적용됩니다.')
            return True
        except (OSError, ValueError) as error:
            messagebox.showerror('설정 저장', str(error), parent=self)
            return False

    def work(self, launch):
        if self.busy or not self.save():
            return
        options = self.values()
        self.busy = True
        for control, _ in self.editors:
            control.configure(state='disabled')
        for button in (self.save_button, self.prepare_button, self.play_button):
            button.configure(state='disabled')
        self.status.set('실행 상태 확인 중…')

        def task():
            try:
                ensure_stopped()
                verify_candidate(self.profile)
                if not launch or options['prepare_shaders']:
                    summary = prepare(self.profile, lambda text: self.events.put(('status', text)))
                    self.events.put(('status', summary))
                if launch:
                    ensure_stopped()
                    log = Path(self.profile['settings_file']).parent / 'manual-launcher-run.log'
                    self.events.put(('status', '게임 실행 중 · 직접 테스트하세요. 종료 후 다시 설정할 수 있습니다.'))
                    with log.open('wb') as output:
                        result = subprocess.run(launch_command(self.profile), stdout=output, stderr=subprocess.STDOUT,
                                                creationflags=NO_WINDOW)
                    if result.returncode:
                        raise RuntimeError('게임을 시작하지 못했거나 실행 오류가 발생했습니다.\n'+str(log))
                    self.events.put(('status', '게임이 종료됐습니다. 설정을 바꿔 다시 실행할 수 있습니다.'))
            except Exception as error:
                self.events.put(('error', str(error)))
            finally:
                self.events.put(('idle', None))
        threading.Thread(target=task, daemon=True).start()

    def drain(self):
        while not self.events.empty():
            kind, value = self.events.get_nowait()
            if kind == 'idle':
                self.busy = False
                for control, state in self.editors:
                    control.configure(state=state)
                for button in (self.save_button, self.prepare_button, self.play_button):
                    button.configure(state='normal')
            else:
                self.status.set(value)
        self.after(100, self.drain)

    def close(self):
        if self.busy:
            self.status.set('준비 또는 게임 실행 중입니다. 게임을 직접 종료한 뒤 이 창을 닫아 주세요.')
        else:
            self.destroy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--check', action='store_true', help='Validate inputs without opening a window or game')
    args = parser.parse_args()
    profile = load_json(args.profile)
    verify_candidate(profile)
    if args.check:
        settings = Path(profile['settings_file'])
        options = validate_options(load_json(settings)) if settings.exists() else dict(DEFAULTS)
        print(json.dumps(dict(ok=True, options=options, command=launch_command(profile),
                              game_elf=str(resolve_game_elf(profile)),
                              game_elf_sha256=profile['game_elf_sha256']), indent=2))
        return
    Window(profile).mainloop()


if __name__ == '__main__':
    main()
