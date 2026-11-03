"""Macross 30 native Tk launcher. No browser engine or external Python is needed in the built EXE."""
import argparse
import concurrent.futures
import ctypes
import json
import os
from pathlib import Path
import queue
import re
import sys
import tempfile
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import webbrowser
from PIL import Image, ImageOps, ImageTk, ImageGrab, ImageDraw
from backend import Backend, VERSION, LOCALES, localized, update_plan, version_from, sha
from i18n import TEXT
from bundle_runtime import recover_bundle, load_hero
from launch_options import SCALES

BG, SURFACE, BORDER, INK, DIM, GOLD = '#10141c', '#1c2430', '#2a3340', '#eef2f7', '#96a5b8', '#edc47f'
DATA = Path(getattr(sys, '_MEIPASS', Path(__file__).parent))


class RoundedButton(tk.Canvas):
    def __init__(self, parent, command):
        super().__init__(parent, width=226, height=55, bg=BG, highlightthickness=0, cursor='hand2', takefocus=1)
        self.command, self.enabled, self.text = command, True, ''
        self.bind('<Button-1>', lambda _: self.invoke())
        self.bind('<Return>', lambda _: self.invoke())
        self.bind('<space>', lambda _: self.invoke())
        self.bind('<Enter>', lambda _: self.draw(True))
        self.bind('<Leave>', lambda _: self.draw())
        self.bind('<FocusIn>', lambda _: self.draw())
        self.bind('<FocusOut>', lambda _: self.draw())

    def invoke(self):
        if self.enabled:
            self.command()

    def draw(self, hover=False):
        self.delete('all')
        fill = ('#ffdaa0' if hover else GOLD) if self.enabled else '#293441'
        image = Image.new('RGB', (226*3, 55*3), BG)
        ImageDraw.Draw(image).rounded_rectangle((3, 3, 225*3, 54*3), radius=8*3, fill=fill)
        self.background = ImageTk.PhotoImage(image.resize((226, 55), Image.Resampling.LANCZOS))
        self.create_image(0, 0, image=self.background, anchor='nw')
        self.create_text(113, 27, text=self.text, font=('Segoe UI', -25, 'bold'),
                         fill='#171a21' if self.enabled else DIM)
        if self.focus_get() == self:
            self.create_rectangle(7, 7, 219, 48, outline='#ffffff')
        self.configure(cursor='hand2' if self.enabled else 'arrow')


