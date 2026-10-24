#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Minus Zero launcher: chooses the GODZILLA game folder, keeps the settings and starts the game.

    python launcher/minuszero_launcher.py            the window
    python launcher/minuszero_launcher.py --play     the game with the saved settings, no window

Frozen into MinusZero.exe (packaging/windows/package.sh), the same program also runs run.py
(--run) and the preparation scripts (--script <path> ...), so players need no Python.
Launcher settings are kept in settings.json, the game's display and input settings in
minuszero.ini (the in-game menu, Insert or R3+L2, changes the same file).
"""
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import threading

FROZEN = getattr(sys, 'frozen', False)
PORT = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
DATA = Path(os.environ.get('MZ_DATA_DIR', PORT))
SETTINGS = DATA / 'settings.json'
INI = DATA / 'minuszero.ini'
LOG = DATA / 'user' / 'last_run.log'
TITLE_IDS = ('CUSA02046',)
# The game's languages (game_en, game_fr, game_ns, game_bp) as PS4 system language ids.
LANGUAGES = (('English', '1'), ('Français', '2'), ('Español (Latinoamérica)', '20'), ('Português (Brasil)', '17'))
DISPLAY_MODES = (('Windowed', 'windowed'), ('Borderless', 'borderless'), ('Fullscreen', 'fullscreen'))
FPS_LIMITS = (('60 (the game\'s own)', '60'), ('30', '30'), ('Unlimited', '0'))
# The keyboard's PS4 buttons and their default keys (src/runtime_pad.c key_map); MZ_KEY_MAP carries changes.
KEYS = (('move_up', 'Walk forward', 'W'), ('move_down', 'Walk back', 'S'), ('move_left', 'Walk left', 'A'),
        ('move_right', 'Walk right', 'D'), ('l1', 'Turn left (L1)', 'Q'), ('r1', 'Turn right (R1)', 'E'),
        ('square', 'Normal attack (Square)', 'Mouse Left'), ('triangle', 'Strong attack (Triangle)', 'Mouse Right'),
        ('cross', 'Cross', 'Space'), ('circle', 'Circle', 'Left Shift'), ('l2', 'L2', 'R'), ('r2', 'R2', 'F'),
        ('l3', 'L3', 'Z'), ('r3', 'R3', 'C'), ('options', 'Options (pause)', 'Return'),
        ('touchpad', 'Touchpad', 'Tab'), ('up', 'D-pad up', 'I'), ('down', 'D-pad down', 'K'),
        ('left', 'D-pad left', 'J'), ('right', 'D-pad right', 'L'), ('look_up', 'Camera up', 'Up'),
        ('look_down', 'Camera down', 'Down'), ('look_left', 'Camera left', 'Left'), ('look_right', 'Camera right', 'Right'))
DEFAULTS = {'game_dir': '', 'language': '1', 'unlock_addons': True, 'keys': {}}

sys.path.insert(0, str(PORT / 'scripts'))


def load_settings():
    try:
        values = json.loads(SETTINGS.read_text(encoding='utf-8-sig'))  # a BOM from Notepad or PowerShell is fine
    except (OSError, ValueError):
        values = {}
    return {**DEFAULTS, **{k: v for k, v in values.items() if k in DEFAULTS}}


def save_settings(values):
    SETTINGS.parent.mkdir(parents=True, exist_ok=True)
    SETTINGS.write_text(json.dumps(values, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def read_ini():
    from graphics_options import read_ini as read
    return read(INI)


def write_ini(updates):
    from graphics_options import write_keys
    write_keys(INI, updates)


def game_problem(folder):
    """Why a folder is not a usable GODZILLA dump, or None when it is."""
    if not folder:
        return 'No game folder chosen yet.'
    folder = Path(folder)
    if not (folder / 'eboot.bin').is_file():
        return 'This folder has no eboot.bin: choose the folder of your dump (with eboot.bin, sce_module, sce_sys).'
    if not (folder / 'sce_module' / 'libc.prx').is_file():
        return 'sce_module/libc.prx is missing: the dump is incomplete.'
    from prepare import read_sfo
    sfo = read_sfo(folder / 'sce_sys' / 'param.sfo') or {}
    if sfo.get('TITLE_ID') not in TITLE_IDS and 'GODZILLA' not in str(sfo.get('TITLE', '')).upper():
        return f'This is {sfo.get("TITLE", "another game")} ({sfo.get("TITLE_ID", "?")}), not GODZILLA.'
    return None


def game_version(folder):
    from prepare import read_sfo
    sfo = read_sfo(Path(folder) / 'sce_sys' / 'param.sfo') or {}
    return f'{sfo.get("TITLE_ID", "?")} · version {sfo.get("APP_VER", "?")}'


def game_environment(values):
    """The variables run.py and the game read, from the launcher's settings."""
    env = {**os.environ, 'MZ_GAME_DIR': values['game_dir'], 'MZ_LANGUAGE': values['language'],
           'MZ_UNLOCK_ADDONS': '1' if values['unlock_addons'] else '0', 'MZ_CONFIG': str(INI),
           'MZ_DATA_DIR': str(DATA), 'PYTHONIOENCODING': 'utf-8'}
    changed = {k: v for k, v in values['keys'].items() if v and v != dict((a, d) for a, _, d in KEYS).get(k)}
    if changed:
        # ',' and '=' separate the entries: the runtime knows those two keys as Comma and Equals.
        env['MZ_KEY_MAP'] = ','.join(f'{k}={v.replace(",", "Comma").replace("=", "Equals")}' for k, v in changed.items())
    return env


