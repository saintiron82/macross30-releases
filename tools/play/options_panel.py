"""런타임 옵션 조절 창.

게임(native/play 작업 폴더)이 약 1초마다 확인하는 토글 파일을 체크박스로
만들고 지운다. 게임을 재시작하지 않고 바로 반영된다.

실행: python tools/play/options_panel.py
"""
import tkinter as tk
from tkinter import ttk, filedialog
import queue
import threading
from pack_selection import PackSelection, pack_label, read_selection
from pathlib import Path

PLAY = Path(__file__).resolve().parents[2] / 'native' / 'play'

# (파일 이름, 체크 시 의미, 설명) -- 체크 = 파일 있음
OPTIONS = [
    ('pp_gpu', 'GPU 후처리 켜기', '밝기·톤커브·블룸 (원본 밝기). 기본 꺼짐.'),
    ('pp_nobloom', '블룸 빼기', 'GPU 후처리 중 블룸(빛 번짐)만 제외.'),
    ('spu_pp_run', 'SPU 후처리 강제 실행', 'GPU 후처리 중에도 SPU의 같은 작업을 그대로 수행.'),
    ('no_zeta_import', '물 반사 수정 끄기', '컬러→깊이/스텐실 변환을 끔 (반사 사라짐).'),
    ('zimp_alt', '반사 깊이 배치 B안', '깊이/스텐실 바이트 배치를 S8D24로 해석.'),
    ('blit_writeback', '메인 메모리 쓰기 (왕복)', 'GPU 장면을 메인 메모리에 써서 SPU에 전달. 느려짐.'),
    ('no_surface_writeback', '1x1 휘도 쓰기 끄기', '메인 메모리 렌더 타깃의 게임 메모리 반영을 끔.'),
    ('no_posttask_native', 'SPU 후처리 네이티브 끄기', 'posttask 플러그인을 원래 해석 경로로 실행.'),
    ('no_poly_offset', '폴리곤 오프셋 끄기', '그림자 깊이 바이어스 끔.'),
    ('depth_border_black', '그림자맵 테두리 검정', '그림자맵 밖을 예전처럼 그림자로 처리.'),
    ('no_gamma', 'sRGB 감마 끄기', '텍스처 감마 변환을 끔.'),
]