class Launcher(tk.Tk):
    def __init__(self, args):
        super().__init__()
        self.args, self.events = args, queue.Queue()
        self.preview = args.preview
        self.worker_active = self.checking = self.handoff = False
        self.message_key, self.stage, self.error, self.tab, self.filter_id = 'ready', 0, '', 'announcements', 'all'
        self.last_transfer = None
        self.last_install_line = ''
        self.self_current = False
        self.title('Macross 30 Launcher ' + VERSION)
        self.configure(bg=BG)
        self.geometry('1030x770')
        self.minsize(900, 735)
        # Keep a normal Windows top-level window. Tk override-redirect windows
        # can remain hidden and cannot reliably restore from the taskbar.
        self.drag_origin = None
        self.option_add('*Font', ('Segoe UI', -16))
        self.protocol('WM_DELETE_WINDOW', self.close)
        try:
            value = ctypes.c_int(1)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(self.winfo_id(), 20, ctypes.byref(value), 4)
        except (AttributeError, OSError):
            pass
        config = Path(os.environ.get('LOCALAPPDATA', tempfile.gettempdir())) / 'Macross30Launcher/python-settings.json'
        if args.config:
            config = Path(args.config)
        self.backend = Backend(DATA / 'bridge.exe', config, emit=self.events.put,
                               confirm=lambda data: self.on_ui(self.confirm, data), choose_iso=lambda: self.on_ui(self.pick_iso_path))
        if self.preview:
            self.backend.manifest = json.loads((DATA / 'assets/preview-manifest.json').read_text(encoding='utf-8-sig'))
            self.backend.status = json.loads((DATA / 'assets/preview-status.json').read_text(encoding='utf-8-sig'))
            self.backend.stale = False
            self.backend.config.update(game_language='ko-KR', ui_language='ko')
            self.backend.installed = dict(core_ready=True, language_ready=True, runtime_status=2, language_status=2,
                                          installed_version='0.3.1', language_version='0.3.1', program='preview')
            self.backend.checked_at = time.time()
        self.state = self.backend.snapshot()
        self.hero_image = load_hero(DATA / 'assets/hero.jpg')
        self.build()
        self.render()
        if not args.smoke:
            self.after(350, self.show_initial_window)
        if os.name == 'nt':
            self.after_idle(self.keep_taskbar_entry)
        self.after(30, self.drain)
        self.after(300000, self.periodic)
        if not self.preview:
            self.after(150, lambda: self.run(self.startup))
        if args.smoke:
            self.after(600, self.smoke)
        elif args.capture:
            self.after(1400, self.capture_when_ready)
        if args.update_health:
            self.after(200, lambda: Path(args.update_health).write_text(sha(sys.executable), encoding='utf-8'))

    def show_initial_window(self):
        # A GUI launched by a hidden parent must explicitly map and raise itself.
        self.update_idletasks()
        x = max(0, (self.winfo_screenwidth() - self.winfo_width()) // 2)
        y = max(0, (self.winfo_screenheight() - self.winfo_height()) // 2)
        self.geometry(f'+{x}+{y}')
        self.deiconify()
        if os.name == 'nt':
            self.keep_taskbar_entry()
            user = ctypes.windll.user32
            handle = user.GetAncestor(self.winfo_id(), 2)
            user.ShowWindow.argtypes = (ctypes.c_void_p, ctypes.c_int)
            user.ShowWindow(handle, 5)
            user.SetForegroundWindow.argtypes = (ctypes.c_void_p,)
            user.SetForegroundWindow(handle)
        self.lift()
        self.attributes('-topmost', True)
        self.after(250, lambda: self.attributes('-topmost', False))

    def on_ui(self, callback, *args):
        future = concurrent.futures.Future()
        self.events.put({'type': 'call', 'callback': callback, 'args': args, 'future': future})
        return future.result()

    def startup(self):
        if self.args.skip_launcher_update:
            raise RuntimeError(self.words()['launcherCheckFailed'])
        self.message_key = 'ready'
        self.after_launcher(self.backend.check)

    def after_launcher(self, action):
        if getattr(sys, 'frozen', False):
            if self.backend.launcher_first(sys.executable, action):
                self.events.put({'type': 'handoff'})
        else:
            action()

    def words(self):
        return TEXT[self.state['config']['ui_language']]

    def label(self, parent, **kwargs):
        options = {'bg': BG, 'fg': DIM}
        options.update(kwargs)
        return tk.Label(parent, **options)

    def link(self, parent, callback, **kwargs):
        options = dict(command=callback, relief='flat', bd=0, highlightthickness=0, padx=0, pady=4, bg=BG, fg=DIM, activebackground=BG, activeforeground=INK, cursor='hand2')
        options.update(kwargs)
        button = tk.Button(parent, **options)
        return button

    def build(self):
        style = ttk.Style(self)
        style.theme_use('clam')
        style.configure('TCombobox', fieldbackground=SURFACE, background=SURFACE, foreground=INK,
                        arrowcolor=INK, bordercolor='#394655', lightcolor=SURFACE, darkcolor=SURFACE, padding=7)
        style.map('TCombobox', fieldbackground=[('readonly', SURFACE)], foreground=[('readonly', INK), ('disabled', DIM)],
                  selectbackground=[('readonly', SURFACE)], selectforeground=[('readonly', INK)])
        style.configure('Horizontal.TProgressbar', troughcolor=BORDER, background=GOLD, bordercolor=BG,
                        lightcolor=GOLD, darkcolor=GOLD, thickness=4)
        self.option_add('*TCombobox*Listbox.background', SURFACE)
        self.option_add('*TCombobox*Listbox.foreground', INK)
        self.hero = tk.Canvas(self, height=260, bg=BG, highlightthickness=0)
        self.hero.pack(fill='x')
        self.hero.bind('<Configure>', lambda event: self.draw_hero(event.width))
        self.hero.bind('<ButtonPress-1>', self.begin_drag)
        self.hero.bind('<B1-Motion>', self.drag)
        self.hero.bind('<ButtonRelease-1>', lambda _: setattr(self, 'drag_origin', None))
        self.ui_frame = tk.Frame(self.hero, bg='#10151e', padx=9, pady=6)
        self.ui_caption = tk.Label(self.ui_frame, bg='#10151e', fg=DIM, font=('Segoe UI', -13))
        self.ui_caption.pack(side='left', padx=(0, 8))
        self.ui_language = ttk.Combobox(self.ui_frame, values=('한국어', 'English', '日本語'), state='readonly', width=9)
        self.ui_language.pack(side='left')
        self.ui_language.bind('<<ComboboxSelected>>', lambda _: self.configure_language(True))
        self.ui_window = self.hero.create_window(1000, 25, window=self.ui_frame, anchor='ne')
        self.body = tk.Frame(self, bg=BG, padx=28)
        self.body.pack(fill='x')
        controls = tk.Frame(self.body, bg=BG)
        controls.pack(fill='x', pady=(5, 0))
        self.locale_caption = self.label(controls, anchor='w', fg=INK)
        self.locale_caption.grid(row=0, column=0, sticky='w', pady=(0, 7))
        self.locale = ttk.Combobox(controls, values=('한국어', 'English', '日本語'), state='readonly', width=26, font=('Segoe UI', -18))
        self.locale.grid(row=1, column=0, sticky='w')
        self.locale.bind('<<ComboboxSelected>>', lambda _: self.configure_language(False))
        self.language_version = self.label(controls, anchor='w', font=('Segoe UI', -13))
        self.language_version.grid(row=2, column=0, sticky='w', pady=(8, 0))
        controls.columnconfigure(0, weight=1)
        self.play = RoundedButton(controls, self.play_clicked)
        self.play.grid(row=0, column=1, rowspan=3, sticky='e', pady=(0, 12))
        self.options_button = self.link(controls, self.options_clicked, font=('Segoe UI', -13))
        self.options_button.grid(row=3, column=1, sticky='e')
        tk.Frame(self.body, bg=BORDER, height=1).pack(fill='x', pady=(22, 14))
        status = tk.Frame(self.body, bg=BG)
        status.pack(fill='x')
        self.status_label = self.label(status, fg='#b4c3d5', anchor='w')
        self.status_label.pack(side='left', fill='x', expand=True)
        self.checked_label = self.label(status, font=('Segoe UI', -12))
        self.checked_label.pack(side='right')
        self.progress = ttk.Progressbar(self.body, mode='determinate', maximum=100)
        self.operation_detail = self.label(self.body, anchor='w', fg=DIM, font=('Segoe UI', -12), wraplength=920)
        self.steps_frame = tk.Frame(self.body, bg=BG)
        self.steps = [self.label(self.steps_frame, font=('Segoe UI', -13)) for _ in range(3)]
        for item in self.steps:
            item.pack(side='left', padx=(0, 25))
        self.error_box = tk.Text(self.body, height=4, wrap='word', relief='flat', bg='#35241f', fg='#ffd1c1',
                                 font=('Segoe UI', -13), state='disabled', highlightthickness=0, padx=10, pady=7)
        self.info = tk.Frame(self, bg=BG, padx=28)
        self.info.pack(fill='both', expand=True, pady=(20, 0))
        tk.Frame(self.info, bg=BORDER, height=1).pack(fill='x', pady=(0, 12))
        tabs = tk.Frame(self.info, bg=BG)
        tabs.pack(fill='x')
        self.notice_button = self.link(tabs, lambda: self.switch_tab('announcements'))
        self.notice_button.pack(side='left', padx=(0, 20))
        self.patch_button = self.link(tabs, lambda: self.switch_tab('patch_notes'))
        self.patch_button.pack(side='left')
        self.version_filter = ttk.Combobox(tabs, state='readonly', width=24, font=('Segoe UI', -13))
        self.version_filter.bind('<<ComboboxSelected>>', self.change_filter)
        self.server_label = self.label(tabs, font=('Segoe UI', -12))
        self.server_label.pack(side='right', padx=(10, 0))
        self.content = tk.Text(self.info, wrap='word', bg=BG, fg=DIM, relief='flat', borderwidth=0,
                                height=6, padx=0, pady=10, highlightthickness=0, state='disabled', cursor='arrow')
        self.content.tag_configure('title', foreground=INK, font=('Segoe UI', -16, 'bold'), spacing3=7)
        self.content.tag_configure('body', foreground=DIM, font=('Segoe UI', -14), spacing3=12)
        self.content.pack(fill='both', expand=True)
        footer = tk.Frame(self, bg=BG, padx=28, pady=12)
        # Reserve recovery controls before the expanding notices pane, including on errors.
        footer.pack(fill='x', side='bottom', before=self.info)
        tk.Frame(footer, bg=BORDER, height=1).pack(fill='x', pady=(0, 8))
        self.discord = self.link(footer, lambda: webbrowser.open('https://discord.gg/WqaaJ6hTmY'), fg='#bfc5ff', font=('Segoe UI', -13))
        self.discord.pack(side='left', padx=(0, 18))
        self.iso_button = self.link(footer, self.iso_clicked, font=('Segoe UI', -13))
        self.iso_button.pack(side='left', padx=(0, 18))
        self.folder_button = self.link(footer, self.folder_clicked, font=('Segoe UI', -13))
        self.folder_button.pack(side='left', padx=(0, 18))
        self.logs_button = self.link(footer, self.show_logs, font=('Segoe UI', -13))
        self.logs_button.pack(side='left')
        self.self_button = self.link(footer, self.self_clicked, font=('Segoe UI', -13))
        self.self_button.pack(side='right')
        self.launcher_version = self.label(footer, font=('Segoe UI', -12))
        self.launcher_version.pack(side='right', padx=(0, 18))

    def begin_drag(self, event):
        current = self.hero.find_withtag('current')
        if current and set(self.hero.gettags(current[0])) & {'check', 'close', 'minimize'}:
            return
        self.drag_origin = event.x_root-self.winfo_x(), event.y_root-self.winfo_y()

    def drag(self, event):
        if self.drag_origin:
            self.geometry(f'+{event.x_root-self.drag_origin[0]}+{event.y_root-self.drag_origin[1]}')

    def minimize(self):
        self.iconify()

    def keep_taskbar_entry(self):
        # Remove only native decorations; retain Tk's normal top-level lifecycle.
        user = ctypes.windll.user32
        user.GetAncestor.argtypes = (ctypes.c_void_p, ctypes.c_uint)
        user.GetAncestor.restype = ctypes.c_void_p
        user.GetWindowLongPtrW.argtypes = (ctypes.c_void_p, ctypes.c_int)
        user.GetWindowLongPtrW.restype = ctypes.c_ssize_t
        user.SetWindowLongPtrW.argtypes = (ctypes.c_void_p, ctypes.c_int, ctypes.c_ssize_t)
        user.SetWindowLongPtrW.restype = ctypes.c_ssize_t
        user.SetWindowPos.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int,
                                     ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint)
        handle = user.GetAncestor(self.winfo_id(), 2)
        style = user.GetWindowLongPtrW(handle, -20)
        user.SetWindowLongPtrW(handle, -20, (style & ~0x80) | 0x40000)
        style = user.GetWindowLongPtrW(handle, -16)
        user.SetWindowLongPtrW(handle, -16, style & ~(0x00C00000 | 0x00040000))
        user.SetWindowPos(handle, None, 0, 0, 0, 0, 0x0037)

    def outlined(self, x, y, text, font, fill=INK, anchor='nw', tags=()):
        for dx, dy in ((-1, -1), (1, -1), (-1, 1), (1, 1), (0, 2)):
            self.hero.create_text(x+dx, y+dy, text=text, font=font, fill='#000000', anchor=anchor, tags=tags)
        return self.hero.create_text(x, y, text=text, font=font, fill=fill, anchor=anchor, tags=tags)

    def draw_hero(self, width=None):
        width = max(900, width or self.hero.winfo_width())
        hero_state = (width, self.state['config']['ui_language'], self.state['installed'].get('installed_version'), (self.state.get('manifest') or {}).get('version'), self.checking)
        if getattr(self, '_hero_state', None) == hero_state:
            return
        self._hero_state = hero_state
        self.hero.delete('drawing')
        image = ImageOps.fit(self.hero_image, (width, 260), method=Image.Resampling.LANCZOS)
        # Fade the picture into the native panel instead of placing text in opaque cards.
        shade = Image.new('RGBA', (1, 260))
        shade.putdata([(16, 20, 28, int(235 * max(0, (y-80)/180) ** 1.5)) for y in range(260)])
        shade = shade.resize(image.size)
        image = Image.alpha_composite(image.convert('RGBA'), shade)
        self.hero_photo = ImageTk.PhotoImage(image)
        self.hero.create_image(0, 0, image=self.hero_photo, anchor='nw', tags='drawing')
        self.outlined(29, 25, 'MACROSS', ('Segoe UI', -41, 'bold italic'), tags='drawing')
        self.outlined(255, 25, '30', ('Segoe UI', -41, 'bold'), fill=GOLD, tags='drawing')
        w, installed = self.words(), self.state['installed']
        current = installed.get('installed_version', '')
        if not re.search(r'\d+\.\d+\.\d+', current):
            current = w['missing']
        latest = (self.state.get('manifest') or {}).get('version', w['unknown'])
        self.outlined(350, 29, w['game']+' '+current, ('Segoe UI', -13), tags='drawing')
        self.outlined(350, 50, w['latest']+' '+latest, ('Segoe UI', -13), tags='drawing')
        self.outlined(550, 40, w['unknown'] if self.checking else w['check'], ('Segoe UI', -13), tags=('drawing', 'check'))
        self.hero.tag_bind('check', '<Button-1>', lambda _: self.check_clicked())
        self.hero.tag_bind('check', '<Enter>', lambda _: self.hero.configure(cursor='hand2'))
        self.hero.tag_bind('check', '<Leave>', lambda _: self.hero.configure(cursor='arrow'))
        self.outlined(29, 230, 'THE VOICES THAT CONNECT THE GALAXY', ('Segoe UI', -12), fill='#9fb0c7', tags='drawing')
        self.hero.coords(self.ui_window, width-28, 26)
        self.hero.tag_raise(self.ui_window)
        for tag, text, x, command in (('minimize', '—', width-51, self.minimize),
                                      ('close', '×', width-18, self.close)):
            self.hero.create_text(x, 10, text=text, font=('Segoe UI', -14), fill=DIM,
                                  tags=('drawing', tag))
            self.hero.tag_bind(tag, '<Button-1>', lambda _, action=command: action())

    def needs_update(self):
        return bool(self.state['manifest'] and any(update_plan(self.state['installed'], self.state['config']['game_language']).values()))

    def render(self):
        w, config, installed = self.words(), self.state['config'], self.state['installed']
        locked = self.worker_active or self.state['busy'] or self.state['running']
        self.ui_language.current(('ko', 'en', 'ja').index(config['ui_language']))
        self.locale.current(LOCALES.index(config['game_language']))
        self.ui_caption.configure(text='Language')
        self.locale_caption.configure(text=w['locale'])
        self.ui_language.configure(state='disabled' if locked else 'readonly')
        self.locale.configure(state='disabled' if locked else 'readonly')
        self.play.enabled = not locked
        self.play.text = w['running'] if self.state['running'] else w['working'] if self.state['busy'] else w['unknown'] if self.checking else w['update']+'  ↓' if self.needs_update() else w['play']+'  ▶'
        self.play.draw()
        locale = config['game_language']
        if locale == 'ja-JP':
            version_text = w['japanese']
        else:
            version = installed.get('language_version', '')
            if not re.fullmatch(r'\d+\.\d+\.\d+', version):
                version = '—' if installed.get('language_ready') else w['missing']
            latest = version_from(((self.state['manifest'] or {}).get('language_packs') or {}).get(locale, {})) or w['unknown']
            version_text = w['pack']+' '+version+' · '+w['latest']+' '+latest
        self.language_version.configure(text=version_text)
        status = w['unknown'] if self.checking else w['needsUpdate'] if self.message_key == 'ready' and self.needs_update() else w.get(self.message_key, self.message_key)
        active = not self.state['running'] and (self.state['busy'] or (self.worker_active and not self.checking))
        if self.last_transfer and active:
            transfer = self.last_transfer
            status += (f" · {transfer['bytes']} / {transfer['total']} · {transfer['percent']}%" if transfer.get('unit') == 'items' else
                       f" · {transfer['bytes']/1048576:.1f} / {transfer['total']/1048576:.1f} MB · {transfer['percent']}%")
        self.status_label.configure(text='•  '+status)
        self.checked_label.configure(text=w['checked'] if self.state['checked_at'] else '')
        if active:
            self.progress.pack(fill='x', pady=(12, 0))
            detail = self.last_install_line
            if self.last_transfer:
                transfer = self.last_transfer
                detail = (f"{transfer['percent']}% · {transfer['bytes']} / {transfer['total']}" if transfer.get('unit') == 'items' else
                          f"{transfer['percent']}% · {transfer['bytes']/1048576:.1f} / {transfer['total']/1048576:.1f} MB")
            self.operation_detail.configure(text=detail)
            self.operation_detail.pack(fill='x', pady=(5, 0))
            self.steps_frame.pack(fill='x', pady=(8, 0))
        else:
            self.progress.pack_forget()
            self.operation_detail.pack_forget()
            self.steps_frame.pack_forget()
        for index, item in enumerate(self.steps):
            item.configure(text=w['steps'][index], fg=GOLD if self.stage == index+1 else DIM)
        if self.error:
            self.error_box.pack(fill='x', pady=(10, 0))
            self.error_box.configure(state='normal')
            self.error_box.delete('1.0', 'end')
            self.error_box.insert('end', self.error)
            self.error_box.configure(state='disabled')
        else:
            self.error_box.pack_forget()
        for button, key in ((self.options_button, 'launchOptions'), (self.discord, 'discord'), (self.iso_button, 'iso'), (self.folder_button, 'folder'), (self.logs_button, 'logs'), (self.self_button, 'self')):
            button.configure(text=w[key])
        last_session = self.state.get('last_session') or {}
        if last_session.get('exit_code'):
            self.logs_button.configure(text={'ko':'로그 보내기','en':'Send logs','ja':'ログを送信'}[self.state['config']['ui_language']],fg=GOLD)
        for button in (self.options_button, self.iso_button, self.folder_button, self.self_button):
            button.configure(state='disabled' if locked else 'normal')
        self.launcher_version.configure(text=w['launcher']+' '+VERSION+(' · '+w['latest'] if self.self_current else ''))
        self.render_content()
        self.draw_hero()
        if getattr(self,'iso_window',None) and self.iso_window.winfo_exists():self.render_iso_window()

    def render_content(self):
        w, status = self.words(), self.state['status'] or {}
        content_state = (self.state['config']['ui_language'], json.dumps(status, sort_keys=True), self.state['stale'], self.tab, self.filter_id)
        if getattr(self, '_content_state', None) == content_state:
            return
        self._content_state = content_state
        self.notice_button.configure(text=w['notice'], fg=GOLD if self.tab == 'announcements' else DIM)
        self.patch_button.configure(text=w['patches'], fg=GOLD if self.tab == 'patch_notes' else DIM)
        self.server_label.configure(text=w['stale'] if self.state['stale'] else w['closed'] if status.get('closed') else w['open'])
        lang = self.state['config']['ui_language']
        fallback = status.get('default_language', 'ko')
        rows = status.get('patch_notes', [])
        self.filter_ids = ['all'] + [row['id'] for row in rows]
        self.version_filter.configure(values=[w['all']] + [localized(row['title'], lang, fallback) for row in rows])
        if self.filter_id not in self.filter_ids:
            self.filter_id = 'all'
        self.version_filter.current(self.filter_ids.index(self.filter_id))
        if self.tab == 'patch_notes':
            self.version_filter.pack(side='right', padx=(12, 0))
        else:
            self.version_filter.pack_forget()
        self.content.configure(state='normal')
        self.content.delete('1.0', 'end')
        entries = [row for row in status.get(self.tab, []) if self.tab == 'announcements' or self.filter_id == 'all' or row['id'] == self.filter_id]
        for row in entries:
            self.content.insert('end', localized(row['title'], lang, fallback)+'\n', 'title')
            self.content.insert('end', localized(row['body'], lang, fallback)+'\n\n', 'body')
        if not entries:
            self.content.insert('end', w['empty'], 'body')
        self.content.configure(state='disabled')

    def run(self, callback):
        if self.worker_active:
            return
        self.worker_active = True
        self.error = ''
        self.last_transfer = None
        self.render()
        def worker():
            try:
                callback()
            except Exception as error:
                self.events.put({'type': 'failure', 'detail': str(error)})
            finally:
                self.events.put({'type': 'worker-done', 'state': self.backend.snapshot()})
        threading.Thread(target=worker, daemon=True).start()

    def drain(self):
        redraw = False
        try:
            while True:
                event = self.events.get_nowait()
                kind = event['type']
                if kind == 'call':
                    try:
                        event['future'].set_result(event['callback'](*event['args']))
                    except Exception as error:
                        event['future'].set_exception(error)
                elif kind == 'state':
                    self.state, redraw = event['state'], True
                elif kind == 'worker-done':
                    self.progress.stop()
                    self.worker_active = False
                    self.state, redraw = event['state'], True
                    if getattr(self,'report_window',None) and self.report_window.winfo_exists():
                        self.report_send.configure(state='normal')
                elif kind == 'report-prepared':
                    self.show_report(event['report'])
                elif kind == 'report-result':
                    report = event['report']
                    if getattr(self,'report_window',None) and self.report_window.winfo_exists():
                        lang = self.state['config']['ui_language']
                        if report['status'] == 'sent':
                            self.report_status.configure(text={'ko':'접수 완료 · 번호 ','en':'Received · ID ','ja':'受付完了 · 番号 '}[lang]+report['receipt_id'])
                            self.report_send.configure(text={'ko':'접수 확인','en':'View receipt','ja':'受付を確認'}[lang],command=lambda url=report['receipt_url']:webbrowser.open(url))
                        else:
                            self.report_status.configure(text={'ko':'전송 결과 확인 중입니다. ZIP은 보존되어 있습니다.','en':'Delivery is being checked. Your ZIP is preserved.','ja':'送信結果を確認中です。ZIPは保存されています。'}[lang])
                elif kind == 'checking':
                    self.checking, redraw = event['value'], True
                elif kind == 'progress':
                    self.message_key = event['key']
                    self.stage = event.get('stage', self.stage)
                    self.last_transfer = None
                    self.last_install_line = ''
                    self.progress['value'] = 0
                    self.progress.configure(mode='indeterminate')
                    self.progress.start(15)
                    redraw = True
                elif kind == 'transfer':
                    self.progress.stop()
                    self.progress.configure(mode='determinate')
                    self.last_transfer = event
                    self.progress['value'] = event['percent']
                    redraw = True
                elif kind == 'install-line':
                    self.last_install_line = event['line']
                    self.last_transfer = None
                    redraw = True
                    match = re.match(r'BOOT_STAGE ([1-7])/7', event['line'])
                    if match:
                        self.progress.stop()
                        self.progress.configure(mode='determinate')
                        self.progress['value'] = (int(match[1])-1)*100/7
                        self.last_transfer = None
                    file_match = re.match(r'\[(\d+)/(\d+)\] ([^:]+): (.*)', event['line'])
                    if file_match:
                        done, total, path, result = file_match.groups()
                        self.last_install_line = f'{done}/{total} · {path.rsplit("/", 1)[-1]} · {result}'
                        self.progress.stop()
                        self.progress.configure(mode='determinate')
                        self.progress['value'] = int(done)*100/max(1, int(total))
                elif kind == 'failure':
                    self.error, self.message_key, redraw = event['detail'], 'failed', True
                elif kind == 'handoff':
                    self.handoff = True
                    self.destroy()
                    return
                elif kind == 'self-current':
                    self.self_current = True
                    redraw = True
                elif kind == 'iso-status':
                    self.iso_info=event['info']
                    redraw=True
        except queue.Empty:
            pass
        if redraw:
            self.render()
        self.after(40, self.drain)

    def configure_language(self, ui):
        if self.preview:
            self.backend.config['ui_language' if ui else 'game_language'] = ('ko','en','ja')[self.ui_language.current()] if ui else LOCALES[self.locale.current()]
            self.state = self.backend.snapshot()
            self.render()
            return
        key = 'ui_language' if ui else 'game_language'
        value = ('ko', 'en', 'ja')[self.ui_language.current()] if ui else LOCALES[self.locale.current()]
        self.run(lambda: self.backend.settings(**{key: value}))

    def switch_tab(self, tab):
        self.tab = tab
        self.render_content()

    def change_filter(self, _event=None):
        self.filter_id = self.filter_ids[self.version_filter.current()]
        self.render_content()

    def check_clicked(self):
        if not self.preview and not self.worker_active:
            self.message_key = 'ready'
            self.args.skip_launcher_update = False
            self.run(self.startup)

    def play_clicked(self):
        if not self.preview:
            self.run(lambda: self.after_launcher(self.backend.play))

    def pick_iso_path(self):
        return filedialog.askopenfilename(parent=self, filetypes=[('PS3 ISO', '*.iso')])

    def options_clicked(self):
        if self.worker_active or self.state['busy'] or self.state['running']:
            return
        if getattr(self, 'options_window', None) and self.options_window.winfo_exists():
            self.options_window.lift()
            return
        words = self.words()
        try:
            document = self.backend.load_launch_options()
        except (OSError, ValueError) as error:
            messagebox.showerror(words['launchOptions'], words['optionsReadFailed']+'\n'+str(error), parent=self)
            return
        window = self.options_window = tk.Toplevel(self)
        window.title(words['launchOptions'])
        window.transient(self)
        window.resizable(False, False)
        body = ttk.Frame(window, padding=20)
        body.pack(fill='both', expand=True)
        ttk.Label(body, text=document['path'], wraplength=540).pack(anchor='w')
        ttk.Label(body, text=words['optionsRestart'], wraplength=540).pack(anchor='w', pady=(10, 14))
        variables = {key: tk.StringVar(window, value=value) for key, value in document['values'].items()}
        variables['bloom'].set('1')
        row = ttk.Frame(body)
        row.pack(fill='x', pady=5)
        ttk.Label(row, text=words['optionScale']).pack(side='left', padx=(0, 20))
        ttk.Combobox(row, textvariable=variables['render_scale'], values=SCALES, state='readonly', width=10).pack(side='right')
        ttk.Label(body, text=words['optionsScaleHelp'], wraplength=540).pack(anchor='w', pady=5)
        for key, label in (('fullscreen', 'optionFullscreen'), ('vsync', 'optionVsync'),
                           ('subtitles', 'optionSubtitles')):
            ttk.Checkbutton(body, text=words[label], variable=variables[key], onvalue='1', offvalue='0').pack(anchor='w', pady=5)
        ttk.Checkbutton(body, text=words['optionsFpsUnavailable'], state='disabled').pack(anchor='w', pady=5)
        if document.get('hd_available'):
            ttk.Checkbutton(body, text=words['optionHd'], variable=variables['hd_textures'], onvalue='1', offvalue='0').pack(anchor='w', pady=5)
        else:
            ttk.Label(body, text=words['optionsHdUnavailable'], wraplength=540).pack(anchor='w', pady=5)
        status = tk.StringVar(window)
        ttk.Label(body, textvariable=status, wraplength=540).pack(anchor='w', pady=8)
        buttons = ttk.Frame(body)
        buttons.pack(fill='x', pady=(8, 0))
        def save_options():
            try:
                if self.worker_active:
                    raise RuntimeError(words['optionsWait'])
                self.backend.save_launch_options(document, {key: value.get() for key, value in variables.items()})
            except (OSError, ValueError, RuntimeError) as error:
                status.set(words['optionsSaveFailed']+'\n'+str(error))
                return
            window.destroy()
            messagebox.showinfo(words['launchOptions'], words['optionsSaved'], parent=self)
        ttk.Button(buttons, text=words['optionsSave'], command=save_options).pack(side='right')
        ttk.Button(buttons, text=words['optionsCancel'], command=window.destroy).pack(side='right', padx=10)
        window.grab_set()

    def iso_clicked(self):
        if getattr(self,'iso_window',None) and self.iso_window.winfo_exists():
            self.iso_window.lift();return
        window=self.iso_window=tk.Toplevel(self)
        window.geometry('760x410');window.minsize(680,380)
        self.iso_labels=[ttk.Label(window,wraplength=700,justify='left') for _ in range(4)]
        for label in self.iso_labels:label.pack(anchor='w',padx=20,pady=(12,0))
        controls=ttk.Frame(window);controls.pack(fill='x',padx=20,pady=20)
        def select_iso():
            path=filedialog.askopenfilename(parent=window,filetypes=[('PS3 ISO','*.iso')])
            if path and not self.preview:
                def action():
                    self.backend.settings(iso=path,disc_key='');self.backend.inspect_iso()
                self.run(action)
        def select_key():
            path=filedialog.askopenfilename(parent=window,filetypes=[('Disc key','*.dkey *.key'),('All files','*.*')])
            if path and not self.preview:
                def action():
                    self.backend.settings(disc_key=path);self.backend.inspect_iso()
                self.run(action)
        self.iso_select=ttk.Button(controls,command=select_iso);self.iso_select.pack(side='left',padx=(0,10))
        self.iso_key=ttk.Button(controls,command=select_key);self.iso_key.pack(side='left',padx=(0,10))
        self.iso_check=ttk.Button(controls,command=lambda:self.run(self.backend.inspect_iso));self.iso_check.pack(side='left')
        self.iso_clear=ttk.Button(controls,command=lambda:self.run(lambda:(self.backend.settings(disc_key=''),self.backend.inspect_iso())))
        self.iso_clear.pack(side='right')
        self.render_iso_window()
        if not self.preview and not self.worker_active and not self.state['running']:
            self.run(self.backend.inspect_iso)

    def render_iso_window(self):
        words=self.words();config=self.state['config'];info=getattr(self,'iso_info',None) or {}
        self.iso_window.title(words['iso'])
        kind=info.get('kind','missing')
        key={'ready':'isoReady','encrypted':'isoEncrypted','unsupported':'isoUnsupported','missing':'isoNoFile'}.get(kind,'isoUnsupported')
        if kind=='encrypted':
            key={'missing':'isoKeyMissing','invalid-format':'isoKeyInvalid','mismatch':'isoKeyMismatch','matched':'isoKeyMatched'}.get(info.get('key_status'),'isoKeyMissing')
        texts=[config.get('iso') or words['isoNoFile'],words['isoHelp'],
               words['isoHashLabel']+': '+info.get('sha1','—'),words[key]]
        if self.worker_active:texts[3]=words.get(self.message_key,words['working'])
        for label,text in zip(self.iso_labels,texts):label.configure(text=text)
        self.iso_select.configure(text=words['isoChoose'])
        self.iso_key.configure(text=words['isoKeyChoose'])
        self.iso_check.configure(text=words['isoCheck'])
        self.iso_clear.configure(text=words['isoKeyClear'])
        self.iso_labels[1].configure(text=words['isoHelp']+'\n'+words['isoKeyFile']+': '+
                                     (Path(config['disc_key']).name if config.get('disc_key') else '—'))
        locked=self.worker_active or self.state['running']
        for button in (self.iso_select,self.iso_key,self.iso_check,self.iso_clear):button.configure(state='disabled' if locked else 'normal')

    def folder_clicked(self):
        path = filedialog.askdirectory(parent=self, initialdir=self.state['config']['folder'])
        if path and not self.preview:
            self.run(lambda: self.backend.settings(folder=path))

    def confirm(self, data):
        w = self.words()
        if data['kind'] == 'launcher':
            return messagebox.askyesno(w['self'], w['launcher']+' '+data['version']+'?', parent=self, default='no')
        rows, count = [], 0
        manifest, plan = data['manifest'], data['plan']
        if plan['core']:
            rows.append(w['steps'][0]+' '+manifest['version'])
            count += manifest['files']['program']['bytes']
        if plan['language']:
            row = manifest['language_packs'][data['locale']]
            rows.append(('한국어' if data['locale'] == 'ko-KR' else 'English')+' '+w['pack']+' '+version_from(row))
            count += row['bytes']
        yes = messagebox.askyesno(w['confirm'], w['confirmBody']+'\n\n'+'\n'.join(rows)+f"\n\n{w['bytes']}: {count/1048576:.1f} MB", parent=self, default='no')
        if not yes:
            self.message_key = 'cancelled'
        return yes

    def self_clicked(self):
        if self.preview:
            return
        if not getattr(sys, 'frozen', False):
            self.self_button.configure(text=self.words()['selfDev'])
            return
        def update():
            try:
                result = self.backend.self_update(sys.executable, automatic=True)
            except __import__('urllib.error', fromlist=['HTTPError']).HTTPError as error:
                if error.code == 404:
                    self.events.put({'type': 'self-current'})
                    return
                raise
            if result:
                self.events.put({'type': 'handoff'})
        self.run(update)

    def show_logs(self):
        window = tk.Toplevel(self)
        window.title(self.words()['logs'])
        window.geometry('870x500')
        locale = self.state['config']['ui_language']
        ttk.Button(window,text={'ko':'마지막 플레이 로그 보내기','en':'Send last play logs','ja':'前回プレイのログを送信'}[locale],
                   command=lambda:self.run(self.backend.prepare_report)).pack(fill='x',pady=(0,6))
        label = {'ko': '게임 로그 ZIP 내보내기 (설정·시스템 정보 포함 / 세이브 선택)',
                 'en': 'Export game logs ZIP (settings and system info / optional save)',
                 'ja': 'ゲームログZIPを出力（設定・システム情報 / セーブ選択）'}[locale]
        def export():
            dialog = tk.Toplevel(window)
            dialog.title(self.words()['logs'])
            saves = Path(self.backend.config['folder']) / 'play/gamedata/dev_hdd0/home/00000001/savedata'
            slots = sorted(p.name for p in saves.iterdir() if p.is_dir()) if saves.is_dir() else []
            none = {'ko': '세이브 첨부 안 함', 'en': 'No save attachment', 'ja': 'セーブを添付しない'}[locale]
            selected = tk.StringVar(value=none)
            ttk.Combobox(dialog, values=[none] + slots, textvariable=selected, state='readonly', width=65).pack(padx=16, pady=16)
            def collect():
                slot = selected.get()
                dialog.destroy()
                self.run(lambda: self.backend.export_game_logs(None if slot == none else slot))
            ttk.Button(dialog, text=label, command=collect).pack(padx=16, pady=(0, 16))
        ttk.Button(window, text=label, command=export).pack(fill='x')
        text = tk.Text(window, wrap='word', bg=BG, fg=INK, font=('Consolas', -14))
        text.pack(fill='both', expand=True)
        previous = None
        def refresh():
            nonlocal previous
            if not window.winfo_exists():
                return
            raw = '\n'.join(self.backend.logs)
            path = self.backend.last_log
            if path:
                try:
                    with path.open('rb') as stream:
                        stream.seek(max(0, path.stat().st_size-60000))
                        raw += '\n\n'+path.name+'\n'+stream.read().decode('utf-8', errors='replace')
                except OSError as error:
                    raw += '\n\n'+str(error)
            if raw != previous:
                follow = previous is None or text.yview()[1] >= 0.99
                position = text.yview()[0]
                text.configure(state='normal')
                text.delete('1.0', 'end')
                text.insert('end', raw)
                text.configure(state='disabled')
                text.see('end') if follow else text.yview_moveto(position)
                previous = raw
            window.after(500, refresh)
        refresh()

    def show_report(self, report):
        lang = self.state['config']['ui_language']
        words = {
            'ko': ('마지막 플레이 로그 보내기','Discord 운영진 전용 채널로 정제된 로그를 보냅니다.\n로그인 불필요 · 자동 전송 안 함 · 전송 ZIP 로컬 보존',
                   '세이브 첨부 안 함','전송','닫기','전송 중…','선택 세이브 반영'),
            'en': ('Send last play logs','Send sanitized logs to the private Discord support channel.\nNo login · No automatic sending · ZIP retained locally',
                   'No save attachment','Send','Close','Sending…','Apply selected save'),
            'ja': ('前回プレイのログを送信','Discord運営専用チャンネルにログを送信します。\nログイン不要・自動送信なし・ZIPはローカルに保存',
                   'セーブを添付しない','送信','閉じる','送信中…','選択したセーブを反映')
        }[lang]
        if getattr(self,'report_window',None) and self.report_window.winfo_exists(): self.report_window.destroy()
        window = self.report_window = tk.Toplevel(self)
        window.title(words[0]); window.geometry('700x450')
        ttk.Label(window,text=words[1],justify='left').pack(anchor='w',padx=16,pady=12)
        meta = report['metadata']
        ttk.Label(window,text=f"Game {meta['game_version']} · Launcher {meta['launcher_version']} · {meta['language']} · Exit {meta['exit_code']}\nZIP {report['bytes']:,} bytes · SHA256 {report['sha256'][:16]}…").pack(anchor='w',padx=16)
        files = tk.Text(window,height=9,wrap='word')
        files.pack(fill='both',expand=True,padx=16,pady=10)
        files.insert('end','\n'.join(row['file']+' · '+str(row['bytes'])+' B' for row in report['files']))
        files.configure(state='disabled')
        saves = Path(self.backend.config['folder'])/'play/gamedata/dev_hdd0/home/00000001/savedata'
        slots = sorted(p.name for p in saves.iterdir() if p.is_dir()) if saves.is_dir() else []
        selected = tk.StringVar(value=report.get('save_slot') or words[2])
        options = ttk.Frame(window); options.pack(fill='x',padx=16)
        ttk.Combobox(options,textvariable=selected,values=[words[2]]+slots,state='readonly',width=44).pack(side='left')
        ttk.Button(options,text=words[6],command=lambda:self.run(lambda:self.backend.prepare_report(None if selected.get()==words[2] else selected.get()))).pack(side='left',padx=8)
        self.report_status = ttk.Label(window,text=report['status'])
        self.report_status.pack(anchor='w',padx=16,pady=8)
        controls = ttk.Frame(window); controls.pack(fill='x',padx=16,pady=(0,12))
        def transmit():
            self.report_status.configure(text=words[5]); self.report_send.configure(state='disabled')
            self.run(lambda:self.backend.send_report(report))
        self.report_send = ttk.Button(controls,text=words[3],command=transmit)
        self.report_send.pack(side='left')
        ttk.Button(controls,text=words[4],command=window.destroy).pack(side='right')

    def periodic(self):
        if not self.preview and not self.worker_active:
            self.run(self.backend.check)
        self.after(300000, self.periodic)

    def close(self):
        if self.backend.cancel_graphics_prep():
            return
        if not self.handoff and (self.worker_active or self.state['running']):
            messagebox.showinfo(self.title(), self.words()['closeBusy'], parent=self)
            return
        self.destroy()

    def capture_when_ready(self):
        if self.worker_active:
            self.after(200, self.capture_when_ready)
            return
        self.update_idletasks()
        x, y, w, h = self.winfo_rootx(), self.winfo_rooty(), self.winfo_width(), self.winfo_height()
        ImageGrab.grab((x, y, x+w, y+h)).save(self.args.capture)
        self.destroy()

    def smoke(self):
        try:
            self.ui_language.current(1)
            self.configure_language(True)
            assert self.notice_button['text'] == 'Notices'
            assert 'Problems may occur in all supported languages' in self.content.get('1.0', 'end')
            self.switch_tab('patch_notes')
            self.version_filter.current(self.filter_ids.index('en-0.3.1'))
            self.change_filter()
            assert 'English pack 0.3.1' in self.content.get('1.0', 'end')
            self.locale.current(1)
            self.configure_language(False)
            assert self.play.text.startswith('Play')
            assert not self.worker_active
            self.iso_info=dict(kind='encrypted',key_status='mismatch',sha1='a'*40)
            self.iso_clicked()
            assert self.words()['isoKeyMismatch'] in self.iso_labels[3]['text']
            assert self.iso_key['text']==self.words()['isoKeyChoose']
            self.iso_window.update()
            if self.args.iso_capture:
                win=self.iso_window
                ImageGrab.grab((win.winfo_rootx(),win.winfo_rooty(),win.winfo_rootx()+win.winfo_width(),win.winfo_rooty()+win.winfo_height())).save(self.args.iso_capture)
            self.iso_window.destroy()
            report = {'language': True, 'localized_notices': True, 'version_filter': True,
                      'selection_without_download': True, 'play_button': self.play.text,'iso_settings':True,'key_mismatch_message':True}
            if self.args.report:
                Path(self.args.report).write_text(json.dumps(report), encoding='utf-8')
            print('TK_SMOKE '+json.dumps(report))
            self.destroy()
        except Exception as error:
            if self.args.report:
                Path(self.args.report).write_text(json.dumps({'error': str(error)}), encoding='utf-8')
            self.destroy()
            raise


def main():
    if recover_bundle(DATA, VERSION):
        return
    parser = argparse.ArgumentParser()
    parser.add_argument('--preview', action='store_true')
    parser.add_argument('--capture')
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--report')
    parser.add_argument('--config')
    parser.add_argument('--update-health')
    parser.add_argument('--skip-launcher-update', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--acceptance', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--staged-assets', help=argparse.SUPPRESS)
    parser.add_argument('--iso-check',action='store_true',help=argparse.SUPPRESS)
    parser.add_argument('--iso-prepare',action='store_true',help=argparse.SUPPRESS)
    parser.add_argument('--program',help=argparse.SUPPRESS)
    parser.add_argument('--iso-capture',help=argparse.SUPPRESS)
    parser.add_argument('--report-acceptance', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.iso_check or args.iso_prepare:
        if not args.config or not args.report:parser.error('ISO acceptance requires config and report paths')
        backend=Backend(DATA/'bridge.exe',Path(args.config))
        try:
            result={'ok':True,'info':backend.inspect_iso()}
            if args.iso_prepare:
                if not args.program:parser.error('ISO preparation requires a verified program directory')
                result['prepared']=backend.prepare_disc(args.program)
        except Exception as error:result={'ok':False,'error':str(error)}
        Path(args.report).write_text(json.dumps(result,indent=2),encoding='utf-8')
        raise SystemExit(0 if result['ok'] else 1)
    if args.report_acceptance:
        if not args.config or not args.report: parser.error('Report acceptance requires config and report paths')
        backend = Backend(DATA/'bridge.exe',Path(args.config))
        report = backend.prepare_report(test=True)
        result = backend.send_report(report)
        Path(args.report).write_text(json.dumps(result,indent=2),encoding='utf-8')
        raise SystemExit(0 if result['status']=='sent' else 1)
    if args.acceptance:
        if not args.config or not args.report:
            parser.error('Acceptance mode requires --config and --report')
        from acceptance import run
        raise SystemExit(run(args.config, args.report, DATA, args.staged_assets))
    try:
        ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    except (AttributeError, OSError):
        pass
    # A single instance avoids competing installers or a self-update replacing an open launcher.
    mutex = ctypes.windll.kernel32.CreateMutexW(None, False, 'Local\\Macross30PythonLauncher') if os.name == 'nt' and not args.preview else None
    if mutex and ctypes.windll.kernel32.GetLastError() == 183:
        return
    try:
        window = Launcher(args)
        if args.smoke:
            window.withdraw()
        window.mainloop()
    finally:
        if mutex:
            ctypes.windll.kernel32.CloseHandle(mutex)


if __name__ == '__main__':
    main()