def game_command():
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT / 'run.py')]


def start_game(values, log_file):
    no_window = subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0
    return subprocess.Popen(game_command(), cwd=PORT, env=game_environment(values), stdin=subprocess.DEVNULL,
                            stdout=log_file, stderr=subprocess.STDOUT, creationflags=no_window)


def play_without_window():
    values = load_settings()
    problem = game_problem(values['game_dir'])
    if problem:
        print(problem, file=sys.stderr)
        return 1
    LOG.parent.mkdir(parents=True, exist_ok=True)
    with open(LOG, 'wb') as log:
        return start_game(values, log).wait()


class Launcher:
    BG, PANEL, TEXT, MUTED, ACCENT = '#0d1117', '#161d27', '#e6f1ff', '#8a9bb0', '#3cb4ff'

    def __init__(self):
        import tkinter as tk
        from tkinter import ttk
        self.tk, self.ttk = tk, ttk
        self.values = load_settings()
        self.process = None
        self.root = tk.Tk()
        self.root.title('Minus Zero — GODZILLA for PC')
        self.root.configure(bg=self.BG)
        # Sizes in 96-DPI pixels, scaled to the display (the process is DPI-aware: crisp text).
        self.scale = self.root.winfo_fpixels('1i') / 96.0
        self.root.minsize(int(760 * self.scale), int(600 * self.scale))
        self.root.geometry(f'{int(820 * self.scale)}x{int(660 * self.scale)}')
        try:
            self.root.iconphoto(True, tk.PhotoImage(file=str(PORT / 'launcher' / 'minuszero.png')))
        except tk.TclError:
            pass
        self.style()
        self.cover_image = None
        self.build()
        self.refresh_game()
        self.root.protocol('WM_DELETE_WINDOW', self.close)

    def style(self):
        s = self.ttk.Style(self.root)
        s.theme_use('clam')
        s.configure('.', background=self.BG, foreground=self.TEXT, fieldbackground=self.PANEL, font=('Segoe UI', 10))
        s.configure('TNotebook', background=self.BG, borderwidth=0)
        s.configure('TNotebook.Tab', background=self.PANEL, foreground=self.MUTED, padding=(16, 6))
        s.map('TNotebook.Tab', background=[('selected', self.BG)], foreground=[('selected', self.ACCENT)])
        s.configure('TFrame', background=self.BG)
        s.configure('Panel.TFrame', background=self.PANEL)
        s.configure('TLabel', background=self.BG, foreground=self.TEXT)
        s.configure('Muted.TLabel', foreground=self.MUTED)
        s.configure('Title.TLabel', font=('Segoe UI Black', 26), foreground=self.ACCENT)
        s.configure('Heading.TLabel', font=('Segoe UI Semibold', 12))
        s.configure('TButton', background=self.PANEL, foreground=self.TEXT, borderwidth=1, padding=(10, 5))
        s.map('TButton', background=[('active', '#22303f')])
        s.configure('Play.TButton', background=self.ACCENT, foreground='#05121d', font=('Segoe UI Black', 14),
                    padding=(28, 10))
        s.map('Play.TButton', background=[('active', '#7fd0ff'), ('disabled', '#2a3a4a')])
        s.configure('TCheckbutton', background=self.BG, foreground=self.TEXT)
        s.configure('TCombobox', fieldbackground=self.PANEL, background=self.PANEL, foreground=self.TEXT,
                    arrowcolor=self.TEXT)
        s.map('TCombobox', fieldbackground=[('readonly', self.PANEL)], foreground=[('readonly', self.TEXT)],
              selectbackground=[('readonly', self.PANEL)], selectforeground=[('readonly', self.TEXT)])
        for option, value in (('background', self.PANEL), ('foreground', self.TEXT),
                              ('selectBackground', self.ACCENT), ('selectForeground', '#05121d')):
            self.root.option_add(f'*TCombobox*Listbox.{option}', value)
        s.configure('TEntry', fieldbackground=self.PANEL, foreground=self.TEXT, insertcolor=self.TEXT)

    def build(self):
        tk, ttk = self.tk, self.ttk
        header = ttk.Frame(self.root, padding=(20, 14, 20, 4))
        header.pack(fill='x')
        ttk.Label(header, text='MINUS ZERO', style='Title.TLabel').pack(side='left')
        ttk.Label(header, text='  GODZILLA (PS4) — native PC port', style='Muted.TLabel').pack(side='left', pady=(12, 0))
        tabs = ttk.Notebook(self.root)
        tabs.pack(fill='both', expand=True, padx=16, pady=8)
        self.home, self.settings_page, self.controls_page, self.advanced = (ttk.Frame(tabs, padding=16) for _ in range(4))
        for page, name in ((self.home, 'Play'), (self.settings_page, 'Settings'), (self.controls_page, 'Controls'),
                           (self.advanced, 'Advanced')):
            tabs.add(page, text=name)
        self.build_home()
        self.build_settings()
        self.build_controls()
        self.build_advanced()

    def build_home(self):
        tk, ttk = self.tk, self.ttk
        # The Play row first: it keeps its place however small the window is.
        bottom = ttk.Frame(self.home)
        bottom.pack(side='bottom', fill='x')
        self.play_button = ttk.Button(bottom, text='PLAY', style='Play.TButton', command=self.play)
        self.play_button.pack(side='right')
        self.run_label = ttk.Label(bottom, text='', style='Muted.TLabel', wraplength=int(480 * self.scale))
        self.run_label.pack(side='left')
        self.cover = tk.Label(self.home, bg=self.BG)
        self.cover.pack(fill='x')
        row = ttk.Frame(self.home)
        row.pack(fill='x', pady=(12, 4))
        ttk.Label(row, text='Game folder', style='Heading.TLabel').pack(side='left')
        ttk.Button(row, text='Browse…', command=self.choose_game).pack(side='right')
        self.folder_label = ttk.Label(self.home, text='', style='Muted.TLabel', wraplength=int(680 * self.scale))
        self.folder_label.pack(fill='x')
        self.status = ttk.Label(self.home, text='', wraplength=int(680 * self.scale))
        self.status.pack(fill='x', pady=(8, 0))

    def labelled(self, parent, text, row, widget):
        self.ttk.Label(parent, text=text).grid(row=row, column=0, sticky='w', pady=6, padx=(0, 16))
        widget.grid(row=row, column=1, sticky='w')

    def choice(self, parent, options, current):
        var = self.tk.StringVar(value=next((label for label, value in options if value == current), options[0][0]))
        box = self.ttk.Combobox(parent, textvariable=var, values=[label for label, _ in options], state='readonly', width=28)
        return var, box

    def build_settings(self):
        tk, ttk = self.tk, self.ttk
        ini = read_ini()
        page = self.settings_page
        self.language_var, box = self.choice(page, LANGUAGES, self.values['language'])
        self.labelled(page, 'Game language', 0, box)
        self.mode_var, box = self.choice(page, DISPLAY_MODES, ini.get('display_mode', 'windowed'))
        self.labelled(page, 'Display mode', 1, box)
        self.vsync_var = tk.BooleanVar(value=ini.get('vsync', '1') != '0')
        self.labelled(page, 'VSync', 2, ttk.Checkbutton(page, variable=self.vsync_var))
        self.fps_var, box = self.choice(page, FPS_LIMITS, ini.get('fps_limit', '60') if ini.get('fps_limit') in ('0', '30', '60') else '60')
        self.labelled(page, 'Frame rate limit', 3, box)
        self.show_fps_var = tk.BooleanVar(value=ini.get('show_fps', '0') == '1')
        self.labelled(page, 'Show the frame rate', 4, ttk.Checkbutton(page, variable=self.show_fps_var))
        self.sensitivity_var = tk.IntVar(value=int(ini.get('mouse_aim_sensitivity', '100') or 100))
        scale = tk.Scale(page, from_=10, to=200, orient='horizontal', variable=self.sensitivity_var, length=240,
                         bg=self.BG, fg=self.TEXT, troughcolor=self.PANEL, highlightthickness=0, activebackground=self.ACCENT)
        self.labelled(page, 'Mouse camera speed (%)', 5, scale)
        self.vibration_var = tk.IntVar(value=int(ini.get('vibration', '100') or 100))
        scale = tk.Scale(page, from_=0, to=100, orient='horizontal', variable=self.vibration_var, length=240,
                         bg=self.BG, fg=self.TEXT, troughcolor=self.PANEL, highlightthickness=0, activebackground=self.ACCENT)
        self.labelled(page, 'Controller vibration (%)', 6, scale)
        self.addons_var = tk.BooleanVar(value=bool(self.values['unlock_addons']))
        self.labelled(page, 'Unlock the add-ons', 7, ttk.Checkbutton(page, variable=self.addons_var))
        ttk.Label(page, text='The pre-order and diorama add-ons are unlocks of content already in the game.',
                  style='Muted.TLabel').grid(row=8, column=0, columnspan=2, sticky='w')
        ttk.Label(page, text='The in-game menu (Insert, or R3 + L2 on a controller) has more options.',
                  style='Muted.TLabel').grid(row=9, column=0, columnspan=2, sticky='w', pady=(16, 0))
        ttk.Button(page, text='Save', command=self.save).grid(row=10, column=1, sticky='e', pady=16)

    def build_controls(self):
        tk, ttk = self.tk, self.ttk
        page = self.controls_page
        ttk.Label(page, text='Keyboard and mouse. A controller works as the PS4 pad without setup.',
                  style='Muted.TLabel').grid(row=0, column=0, columnspan=4, sticky='w', pady=(0, 8))
        ttk.Label(page, text='Key names as SDL names them: Space, Left Shift, Return, Mouse Left, Mouse Right, '
                             'Wheel Up, F1… The mouse also moves the camera.', style='Muted.TLabel',
                  wraplength=700).grid(row=1, column=0, columnspan=4, sticky='w', pady=(0, 10))
        self.key_vars = {}
        for i, (action, label, default) in enumerate(KEYS):
            column = (i // 12) * 2
            var = tk.StringVar(value=self.values['keys'].get(action, default))
            self.key_vars[action] = var
            ttk.Label(page, text=label).grid(row=2 + i % 12, column=column, sticky='w', padx=(0, 8), pady=2)
            ttk.Entry(page, textvariable=var, width=14).grid(row=2 + i % 12, column=column + 1, sticky='w', padx=(0, 24))
        buttons = ttk.Frame(page)
        buttons.grid(row=15, column=0, columnspan=4, sticky='e', pady=12)
        ttk.Button(buttons, text='Defaults', command=self.default_keys).pack(side='left', padx=6)
        ttk.Button(buttons, text='Save', command=self.save).pack(side='left')

    def build_advanced(self):
        ttk = self.ttk
        page = self.advanced
        for text, command in (('Open the data folder (saves, settings, logs)', self.open_data),
                              ('Show the last game log', self.open_log),
                              ('Clear the shader cache (it is rebuilt while you play)', self.clear_cache)):
            ttk.Button(page, text=text, command=command).pack(anchor='w', pady=4)
        ttk.Label(page, text=f'Port folder: {PORT}', style='Muted.TLabel').pack(anchor='w', pady=(16, 0))
        ttk.Label(page, text=f'Data folder: {DATA}', style='Muted.TLabel').pack(anchor='w')

    def default_keys(self):
        for action, _, default in KEYS:
            self.key_vars[action].set(default)

    def collect(self):
        self.values['language'] = dict(LANGUAGES)[self.language_var.get()]
        self.values['unlock_addons'] = bool(self.addons_var.get())
        defaults = {a: d for a, _, d in KEYS}
        self.values['keys'] = {a: v.get().strip() for a, v in self.key_vars.items() if v.get().strip() != defaults[a]}

    def save(self):
        self.collect()
        save_settings(self.values)
        write_ini({'display_mode': dict(DISPLAY_MODES)[self.mode_var.get()], 'vsync': int(self.vsync_var.get()),
                   'fps_limit': dict(FPS_LIMITS)[self.fps_var.get()], 'show_fps': int(self.show_fps_var.get()),
                   'mouse_aim_sensitivity': self.sensitivity_var.get(), 'vibration': self.vibration_var.get()})
        self.run_label.configure(text='Settings saved.')

    def choose_game(self):
        from tkinter import filedialog
        folder = filedialog.askdirectory(title='The GODZILLA folder (eboot.bin, sce_module, sce_sys)',
                                         initialdir=self.values['game_dir'] or None)
        if folder:
            self.values['game_dir'] = str(Path(folder))
            save_settings(self.values)
            self.refresh_game()

    def refresh_game(self):
        folder = self.values['game_dir']
        problem = game_problem(folder)
        self.folder_label.configure(text=folder or '—')
        self.status.configure(text=problem or f'Ready: {game_version(folder)}',
                              foreground='#ff8a7a' if problem else self.ACCENT)
        self.play_button.state(['disabled'] if problem else ['!disabled'])
        self.show_cover(None if problem else Path(folder) / 'sce_sys' / 'pic1.png')

    def show_cover(self, path):
        """The game's own background picture (from the player's dump; none ships with the port)."""
        self.cover_image = None
        try:
            from PIL import Image, ImageTk
            if path and path.is_file():
                with Image.open(path) as image:
                    image = image.convert('RGB')
                    image.thumbnail((int(640 * self.scale), int(220 * self.scale)))
                    self.cover_image = ImageTk.PhotoImage(image)
        except (ImportError, OSError):
            pass
        if self.cover_image is None:
            try:
                self.cover_image = self.tk.PhotoImage(file=str(PORT / 'launcher' / 'minuszero.png'))
            except self.tk.TclError:
                self.cover_image = None
        self.cover.configure(image=self.cover_image or '')

    def play(self):
        if self.process and self.process.poll() is None:
            return
        self.save()
        LOG.parent.mkdir(parents=True, exist_ok=True)
        log = open(LOG, 'wb')
        try:
            self.process = start_game(self.values, log)
        except OSError as error:
            log.close()
            self.run_label.configure(text=f'Could not start: {error}')
            return
        self.play_button.state(['disabled'])
        self.run_label.configure(text='GODZILLA is running… (the first start prepares the game image)')
        threading.Thread(target=self.wait, args=(self.process, log), daemon=True).start()

    def wait(self, process, log):
        code = process.wait()
        log.close()
        self.root.after(0, self.finished, code)

    def finished(self, code):
        self.play_button.state(['!disabled'])
        text = 'The game closed.' if code in (0, 1, None) else f'The game stopped (code {code}): see Advanced → Show the last game log.'
        self.run_label.configure(text=text)

    def open_data(self):
        DATA.mkdir(parents=True, exist_ok=True)
        os.startfile(DATA) if sys.platform == 'win32' else subprocess.Popen(['xdg-open', str(DATA)])

    def open_log(self):
        if LOG.is_file():
            os.startfile(LOG) if sys.platform == 'win32' else subprocess.Popen(['xdg-open', str(LOG)])
        else:
            self.run_label.configure(text='No game log yet.')

    def clear_cache(self):
        import shutil
        removed = 0
        for cache in (DATA / 'user' / 'cache', DATA / 'user' / 'shader_cache'):
            if cache.is_dir():
                shutil.rmtree(cache, ignore_errors=True)
                removed += 1
        self.run_label.configure(text='Shader cache cleared.' if removed else 'There was no shader cache.')

    def close(self):
        if self.process and self.process.poll() is None:
            from tkinter import messagebox
            if not messagebox.askyesno('Minus Zero', 'The game is still running. Close the launcher anyway?'):
                return
        self.root.destroy()

    def run(self):
        self.root.mainloop()


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--script':
        # The preparation scripts, run by run.py through this executable when frozen.
        sys.argv = args[1:]
        sys.path.insert(0, str(Path(args[1]).resolve().parent))
        runpy.run_path(args[1], run_name='__main__')
        return 0
    if args and args[0] == '--run':
        sys.argv = [str(PORT / 'run.py'), *args[1:]]
        runpy.run_path(str(PORT / 'run.py'), run_name='__main__')
        return 0
    if args and args[0] == '--play':
        return play_without_window()
    if sys.platform == 'win32':
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    Launcher().run()
    return 0


if __name__ == '__main__':
    sys.exit(main())