def main():
    root = tk.Tk()
    root.title('Macross 옵션 조절')
    root.attributes('-topmost', True)
    tk.Label(root, text=f'게임 작업 폴더: {PLAY}', anchor='w').pack(fill='x', padx=8, pady=(8, 4))
    vars_ = []
    language_frame = ttk.LabelFrame(root, text='게임 언어 (다음 실행부터 적용)')
    language_frame.pack(fill='x', padx=8, pady=6)
    controller = PackSelection(PLAY.parents[1])
    saved_locale, saved_partial = read_selection(PLAY / 'settings.ini')
    selected_language = tk.StringVar()
    language_combo = ttk.Combobox(language_frame, state='readonly', textvariable=selected_language, width=66)
    language_combo.pack(fill='x', padx=8, pady=4)
    partial = tk.BooleanVar(value=saved_partial)
    partial_check = ttk.Checkbutton(language_frame, text='부분 테스트 팩 허용 (미번역 부분은 팩의 대체 언어로 표시)', variable=partial)
    partial_check.pack(anchor='w', padx=8)
    language_status = tk.StringVar(value='설치된 언어팩 확인 중…')
    ttk.Label(language_frame, textvariable=language_status, wraplength=590).pack(anchor='w', padx=8, pady=4)
    buttons = ttk.Frame(language_frame)
    buttons.pack(fill='x', padx=8, pady=4)
    results = queue.Queue()
    entries = []
    busy = False

    def selected_pack():
        index = language_combo.current()
        return entries[index] if 0 <= index < len(entries) else None

    def selection_changed(_event=None):
        pack = selected_pack()
        if pack:
            language_status.set(str(pack.get('error') or ('부분 테스트 팩: 미번역 내용은 대체 언어로 표시됩니다.' if pack.get('status') == 'partial_test' else '선택한 언어를 저장하면 다음 실행부터 적용됩니다.')))

    def populate(preferred):
        nonlocal entries
        entries = controller.packs
        language_combo['values'] = [pack_label(pack) for pack in entries]
        index = next((i for i, pack in enumerate(entries) if pack.get('locale') == preferred), -1)
        if index >= 0:
            language_combo.current(index)
            selection_changed()
        else:
            selected_language.set('')
            language_status.set('저장된 언어팩을 찾을 수 없습니다: ' + preferred)

    def run(operation, message, preferred=None):
        nonlocal busy
        if busy:
            return
        busy = True
        language_status.set(message)
        language_combo.configure(state='disabled')
        for widget in (partial_check, save_button, import_button, refresh_button):
            widget.configure(state='disabled')
        def work():
            try:
                results.put((True, operation(), preferred))
            except Exception as error:
                results.put((False, str(error), preferred))
        threading.Thread(target=work, daemon=True).start()

    def poll():
        nonlocal busy
        try:
            success, result, preferred = results.get_nowait()
        except queue.Empty:
            pass
        else:
            busy = False
            language_combo.configure(state='readonly')
            for widget in (partial_check, save_button, import_button, refresh_button):
                widget.configure(state='normal')
            if success:
                if preferred is not None:
                    populate(preferred)
                elif isinstance(result, dict):
                    populate(result['locale'])
                    language_status.set('팩 가져오기 완료. 내용을 확인하고 언어 저장을 누르세요.')
                else:
                    language_status.set('저장됨 · 다음 게임 실행부터 적용됩니다.')
            else:
                language_status.set('처리 실패: ' + result)
        root.after(100, poll)

    def save():
        pack = selected_pack()
        if pack:
            locale, allow = pack['locale'], partial.get()
            run(lambda: controller.save(locale, allow), '언어팩 검증 및 저장 중…')

    def import_file():
        source = filedialog.askopenfilename(parent=root, title='로컬 언어팩 가져오기',
            filetypes=[('언어팩 ZIP 또는 manifest', '*.zip *.json'), ('ZIP', '*.zip'), ('Manifest', '*.json')])
        if source:
            run(lambda: controller.import_file(source), '언어팩 검증 및 가져오기 중…')

    save_button = ttk.Button(buttons, text='언어 저장', command=save)
    save_button.pack(side='left')
    import_button = ttk.Button(buttons, text='로컬 팩 가져오기…', command=import_file)
    import_button.pack(side='left', padx=6)
    refresh_button = ttk.Button(buttons, text='목록 새로 고침', command=lambda: run(controller.refresh, '설치된 언어팩 확인 중…', (selected_pack() or {}).get('locale', saved_locale)))
    refresh_button.pack(side='left')
    language_combo.bind('<<ComboboxSelected>>', selection_changed)
    root.after(100, poll)
    run(controller.refresh, '설치된 언어팩 확인 중…', saved_locale)


    def toggle(name, var):
        path = PLAY / name
        if var.get():
            path.touch()
        elif path.exists():
            path.unlink()

    for name, label, desc in OPTIONS:
        frame = tk.Frame(root)
        frame.pack(fill='x', padx=8, pady=2)
        var = tk.BooleanVar(value=(PLAY / name).exists())
        cb = tk.Checkbutton(frame, text=label, variable=var, command=lambda n=name, v=var: toggle(n, v),
                            font=('Malgun Gothic', 10, 'bold'), anchor='w')
        cb.pack(fill='x')
        tk.Label(frame, text=f'   {desc}  ({name})', fg='#555', anchor='w',
                 font=('Malgun Gothic', 9)).pack(fill='x')
        vars_.append((name, var))

    def refresh():
        # 파일이 바깥에서 바뀌어도 체크 상태를 맞춘다.
        for name, var in vars_:
            exists = (PLAY / name).exists()
            if var.get() != exists:
                var.set(exists)
        root.after(1000, refresh)

    root.after(1000, refresh)
    tk.Label(root, text='체크박스는 약 1초 이내 반영 · 언어는 재시작 필요.', fg='#336', anchor='w').pack(fill='x', padx=8, pady=8)
    root.mainloop()


if __name__ == '__main__':
    main()
