#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Minus Two launcher: chooses the GODZILLA game folder, keeps the settings and starts the game.

    python launcher/minustwo_launcher.py            the window
    python launcher/minustwo_launcher.py --play     the game with the saved settings, no window

Frozen into MinusTwo.exe (packaging/windows/package.sh), the same program also runs run.py
(--run) and the preparation scripts (--script <path> ...), so players need no Python.
Launcher settings are kept in settings.json, the game's display and input settings in
minustwo.ini (the in-game menu, Insert or R3+L2, changes the same file).
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
INI = DATA / 'minustwo.ini'
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
DEFAULTS = {'game_dir': '', 'language': '1', 'unlock_addons': True, 'keys': {}, 'mods_enabled': True}
MODS, MODS_CONFIG, MOD_PRESETS = DATA / 'mods', DATA / 'mods.json', DATA / 'mods_presets.json'
PATCHES, PATCHES_CONFIG = DATA / 'patches', DATA / 'patches.json'

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
           'MZ_DATA_DIR': str(DATA), 'MZ_MODS_ENABLED': '1' if values.get('mods_enabled', True) else '0',
           'PYTHONIOENCODING': 'utf-8'}
    changed = {k: v for k, v in values['keys'].items() if v and v != dict((a, d) for a, _, d in KEYS).get(k)}
    if changed:
        # ',' and '=' separate the entries: the runtime knows those two keys as Comma and Equals.
        env['MZ_KEY_MAP'] = ','.join(f'{k}={v.replace(",", "Comma").replace("=", "Equals")}' for k, v in changed.items())
    return env


def load_json(path, default):
    try:
        value = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return default
    return value if isinstance(value, type(default)) else default


def write_json(path, value):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


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


PAGES = (('play', 'Play'), ('graphics', 'Graphics'), ('display', 'Display & FPS'), ('game', 'Game'),
         ('controls', 'Controls'), ('mods', 'Mods & patches'), ('achievements', 'Achievements'),
         ('advanced', 'Advanced'))
ANISOTROPY = (('Game default', '0'), ('2x', '2'), ('4x', '4'), ('8x', '8'), ('16x', '16'))
# GODZILLA's look: near black, the atomic blue of its menus and HUD, the red of the ゴジラ in its logo.
BG, PANEL, CARD, LINE = '#06080c', '#0b0f15', '#121a24', '#1d2836'
TEXT, MUTED, ACCENT, ACCENT_HI, RED = '#e6edf5', '#7f8da0', '#1680d8', '#4cb4ff', '#d3202a'
WARN = '#f0a640'  # a file two mods change
GRADE_COLOURS = {'Platinum': '#a9d8f5', 'Gold': '#e2b44c', 'Silver': '#c3cad3', 'Bronze': '#c98552'}


def display_refresh():
    """(width, height, refresh rate) of the primary display's current mode, or None (Windows only)."""
    if sys.platform != 'win32':
        return None
    import ctypes
    from ctypes import wintypes

    class DEVMODEW(ctypes.Structure):
        _fields_ = [('dmDeviceName', wintypes.WCHAR * 32), ('dmSpecVersion', wintypes.WORD),
                    ('dmDriverVersion', wintypes.WORD), ('dmSize', wintypes.WORD), ('dmDriverExtra', wintypes.WORD),
                    ('dmFields', wintypes.DWORD), ('dmPosition', wintypes.LONG * 2), ('dmOrientation', wintypes.DWORD),
                    ('dmFixedOutput', wintypes.DWORD), ('dmColor', ctypes.c_short), ('dmDuplex', ctypes.c_short),
                    ('dmYResolution', ctypes.c_short), ('dmTTOption', ctypes.c_short), ('dmCollate', ctypes.c_short),
                    ('dmFormName', wintypes.WCHAR * 32), ('dmLogPixels', wintypes.WORD), ('dmBitsPerPel', wintypes.DWORD),
                    ('dmPelsWidth', wintypes.DWORD), ('dmPelsHeight', wintypes.DWORD), ('dmDisplayFlags', wintypes.DWORD),
                    ('dmDisplayFrequency', wintypes.DWORD)]
    mode = DEVMODEW()
    mode.dmSize = ctypes.sizeof(DEVMODEW)
    try:
        if ctypes.windll.user32.EnumDisplaySettingsW(None, -1, ctypes.byref(mode)):
            return mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency
    except (AttributeError, OSError):
        pass
    return None


def graphics_card():
    """The graphics card the game will use, from the port's Vulkan check (bin/minustwo-gpu-capabilities):
    its name, '' when the check found none, None when the check could not run."""
    tool = PORT / 'bin' / 'minustwo-gpu-capabilities.exe'
    if not tool.is_file():
        tool = PORT / 'out' / 'minustwo-gpu-capabilities.exe'
    try:
        done = subprocess.run([str(tool)], capture_output=True, text=True, timeout=10,
                              creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
    except (OSError, subprocess.SubprocessError):
        return None
    if done.returncode != 0:
        return None
    for line in (done.stdout + done.stderr).splitlines():  # it reports on stderr
        if line.startswith('GPU scene scaling: '):
            name = line[len('GPU scene scaling: '):]
            for tail in (' supports', ' does not', ' cannot'):
                name = name.split(tail)[0]
            return name.strip()
    return ''


class Launcher:
    """The window: GODZILLA's pages in a sidebar (as the God of War III and Bloodborne ports have
    them), the Play page with the game's key art, a ready check and quick settings, and the PLAY bar."""

    def __init__(self):
        import tkinter as tk
        from tkinter import ttk
        import tkinter.font as tkfont
        self.tk, self.ttk = tk, ttk
        self.values = load_settings()
        self.ini = read_ini()
        self.process = None
        self.root = tk.Tk()
        self.root.withdraw()
        self.root.title('GODZILLA — Minus Two')
        self.root.configure(bg=BG)
        families = set(tkfont.families(self.root))
        pick = lambda *names: next((n for n in names if n in families), 'Segoe UI')  # noqa: E731
        self.title_face = pick('Impact', 'Bahnschrift SemiBold Condensed', 'Segoe UI Black')
        self.nav_face = pick('Bahnschrift SemiBold', 'Segoe UI Semibold')
        self.kana_face = pick('Yu Gothic UI Semibold', 'Yu Gothic UI', 'Meiryo', 'Segoe UI')
        # Sizes in 96-DPI pixels, scaled to the display (the process is DPI-aware: crisp text).
        self.scale = self.root.winfo_fpixels('1i') / 96.0
        width, height = int(1120 * self.scale), int(760 * self.scale)
        width = min(width, int(self.root.winfo_screenwidth() * 0.9))
        height = min(height, int(self.root.winfo_screenheight() * 0.9))
        self.root.geometry(f'{width}x{height}+{(self.root.winfo_screenwidth() - width) // 2}'
                           f'+{(self.root.winfo_screenheight() - height) // 2}')
        self.root.minsize(min(self.px(960), width), min(self.px(640), height))
        try:
            self.root.iconphoto(True, tk.PhotoImage(file=str(PORT / 'launcher' / 'minustwo.png')))
        except tk.TclError:
            pass
        self.style()
        self.art = None          # the key art (PIL image) of the chosen dump
        self.banner_photo = None
        self.build()
        self.refresh_game()
        self.root.protocol('WM_DELETE_WINDOW', self.close)
        self.root.deiconify()
        self.dark_title_bar()
        # The card check runs on a thread; the window picks its answer up (Tk is not called from threads).
        import queue
        self.card_answer = queue.Queue()
        threading.Thread(target=lambda: self.card_answer.put(graphics_card()), daemon=True).start()
        self.root.after(100, self.poll_card)

    def px(self, value):
        return int(round(value * self.scale))

    def font(self, size, weight='normal', face='Segoe UI'):
        return (face, size, weight) if weight != 'normal' else (face, size)

    def dark_title_bar(self):
        """Windows 11: a title bar in the window's own black instead of the light default."""
        if sys.platform != 'win32':
            return
        try:
            import ctypes
            self.root.update_idletasks()
            hwnd = ctypes.windll.user32.GetParent(self.root.winfo_id())
            on, caption = ctypes.c_int(1), ctypes.c_int(int(BG[5:7] + BG[3:5] + BG[1:3], 16))  # COLORREF: 0x00BBGGRR
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(on), 4)        # immersive dark mode
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 35, ctypes.byref(caption), 4)   # caption colour
        except (AttributeError, OSError):
            pass

    def check_images(self):
        """Check box images, 18 px scaled: a dark box, and the accent with a tick when set."""
        n = self.px(18)
        images = []
        for checked in (False, True):
            image = self.tk.PhotoImage(width=n + self.px(10), height=n)
            image.put(ACCENT_HI if checked else '#3a4a5c', to=(0, 0, n, n))
            border = max(1, self.px(1))
            image.put(ACCENT if checked else CARD, to=(border, border, n - border, n - border))
            if checked:  # a tick from (0.22n, 0.52n) down to (0.42n, 0.72n), up to (0.78n, 0.28n)
                width = max(2, round(n / 8))
                x0, y0, x1, y1, x2, y2 = .22 * n, .52 * n, .42 * n, .72 * n, .78 * n, .28 * n
                for t in range(60):
                    f = t / 59
                    for x, y in ((x0 + (x1 - x0) * f, y0 + (y1 - y0) * f), (x1 + (x2 - x1) * f, y1 + (y2 - y1) * f)):
                        image.put('#f2f8ff', to=(int(x), int(y), int(x) + width, int(y) + width))
            images.append(image)
        self.check_off, self.check_on = images

    def style(self):
        s = self.ttk.Style(self.root)
        s.theme_use('clam')
        self.check_images()
        s.element_create('Mz.indicator', 'image', self.check_off, ('selected', self.check_on), sticky='')
        s.layout('TCheckbutton', [('Checkbutton.padding', {'sticky': 'nswe', 'children': [
            ('Mz.indicator', {'side': 'left', 'sticky': ''}),
            ('Checkbutton.focus', {'side': 'left', 'sticky': 'w', 'children': [
                ('Checkbutton.label', {'sticky': 'nswe'})]})]})])
        base = self.font(10)
        s.configure('.', background=PANEL, foreground=TEXT, fieldbackground=CARD, bordercolor=LINE, lightcolor=LINE,
                    darkcolor=LINE, troughcolor=CARD, focuscolor=ACCENT, font=base)
        s.configure('TFrame', background=PANEL)
        s.configure('TLabel', background=PANEL, foreground=TEXT)
        s.configure('Muted.TLabel', foreground=MUTED, font=self.font(9))
        s.configure('Section.TLabel', foreground=ACCENT_HI, font=(self.title_face, 15))
        s.configure('Heading.TLabel', font=(self.title_face, 24))
        s.configure('Version.TLabel', foreground=ACCENT_HI, font=self.font(12, 'bold', 'Segoe UI Semibold'))
        s.configure('Check.TLabel', font=self.font(11))
        s.configure('TButton', background=CARD, foreground=TEXT, padding=self.px(6), borderwidth=self.px(1))
        s.map('TButton', background=[('active', LINE), ('disabled', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Play.TButton', background=ACCENT, foreground='#f2f8ff', font=(self.title_face, 20),
                    padding=(self.px(46), self.px(4)), borderwidth=0)
        s.map('Play.TButton', background=[('active', ACCENT_HI), ('disabled', '#1a2a3a')],
              foreground=[('disabled', MUTED)])
        s.configure('TCheckbutton', background=PANEL, foreground=TEXT, padding=(0, self.px(3)))
        s.map('TCheckbutton', background=[('active', PANEL)])
        s.configure('TCombobox', arrowcolor=MUTED, foreground=TEXT, padding=self.px(4), arrowsize=self.px(14))
        s.map('TCombobox', fieldbackground=[('readonly', CARD)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', CARD)], selectforeground=[('readonly', TEXT)])
        for option, value in (('background', CARD), ('foreground', TEXT), ('selectBackground', ACCENT),
                              ('selectForeground', '#f2f8ff'), ('font', base)):
            self.root.option_add(f'*TCombobox*Listbox.{option}', value)
        s.configure('TEntry', fieldbackground=CARD, foreground=TEXT, insertcolor=TEXT, padding=self.px(4))
        s.configure('Trophy.Horizontal.TProgressbar', background=ACCENT, troughcolor=CARD, bordercolor=LINE,
                    lightcolor=ACCENT_HI, darkcolor=ACCENT, thickness=self.px(10))
        s.configure('Vertical.TScrollbar', background='#26364a', troughcolor=PANEL, bordercolor=PANEL, arrowcolor=MUTED,
                    lightcolor='#26364a', darkcolor='#26364a', arrowsize=self.px(14))
        s.map('Vertical.TScrollbar', background=[('active', '#33475f')])
        s.configure('Horizontal.TScale', background=ACCENT, troughcolor=CARD, bordercolor=LINE, lightcolor=ACCENT_HI,
                    darkcolor=ACCENT, sliderlength=self.px(14), gripcount=0)
        s.map('Horizontal.TScale', background=[('active', ACCENT_HI)])

    # ---- layout -------------------------------------------------------------------------------
    def build(self):
        tk = self.tk
        side = tk.Frame(self.root, bg=BG, width=self.px(236))
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        brand = tk.Frame(side, bg=BG)
        brand.pack(anchor='w', padx=self.px(24), pady=(self.px(22), self.px(20)))
        tk.Label(brand, text='ゴジラ', bg=BG, fg=RED, font=(self.kana_face, 11, 'bold')).pack(anchor='w')
        tk.Label(brand, text='GODZILLA', bg=BG, fg=TEXT, font=(self.title_face, 31)).pack(anchor='w')
        tk.Label(brand, text='Minus Two · native PC port', bg=BG, fg=MUTED, font=self.font(9)).pack(anchor='w')
        self.nav, self.current = {}, None
        for name, title in PAGES:
            row = tk.Frame(side, bg=BG, cursor='hand2')
            row.pack(fill='x')
            bar = tk.Frame(row, bg=BG, width=self.px(4))
            bar.pack(side='left', fill='y')
            label = tk.Label(row, text=title, bg=BG, fg=TEXT, anchor='w', font=(self.nav_face, 13),
                             padx=self.px(20), pady=self.px(9))
            label.pack(side='left', fill='x', expand=True)
            for widget in (row, label):
                widget.bind('<Button-1>', lambda _e, n=name: self.show(n))
                widget.bind('<Enter>', lambda _e, n=name: self.hover(n, True))
                widget.bind('<Leave>', lambda _e, n=name: self.hover(n, False))
            self.nav[name] = (row, bar, label)
        tk.Frame(side, bg=BG).pack(fill='both', expand=True)
        tk.Label(side, text='In the game: Insert or R3+L2\nopens the port\'s menu.', bg=BG, fg=MUTED,
                 font=self.font(9), justify='left').pack(anchor='w', padx=self.px(24), pady=(0, self.px(18)))

        right = tk.Frame(self.root, bg=PANEL)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=BG, height=self.px(76))
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        self.summary = tk.Label(bar, text='', bg=BG, fg=MUTED, font=self.font(10), anchor='w')
        self.summary.pack(side='left', padx=self.px(26))
        self.play_button = self.ttk.Button(bar, text='PLAY', style='Play.TButton', command=self.play)
        self.play_button.pack(side='right', padx=(self.px(10), self.px(26)), pady=self.px(12))
        self.ttk.Button(bar, text='Save', command=self.save).pack(side='right', pady=self.px(12))
        self.content = tk.Frame(right, bg=PANEL)
        self.content.pack(fill='both', expand=True)

        self.pages = {}
        self.build_play()
        self.build_graphics()
        self.build_display()
        self.build_game()
        self.build_controls()
        self.build_mods()
        self.build_achievements()
        self.build_advanced()
        self.root.bind_all('<MouseWheel>', self.wheel)
        for var in (self.mode_var, self.resolution_var, self.vsync_var, self.match_refresh_var, self.fps_var):
            var.trace_add('write', lambda *_a: self.update_summary())
        self.update_summary()
        self.show('play')

    def hover(self, name, inside):
        if name != self.current:
            row, bar, label = self.nav[name]
            for widget in (row, label):
                widget.configure(bg='#0f141c' if inside else BG)

    def show(self, name):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current = name
        for n, (row, bar, label) in self.nav.items():
            active = n == name
            row.configure(bg=PANEL if active else BG)
            label.configure(bg=PANEL if active else BG, fg=ACCENT_HI if active else TEXT)
            bar.configure(bg=ACCENT_HI if active else BG)
        if name == 'achievements':
            self.refresh_achievements()  # the game may have unlocked some since
        if name == 'mods':
            self.refresh_mods()  # mods may have been added to the folder since

    def page(self, name, title, subtitle):
        """A settings page: its title, a line under it, then rows (grid, two columns)."""
        frame = self.ttk.Frame(self.content, padding=(self.px(32), self.px(26), self.px(32), self.px(16)))
        self.ttk.Label(frame, text=title.upper(), style='Heading.TLabel').grid(row=0, column=0, columnspan=2, sticky='w')
        self.ttk.Label(frame, text=subtitle, style='Muted.TLabel').grid(row=1, column=0, columnspan=2, sticky='w')
        frame.next_row = 2
        self.pages[name] = frame
        return frame

    def section(self, parent, title, column=0, columnspan=2):
        row = parent.next_row
        self.ttk.Label(parent, text=title.upper(), style='Section.TLabel').grid(
            row=row, column=column, columnspan=columnspan, sticky='w', pady=(self.px(22), 0))
        self.tk.Frame(parent, bg=LINE, height=self.px(1)).grid(row=row + 1, column=column, columnspan=columnspan,
                                                               sticky='we', pady=(self.px(2), self.px(8)))
        parent.next_row = row + 2

    def row(self, parent, text, widget, hint=None):
        r = parent.next_row
        self.ttk.Label(parent, text=text).grid(row=r, column=0, sticky='w', pady=self.px(5), padx=(0, self.px(24)))
        widget.grid(row=r, column=1, sticky='w')
        parent.next_row = r + 1
        if hint:
            self.ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
                row=parent.next_row, column=1, sticky='w', pady=(0, self.px(6)))
            parent.next_row += 1

    def choice(self, parent, options, current, width=30, var=None):
        var = var or self.tk.StringVar(value=next((label for label, value in options if value == current), options[0][0]))
        box = self.ttk.Combobox(parent, textvariable=var, values=[label for label, _ in options], state='readonly',
                                width=width)
        return var, box

    def check(self, parent, var):
        return self.ttk.Checkbutton(parent, variable=var)

    def slider(self, parent, var, low, high):
        holder = self.ttk.Frame(parent)
        self.ttk.Scale(holder, from_=low, to=high, orient='horizontal', variable=var, length=self.px(240),
                       command=lambda _v: var.set(int(float(_v)))).pack(side='left')
        self.ttk.Label(holder, textvariable=var, width=4).pack(side='left', padx=(self.px(10), 0))
        return holder

    # ---- the Play page ------------------------------------------------------------------------
    def build_play(self):
        tk, ttk = self.tk, self.ttk
        page = tk.Frame(self.content, bg=PANEL)
        self.pages['play'] = page
        self.banner = tk.Canvas(page, height=self.px(330), bg=BG, highlightthickness=0, bd=0)
        self.banner.pack(fill='x')
        self.banner.bind('<Configure>', lambda _e: self.draw_banner())
        self.choose_button = ttk.Button(self.banner, text='Choose the GODZILLA folder…', command=self.choose_game)
        body = ttk.Frame(page, padding=(self.px(30), self.px(10), self.px(30), self.px(10)))
        body.pack(fill='both', expand=True)
        body.columnconfigure(0, weight=3)
        body.columnconfigure(1, weight=2)
        self.version_label = ttk.Label(body, text='', style='Version.TLabel')
        self.version_label.grid(row=0, column=0, columnspan=2, sticky='w')

        ready = ttk.Frame(body)
        ready.grid(row=1, column=0, sticky='nwe', padx=(0, self.px(30)))
        ready.next_row = 0
        self.section(ready, 'Ready check')
        self.checks = ttk.Frame(ready)
        self.checks.grid(row=ready.next_row, column=0, columnspan=2, sticky='w')
        folder = ttk.Frame(ready)
        folder.grid(row=ready.next_row + 1, column=0, columnspan=2, sticky='w', pady=(self.px(10), 0))
        ttk.Button(folder, text='Change the game folder…', command=self.choose_game).pack(side='left')

        quick = ttk.Frame(body)
        quick.grid(row=1, column=1, sticky='nwe')
        quick.next_row = 0
        self.section(quick, 'Quick settings')
        ini = self.ini
        self.mode_var, box = self.choice(quick, DISPLAY_MODES, ini.get('display_mode', 'windowed'), width=24)
        self.row(quick, 'Display mode', box)
        from graphics_options import RESOLUTIONS, resolution_label
        self.resolution_pairs = tuple((resolution_label(name), name) for name in RESOLUTIONS)
        current = ini.get('render_resolution', 'native')
        self.resolution_var, box = self.choice(quick, self.resolution_pairs,
                                               current if current in RESOLUTIONS else 'native', width=24)
        self.row(quick, 'Render resolution', box)
        self.vsync_var = tk.BooleanVar(value=ini.get('vsync', '1') != '0')
        self.row(quick, 'VSync', self.check(quick, self.vsync_var))
        self.match_refresh_var = tk.BooleanVar(value=ini.get('match_refresh', '1') != '0')
        self.row(quick, 'Match refresh rate', self.check(quick, self.match_refresh_var))

    def ready_lines(self):
        """(mark, text, colour) of each line of the ready check."""
        folder = self.values['game_dir']
        problem = game_problem(folder)
        lines = [('✗', problem, RED) if problem else ('✓', f'GODZILLA found: {folder}', ACCENT_HI)]
        saves = DATA / 'user' / 'savedata'
        if saves.is_dir() and any(saves.rglob('SAVEDATA*')):
            lines.append(('✓', f'Saves found in {DATA / "user"}', ACCENT_HI))
        else:
            lines.append(('•', 'No saves yet: the game creates them in user\\savedata', MUTED))
        card = getattr(self, 'card', 'checking')
        if card == 'checking':
            lines.append(('•', 'Graphics card: checking…', MUTED))
        elif card:
            lines.append(('✓', f'Graphics card: {card}', ACCENT_HI))
        elif card == '':
            lines.append(('✗', 'Graphics card: no Vulkan 1.3 card found (update the driver)', RED))
        display = display_refresh()
        if display:
            w, h, hz = display
            text = f'Display: {w} × {h} at {hz} Hz'
            if self.match_refresh_var.get() and hz % 60 and 120 <= hz and self.mode_var.get() != 'Windowed':
                text += ' (120 Hz while playing: every frame lasts the same)'
            lines.append(('✓', text, ACCENT_HI))
        addons = self.addons_var.get() if hasattr(self, 'addons_var') else self.values['unlock_addons']
        lines.append(('✓' if addons else '•', 'Add-ons unlocked (pre-order and diorama sets)' if addons
                      else 'Add-ons locked (Game page)', ACCENT_HI if addons else MUTED))
        return lines

    def refresh_checks(self):
        for widget in self.checks.winfo_children():
            widget.destroy()
        for i, (mark, text, colour) in enumerate(self.ready_lines()):
            self.tk.Label(self.checks, text=mark, bg=PANEL, fg=colour, font=self.font(12, 'bold')).grid(
                row=i, column=0, sticky='nw', padx=(0, self.px(10)), pady=self.px(2))
            self.ttk.Label(self.checks, text=text, style='Check.TLabel', wraplength=self.px(520),
                           justify='left').grid(row=i, column=1, sticky='w', pady=self.px(2))

    def poll_card(self):
        import queue
        try:
            self.card = self.card_answer.get_nowait()
        except queue.Empty:
            self.root.after(100, self.poll_card)
            return
        self.refresh_checks()

    def draw_banner(self):
        """The game's key art (sce_sys/pic1.png of the player's dump; none ships with the port), cropped
        to the banner and fading into the page."""
        c = self.banner
        width, height = max(c.winfo_width(), 1), max(c.winfo_height(), 1)
        if getattr(self, 'banner_key', None) == (id(self.art), width, height):
            return
        self.banner_key = (id(self.art), width, height)
        c.delete('all')
        self.banner_photo = None
        if self.art is not None:
            try:
                from PIL import Image, ImageTk
                art = self.art
                scale = max(width / art.width, height / art.height)
                size = (max(1, int(art.width * scale)), max(1, int(art.height * scale)))
                image = art.resize(size, Image.LANCZOS)
                top = max(0, int((size[1] - height) * 0.45))
                left = (size[0] - width) // 2
                image = image.crop((left, top, left + width, top + height)).convert('RGBA')
                fade = Image.new('RGBA', (width, height))
                panel = tuple(int(PANEL[i:i + 2], 16) for i in (1, 3, 5))
                start = int(height * 0.62)
                for y in range(start, height):
                    alpha = int(255 * ((y - start) / max(1, height - start)) ** 1.6)
                    fade.paste((*panel, alpha), (0, y, width, y + 1))
                self.banner_photo = ImageTk.PhotoImage(Image.alpha_composite(image, fade))
                c.create_image(0, 0, image=self.banner_photo, anchor='nw')
            except (ImportError, OSError, ValueError):
                self.banner_photo = None
        if self.banner_photo is None:
            c.create_text(width // 2, height // 2 - self.px(40), text='GODZILLA', fill=LINE,
                          font=(self.title_face, 72))
            c.create_window(width // 2, height // 2 + self.px(40), window=self.choose_button)

    def load_art(self, folder):
        self.art = None
        try:
            from PIL import Image
            path = Path(folder) / 'sce_sys' / 'pic1.png'
            if path.is_file():
                with Image.open(path) as image:
                    self.art = image.convert('RGB')
        except (ImportError, OSError):
            self.art = None

    # ---- the other pages ----------------------------------------------------------------------
    def build_graphics(self):
        page = self.page('graphics', 'Graphics', 'How the game is drawn. The game restarts with a new render resolution.')
        self.section(page, 'Image')
        _, box = self.choice(page, self.resolution_pairs, None, width=36, var=self.resolution_var)
        self.row(page, 'Render resolution', box,
                 'Draws the whole frame, interface included, at this size on the GPU. 4K suits a fast card.')
        self.anisotropy_var, box = self.choice(page, ANISOTROPY, self.ini.get('anisotropy', '0'), width=18)
        self.row(page, 'Anisotropic filtering', box, 'Sharper textures at a glancing angle.')
        self.ttk.Label(page, text='The in-game menu (Insert, or R3 + L2) has the sharpening and the overlay.',
                       style='Muted.TLabel').grid(row=page.next_row, column=0, columnspan=2, sticky='w',
                                                  pady=(self.px(18), 0))

    def build_display(self):
        tk = self.tk
        page = self.page('display', 'Display & FPS', 'The window, the refresh rate and the frame rate.')
        self.section(page, 'Window')
        _, box = self.choice(page, DISPLAY_MODES, None, width=24, var=self.mode_var)
        self.row(page, 'Display mode', box)
        self.row(page, 'VSync', self.check(page, self.vsync_var), 'Waits for the display: no tearing.')
        self.row(page, 'Match refresh rate', self.check(page, self.match_refresh_var),
                 'Borderless and fullscreen: a display whose refresh rate is not a multiple of 60 (144 Hz) runs at '
                 '120 Hz while the game runs, and the game\'s frames follow its refreshes: every frame lasts the same.')
        self.section(page, 'Frame rate')
        fps = self.ini.get('fps_limit', '60')
        self.fps_var, box = self.choice(page, FPS_LIMITS, fps if fps in ('0', '30', '60') else '60', width=24)
        self.row(page, 'Frame rate limit', box,
                 'GODZILLA runs its world in 60 steps a second: above 60 FPS it would run faster, not smoother.')
        self.show_fps_var = tk.BooleanVar(value=self.ini.get('show_fps', '0') == '1')
        self.row(page, 'Show the frame rate', self.check(page, self.show_fps_var))

    def build_game(self):
        tk = self.tk
        page = self.page('game', 'Game', 'Language, add-ons and the controller.')
        self.section(page, 'Game')
        self.language_var, box = self.choice(page, LANGUAGES, self.values['language'], width=26)
        self.row(page, 'Game language', box)
        self.addons_var = tk.BooleanVar(value=bool(self.values['unlock_addons']))
        self.addons_var.trace_add('write', lambda *_a: self.refresh_checks())
        self.row(page, 'Unlock the add-ons', self.check(page, self.addons_var),
                 'The pre-order and diorama add-ons are unlocks of content already in the game.')
        self.section(page, 'Controls')
        self.vibration_var = tk.IntVar(value=int(self.ini.get('vibration', '100') or 100))
        self.row(page, 'Controller vibration (%)', self.slider(page, self.vibration_var, 0, 100))
        self.sensitivity_var = tk.IntVar(value=int(self.ini.get('mouse_aim_sensitivity', '100') or 100))
        self.row(page, 'Mouse camera speed (%)', self.slider(page, self.sensitivity_var, 10, 200))

    def build_controls(self):
        tk, ttk = self.tk, self.ttk
        page = self.page('controls', 'Controls', 'Keyboard and mouse. A controller works as the PS4 pad without setup.')
        page.columnconfigure(3, weight=1)
        ttk.Label(page, text='Key names as SDL names them: Space, Left Shift, Return, Mouse Left, Mouse Right, '
                             'Wheel Up, F1… The mouse also moves the camera.', style='Muted.TLabel',
                  wraplength=self.px(760)).grid(row=2, column=0, columnspan=4, sticky='w', pady=(self.px(10), self.px(10)))
        self.key_vars = {}
        for i, (action, label, default) in enumerate(KEYS):
            column = (i // 12) * 2
            var = tk.StringVar(value=self.values['keys'].get(action, default))
            self.key_vars[action] = var
            ttk.Label(page, text=label).grid(row=3 + i % 12, column=column, sticky='w', padx=(0, self.px(10)),
                                             pady=self.px(2))
            ttk.Entry(page, textvariable=var, width=14).grid(row=3 + i % 12, column=column + 1, sticky='w',
                                                             padx=(0, self.px(28)))
        ttk.Button(page, text='Default keys', command=self.default_keys).grid(row=16, column=0, sticky='w',
                                                                              pady=self.px(14))

    # ---- the Mods & patches page --------------------------------------------------------------
    def build_mods(self):
        """Loose-file mods layered over the game when it starts (scripts/mods.py), in a load order with
        a details panel for the selected mod, presets, and third-party eboot patches (patches.py)."""
        tk, ttk = self.tk, self.ttk
        outer = ttk.Frame(self.content)
        self.pages['mods'] = outer
        self.mods_canvas = tk.Canvas(outer, bg=PANEL, highlightthickness=0, bd=0)
        bar = ttk.Scrollbar(outer, orient='vertical', command=self.mods_canvas.yview)
        page = ttk.Frame(self.mods_canvas, padding=(self.px(32), self.px(26), self.px(32), self.px(24)))
        window = self.mods_canvas.create_window(0, 0, window=page, anchor='nw')
        page.bind('<Configure>', lambda _e: self.mods_canvas.configure(scrollregion=self.mods_canvas.bbox('all')))
        self.mods_canvas.bind('<Configure>', lambda e: self.mods_canvas.itemconfigure(window, width=e.width))
        self.mods_canvas.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        self.mods_canvas.pack(side='left', fill='both', expand=True)
        page.columnconfigure(0, weight=1)
        ttk.Label(page, text='MODS & PATCHES', style='Heading.TLabel').grid(row=0, column=0, sticky='w')
        ttk.Label(page, text='Change textures, sounds and text, or the game\'s code. Your game files stay untouched.',
                  style='Muted.TLabel').grid(row=1, column=0, sticky='w')

        top = ttk.Frame(page)
        top.grid(row=2, column=0, sticky='we', pady=(self.px(16), 0))
        self.mods_enabled_var = tk.BooleanVar(value=bool(self.values.get('mods_enabled', True)))
        ttk.Checkbutton(top, text='Load mods', variable=self.mods_enabled_var,
                        command=self.refresh_mod_marks).pack(side='left')
        self.mods_count = ttk.Label(top, text='', style='Muted.TLabel')
        self.mods_count.pack(side='left', padx=(self.px(18), 0))
        ttk.Button(top, text='Refresh', command=self.refresh_mods).pack(side='right')
        ttk.Button(top, text='Open mods folder', command=lambda: self.open_folder(MODS)).pack(
            side='right', padx=(0, self.px(8)))

        head = ttk.Frame(page)
        head.grid(row=3, column=0, sticky='we', pady=(self.px(22), 0))
        ttk.Label(head, text='LOAD ORDER', style='Section.TLabel').pack(side='left')
        ttk.Label(head, text='Lower in the list wins', style='Muted.TLabel').pack(side='right')
        tk.Frame(page, bg=LINE, height=self.px(1)).grid(row=4, column=0, sticky='we', pady=(self.px(2), self.px(10)))
        self.mods_body = ttk.Frame(page)
        self.mods_body.grid(row=5, column=0, sticky='we')
        self.mods_body.columnconfigure(0, weight=1)
        left = ttk.Frame(self.mods_body)
        left.grid(row=0, column=0, sticky='new')
        self.mods_list = tk.Frame(left, bg=CARD)
        self.mods_list.pack(fill='x')
        self.conflict_line = ttk.Frame(left)
        self.conflict_line.pack(fill='x', pady=(self.px(8), 0))
        self.conflict_label = tk.Label(self.conflict_line, text='', bg=PANEL, fg=WARN, font=self.font(10),
                                       justify='left', anchor='w')
        self.conflict_label.pack(side='left')
        self.conflict_toggle = ttk.Button(self.conflict_line, text='Details', command=self.toggle_conflicts)
        self.conflict_files = ttk.Label(left, text='', style='Muted.TLabel', justify='left')
        self.conflict_files.pack(anchor='w')
        self.mod_details = tk.Frame(self.mods_body, bg=CARD, padx=self.px(16), pady=self.px(16))

        page.next_row = 6
        self.section(page, 'Presets')
        presets = ttk.Frame(page)
        presets.grid(row=page.next_row, column=0, sticky='w')
        page.next_row += 1
        self.preset_box = ttk.Combobox(presets, values=sorted(load_json(MOD_PRESETS, {})), width=26)
        self.preset_box.pack(side='left')
        for text, command in (('Load', self.load_preset), ('Save', self.save_preset), ('Delete', self.delete_preset)):
            ttk.Button(presets, text=text, command=command).pack(side='left', padx=(self.px(6), 0))
        ttk.Label(page, text='A preset keeps which mods are on and their order.', style='Muted.TLabel').grid(
            row=page.next_row, column=0, sticky='w', pady=(self.px(4), 0))
        page.next_row += 1

        self.section(page, 'Third-party patches')
        ttk.Label(page, text='shadPS4 and GoldHEN patch files (.xml) for GODZILLA 01.01, in the patches folder. They '
                             'change the game\'s code when it starts.', style='Muted.TLabel', wraplength=self.px(760),
                  justify='left').grid(row=page.next_row, column=0, sticky='w')
        page.next_row += 1
        self.patches_list = ttk.Frame(page)
        self.patches_list.grid(row=page.next_row, column=0, sticky='we', pady=(self.px(8), 0))
        page.next_row += 1
        ttk.Button(page, text='Open patches folder', command=lambda: self.open_folder(PATCHES)).grid(
            row=page.next_row, column=0, sticky='w', pady=(self.px(10), 0))
        page.next_row += 1

        self.section(page, 'Folders')
        for name, path in (('Mods', MODS), ('Patches', PATCHES)):
            self.ttk.Label(page, text=f'{name}:  {path}', style='Muted.TLabel', wraplength=self.px(760)).grid(
                row=page.next_row, column=0, sticky='w')
            page.next_row += 1
        self.mod_order, self.mod_vars, self.mod_rows = None, {}, {}
        self.patch_vars = {}
        self.selected_mod, self.conflicts_open, self.mod_conflicts = None, False, []
        self.mod_photo_key, self.mod_photo = None, None

    def open_folder(self, path):
        Path(path).mkdir(parents=True, exist_ok=True)
        os.startfile(path) if sys.platform == 'win32' else subprocess.Popen(['xdg-open', str(path)])

    def refresh_mods(self):
        """The load order (mods.json's, new mods last), the patches of this game version and their state."""
        tk, ttk = self.tk, self.ttk
        import mods
        for holder in (self.mods_list, self.patches_list):
            for widget in holder.winfo_children():
                widget.destroy()
        game = self.values['game_dir'] or None
        try:
            available = mods.discover(MODS, game)
        except (OSError, ValueError):
            available = []
        config = load_json(MODS_CONFIG, {})
        if self.mod_order is None or set(self.mod_order) != set(available):
            known = [n for n in config.get('order', []) if n in available]
            self.mod_order = known + [n for n in available if n not in known]
        if self.selected_mod not in available:
            self.selected_mod = None
        old = {n: v.get() for n, v in self.mod_vars.items()}
        self.mod_vars, self.mod_rows = {}, {}
        if not available:
            box = tk.Frame(self.mods_list, bg=CARD, pady=self.px(26))
            box.pack(fill='x')
            tk.Label(box, text='No mods yet', bg=CARD, fg=TEXT, font=(self.title_face, 16)).pack()
            tk.Label(box, text='Put each mod in its own folder, or as a .zip, in the mods folder. A mod holds the '
                               'game\'s folders (sound, ...) with the files it replaces.', bg=CARD, fg=MUTED,
                     font=self.font(10), wraplength=self.px(560)).pack(pady=(self.px(4), self.px(12)))
            ttk.Button(box, text='Open mods folder', command=lambda: self.open_folder(MODS)).pack()
        for index, name in enumerate(self.mod_order):
            if index:
                tk.Frame(self.mods_list, bg=LINE, height=self.px(1)).pack(fill='x')
            chosen = name == self.selected_mod
            bg = '#18222e' if chosen else CARD
            line = tk.Frame(self.mods_list, bg=bg, padx=self.px(12), pady=self.px(6))
            line.pack(fill='x')
            var = tk.BooleanVar(value=old.get(name, name not in config.get('disabled', [])))
            self.mod_vars[name] = var
            check = tk.Checkbutton(line, variable=var, image=self.check_off, selectimage=self.check_on, indicatoron=False,
                                   bd=0, bg=bg, activebackground=bg, selectcolor=bg, highlightthickness=0,
                                   relief='flat', offrelief='flat', command=self.refresh_mod_marks, cursor='hand2')
            check.pack(side='left')
            tk.Label(line, text=str(index + 1), bg=bg, fg=MUTED, font=self.font(9), width=3, anchor='e').pack(
                side='left', padx=(self.px(2), self.px(10)))
            try:
                title = mods.mod_info(MODS / name, game)['name']
            except OSError:
                title = name
            label = tk.Label(line, text=title, bg=bg, anchor='w', cursor='hand2',
                             font=self.font(11, 'bold', 'Segoe UI Semibold') if chosen else self.font(11))
            label.pack(side='left', fill='x', expand=True)
            for widget in (line, label):
                widget.bind('<Button-1>', lambda _e, n=name: self.select_mod(n))
            ttk.Button(line, text='▼', width=2, command=lambda i=index: self.move_mod(i, 1)).pack(side='right')
            ttk.Button(line, text='▲', width=2, command=lambda i=index: self.move_mod(i, -1)).pack(
                side='right', padx=(0, self.px(4)))
            mark = tk.Label(line, text='', bg=bg, fg=WARN, font=self.font(11))
            mark.pack(side='right', padx=(0, self.px(12)))
            self.mod_rows[name] = (label, mark, chosen)
        self.refresh_mod_marks()
        self.preset_box.configure(values=sorted(load_json(MOD_PRESETS, {})))
        self.refresh_patches()

    def refresh_patches(self):
        """The third-party patches for this game version: one check box each, on as patches.json (or
        the file's isEnabled) says."""
        ttk = self.ttk
        import patches
        chosen = load_json(PATCHES_CONFIG, {})
        profile = patches.game_profile_for(self.values['game_dir']) if self.values['game_dir'] else None
        try:
            found = patches.external_patches(PATCHES, profile[2], profile[1], profile[0]) if profile else []
        except (OSError, ValueError):
            found = []
        old = {k: v.get() for k, v in self.patch_vars.items()}
        self.patch_vars = {}
        if not found:
            ttk.Label(self.patches_list, text='No patch files for this game version yet.', style='Muted.TLabel').pack(
                anchor='w')
        for key, _path, meta in found:
            on = key in chosen.get('enabled', []) or (key not in chosen.get('disabled', []) and
                                                      meta.get('isEnabled', 'false').lower() == 'true')
            var = self.tk.BooleanVar(value=old.get(key, on))
            self.patch_vars[key] = var
            author = meta.get('Author')
            text = meta.get('Name', key) + (f'  ·  {author}' if author else '') + f'   ({key.split("/")[0]})'
            ttk.Checkbutton(self.patches_list, text=text, variable=var).pack(anchor='w')
            note = (meta.get('Note') or '').strip()
            if note:
                ttk.Label(self.patches_list, text=note, style='Muted.TLabel', wraplength=self.px(700),
                          justify='left').pack(anchor='w', padx=(self.px(30), 0), pady=(0, self.px(4)))

    def refresh_mod_marks(self):
        """The count of active mods, the files two of them change (the lower one wins) and the details."""
        import mods
        active = [n for n in self.mod_order or [] if self.mod_vars[n].get()]
        try:
            found = mods.conflicts(MODS, active, self.values['game_dir'] or None)
        except (OSError, ValueError):
            found = []
        self.mod_conflicts = found
        loading = self.mods_enabled_var.get()
        total = len(self.mod_order or [])
        self.mods_count.configure(text=(f'{len(active)} of {total} active' if loading else 'Mods are off')
                                  if total else '')
        involved = {name for _path, owners in found for name in owners}
        for name, (label, mark, chosen) in self.mod_rows.items():
            on = self.mod_vars[name].get() and loading
            label.configure(fg=ACCENT_HI if chosen else TEXT if on else MUTED)
            mark.configure(text='⚠' if name in involved and loading else '')
        if not found or not loading:
            summary = ''
        elif len(found) == 1:
            summary = f'⚠ 1 file is changed by two mods: {found[0][1][-1]} wins.'
        else:
            summary = f'⚠ {len(found)} files are changed by more than one mod: the lower one in the list wins.'
        self.conflict_label.configure(text=summary)
        if summary:
            self.conflict_toggle.configure(text='Hide' if self.conflicts_open else 'Details')
            self.conflict_toggle.pack(side='left', padx=(self.px(10), 0))
        else:
            self.conflict_toggle.pack_forget()
        lines = [f'{path}:  {" > ".join(owners)}' for path, owners in found[:12]]
        if len(found) > 12:
            lines.append(f'… and {len(found) - 12} more')
        self.conflict_files.configure(text='\n'.join(lines) if self.conflicts_open and summary else '')
        self.show_mod_details()

    def toggle_conflicts(self):
        self.conflicts_open = not self.conflicts_open
        self.refresh_mod_marks()

    def select_mod(self, name):
        """A click on a mod shows its details; a second click hides them."""
        self.selected_mod = None if name == self.selected_mod else name
        self.refresh_mods()

    def move_mod(self, index, step):
        other = index + step
        if 0 <= other < len(self.mod_order):
            self.mod_order[index], self.mod_order[other] = self.mod_order[other], self.mod_order[index]
            self.refresh_mods()

    def show_mod_details(self):
        """The selected mod's picture, name, author, version, description, file count and conflicts."""
        tk, ttk, panel = self.tk, self.ttk, self.mod_details
        import mods
        for widget in panel.winfo_children():
            widget.destroy()
        panel.grid_forget()
        name = self.selected_mod
        if not name:
            return
        game = self.values['game_dir'] or None
        try:
            info = mods.mod_info(MODS / name, game)
            count = sum(1 for _file in mods.mod_files(MODS / name, game))
            has_patches = bool(mods.patch_files(MODS / name))
        except (OSError, ValueError):
            return
        width = self.px(280)
        picture = self.mod_picture(info['image'], width) if info['image'] else None
        if picture:
            tk.Label(panel, image=picture, bg=CARD, bd=0).pack(anchor='w', pady=(0, self.px(12)))
        tk.Label(panel, text=info['name'], bg=CARD, fg=TEXT, font=(self.title_face, 16), wraplength=width,
                 justify='left').pack(anchor='w')
        byline = '  ·  '.join(filter(None, (info['author'] and f'by {info["author"]}',
                                            info['version'] and f'v{info["version"]}')))
        if byline:
            tk.Label(panel, text=byline, bg=CARD, fg=MUTED, font=self.font(9)).pack(anchor='w', pady=(self.px(2), 0))
        if info['description']:
            tk.Label(panel, text=info['description'], bg=CARD, fg=TEXT, font=self.font(10), wraplength=width,
                     justify='left').pack(anchor='w', pady=(self.px(10), 0))
        facts = ('1 file' if count == 1 else f'{count} files') + ('  ·  game patches' if has_patches else '')
        tk.Label(panel, text=facts, bg=CARD, fg=MUTED, font=self.font(9)).pack(anchor='w', pady=(self.px(10), 0))
        for path, owners in [c for c in self.mod_conflicts if name in c[1]][:4]:
            others = ', '.join(o for o in owners if o != name)
            tk.Label(panel, text=f'⚠ {Path(path).name} is also in {others}: {owners[-1]} wins.', bg=CARD, fg=WARN,
                     font=self.font(9), wraplength=width, justify='left').pack(anchor='w', pady=(self.px(4), 0))
        buttons = tk.Frame(panel, bg=CARD)
        buttons.pack(anchor='w', pady=(self.px(14), 0))
        ttk.Button(buttons, text='Open mod folder', command=lambda: self.open_folder(MODS / name)).pack(side='left')
        if info['url']:
            import webbrowser
            ttk.Button(buttons, text='Website', command=lambda: webbrowser.open(info['url'])).pack(
                side='left', padx=(self.px(6), 0))
        panel.grid(row=0, column=1, sticky='n', padx=(self.px(16), 0))

    def mod_picture(self, path, width):
        """A mod's preview scaled to `width` (at most 4:3 tall), or None."""
        key = (str(path), width)
        if self.mod_photo_key != key:
            self.mod_photo_key, self.mod_photo = key, None
            try:
                from PIL import Image, ImageTk
                with Image.open(path) as image:
                    image = image.convert('RGB')
                    image.thumbnail((width, width * 3 // 4), Image.LANCZOS)
                    self.mod_photo = ImageTk.PhotoImage(image)
            except (ImportError, OSError, ValueError):
                self.mod_photo = None
        return self.mod_photo

    def save_preset(self):
        name = self.preset_box.get().strip()
        if not name:
            self.summary.configure(text='Type a name for the preset first.', fg=MUTED)
            return
        presets = load_json(MOD_PRESETS, {})
        presets[name] = {'order': list(self.mod_order or []),
                         'disabled': [n for n, v in self.mod_vars.items() if not v.get()]}
        write_json(MOD_PRESETS, presets)
        self.preset_box.configure(values=sorted(presets))
        self.summary.configure(text=f'Preset "{name}" saved.', fg=ACCENT_HI)

    def load_preset(self):
        import mods
        name = self.preset_box.get().strip()
        preset = load_json(MOD_PRESETS, {}).get(name)
        if not isinstance(preset, dict):
            return
        self.mod_order, disabled = mods.apply_preset(preset, list(self.mod_order or []))
        for mod, var in self.mod_vars.items():
            var.set(mod not in disabled)
        self.refresh_mods()
        self.preset_box.set(name)

    def delete_preset(self):
        presets = load_json(MOD_PRESETS, {})
        if presets.pop(self.preset_box.get().strip(), None) is not None:
            write_json(MOD_PRESETS, presets)
        self.preset_box.configure(values=sorted(presets))
        self.preset_box.set('')

    def save_mods(self):
        """mods.json (the order and the mods that are off) and patches.json (the third-party patches,
        keeping the choices for files that are not there now)."""
        if self.mod_order:
            write_json(MODS_CONFIG, {'order': list(self.mod_order),
                                     'disabled': [n for n, v in self.mod_vars.items() if not v.get()]})
        if self.patch_vars:
            chosen = load_json(PATCHES_CONFIG, {})
            enabled = set(chosen.get('enabled', [])) - set(self.patch_vars)
            disabled = set(chosen.get('disabled', [])) - set(self.patch_vars)
            enabled |= {k for k, v in self.patch_vars.items() if v.get()}
            disabled |= {k for k, v in self.patch_vars.items() if not v.get()}
            write_json(PATCHES_CONFIG, {'enabled': sorted(enabled), 'disabled': sorted(disabled)})

    # ---- the Achievements page ----------------------------------------------------------------
    def build_achievements(self):
        tk, ttk = self.tk, self.ttk
        outer = ttk.Frame(self.content)
        self.pages['achievements'] = outer
        head = ttk.Frame(outer, padding=(self.px(32), self.px(26), self.px(32), self.px(4)))
        head.pack(fill='x')
        head.columnconfigure(1, weight=1)
        ttk.Label(head, text='ACHIEVEMENTS', style='Heading.TLabel').grid(row=0, column=0, columnspan=3, sticky='w')
        ttk.Label(head, text='GODZILLA\'s 45 trophies and the ones you unlocked with this port.',
                  style='Muted.TLabel').grid(row=1, column=0, columnspan=3, sticky='w')
        self.trophy_count = tk.Label(head, text='', bg=PANEL, fg=TEXT, font=(self.title_face, 30))
        self.trophy_count.grid(row=2, column=0, rowspan=2, sticky='w', pady=(self.px(14), 0), padx=(0, self.px(22)))
        self.trophy_bar = ttk.Progressbar(head, style='Trophy.Horizontal.TProgressbar', maximum=45)
        self.trophy_bar.grid(row=2, column=1, sticky='we', pady=(self.px(22), 0))
        grades = tk.Frame(head, bg=PANEL)
        grades.grid(row=3, column=1, sticky='w', pady=(self.px(6), 0))
        self.grade_labels = {}
        for grade in ('Platinum', 'Gold', 'Silver', 'Bronze'):
            tk.Label(grades, text='●', bg=PANEL, fg=GRADE_COLOURS[grade], font=self.font(10)).pack(side='left')
            label = tk.Label(grades, text='', bg=PANEL, fg=TEXT, font=self.font(10))
            label.pack(side='left', padx=(self.px(4), self.px(18)))
            self.grade_labels[grade] = label
        filters = ttk.Frame(head)
        filters.grid(row=2, column=2, rowspan=2, sticky='e', padx=(self.px(22), 0), pady=(self.px(14), 0))
        self.trophy_filter = self.tk.StringVar(value='All trophies')
        box = ttk.Combobox(filters, textvariable=self.trophy_filter, values=('All trophies', 'Unlocked', 'Locked'),
                           state='readonly', width=14)
        box.pack(side='left')
        box.bind('<<ComboboxSelected>>', lambda _e: self.refresh_achievements())
        ttk.Button(filters, text='Refresh', command=self.refresh_achievements).pack(side='left', padx=(self.px(8), 0))

        holder = ttk.Frame(outer, padding=(self.px(32), self.px(12), self.px(16), self.px(12)))
        holder.pack(fill='both', expand=True)
        self.trophy_canvas = tk.Canvas(holder, bg=PANEL, highlightthickness=0, bd=0)
        bar = ttk.Scrollbar(holder, orient='vertical', command=self.trophy_canvas.yview)
        self.trophy_list = tk.Frame(self.trophy_canvas, bg=PANEL)
        window = self.trophy_canvas.create_window(0, 0, window=self.trophy_list, anchor='nw')
        self.trophy_list.bind('<Configure>', lambda _e: self.trophy_canvas.configure(
            scrollregion=self.trophy_canvas.bbox('all')))
        self.trophy_canvas.bind('<Configure>', lambda e: self.trophy_canvas.itemconfigure(window, width=e.width))
        self.trophy_canvas.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y', padx=(self.px(8), 0))
        self.trophy_canvas.pack(side='left', fill='both', expand=True)
        self.trophy_icons = {}    # (game folder, id) -> PNG bytes of the dump's trophy00.trp
        self.trophy_photos = {}   # (id, locked) -> the icon as shown

    def wheel(self, event):
        """The mouse wheel scrolls the trophy list and the mods page (not an open drop-down list)."""
        canvas = {'achievements': self.trophy_canvas, 'mods': self.mods_canvas}.get(self.current)
        if canvas and 'popdown' not in str(event.widget):
            canvas.yview_scroll(int(-event.delta / 120), 'units')

    def trophy_photo(self, trophy, png, locked):
        """The icon at 64 px (scaled); grey and dark while the trophy is locked."""
        key = (trophy, locked)
        if key not in self.trophy_photos:
            photo = None
            try:
                import io
                from PIL import Image, ImageTk
                size = self.px(64)
                image = Image.open(io.BytesIO(png)).convert('RGB').resize((size, size), Image.LANCZOS)
                if locked:
                    image = image.convert('L').point(lambda v: int(v * 0.45)).convert('RGB')
                photo = ImageTk.PhotoImage(image)
            except (ImportError, OSError, ValueError):
                photo = None
            self.trophy_photos[key] = photo
        return self.trophy_photos[key]

    def refresh_achievements(self):
        tk = self.tk
        import godzilla_trophies
        for child in self.trophy_list.winfo_children():
            child.destroy()
        folder = self.values['game_dir']
        if folder not in self.trophy_icons:
            self.trophy_icons = {folder: godzilla_trophies.read_trp_icons(
                Path(folder) / 'sce_sys' / 'trophy' / 'trophy00.trp') if folder else {}}
            self.trophy_photos = {}
        icons = self.trophy_icons[folder]
        unlocked = godzilla_trophies.read_unlocked(DATA / 'user' / 'trophies.dat')
        trophies = godzilla_trophies.TROPHIES
        got = [i for i in range(len(trophies)) if i in unlocked]
        self.trophy_count.configure(text=f'{len(got)} / {len(trophies)}')
        self.trophy_bar.configure(value=len(got), maximum=len(trophies))
        for grade, label in self.grade_labels.items():
            ids = [i for i, t in enumerate(trophies) if t[2] == grade]
            label.configure(text=f'{grade} {sum(i in unlocked for i in ids)} / {len(ids)}')
        if not icons:
            tk.Label(self.trophy_list, text='Choose the game folder on the Play page: the icons come from its '
                                            'sce_sys/trophy/trophy00.trp.', bg=PANEL, fg=MUTED, anchor='w',
                     font=self.font(10)).pack(fill='x', pady=(0, self.px(10)))
        show = self.trophy_filter.get()
        for trophy, (name, description, grade) in enumerate(trophies):
            done = trophy in unlocked
            if (show == 'Unlocked' and not done) or (show == 'Locked' and done):
                continue
            card = tk.Frame(self.trophy_list, bg=CARD)
            card.pack(fill='x', pady=(0, self.px(6)))
            tk.Frame(card, bg=GRADE_COLOURS[grade] if done else LINE, width=self.px(4)).pack(side='left', fill='y')
            photo = self.trophy_photo(trophy, icons[trophy], not done) if trophy in icons else None
            if photo:
                icon = tk.Label(card, image=photo, bg=CARD)
            else:
                icon = tk.Frame(card, bg=LINE, width=self.px(64), height=self.px(64))
            icon.pack(side='left', padx=(self.px(12), self.px(16)), pady=self.px(10))
            tag = tk.Label(card, text='UNLOCKED' if done else 'LOCKED', bg=CARD, fg=ACCENT_HI if done else MUTED,
                           font=self.font(9, 'bold'))
            tag.pack(side='right', padx=self.px(18))
            text = tk.Frame(card, bg=CARD)
            text.pack(side='left', fill='x', expand=True, pady=self.px(8))
            tk.Label(text, text=name, bg=CARD, fg=TEXT if done else '#b4c0ce', anchor='w',
                     font=self.font(12, 'bold', 'Segoe UI Semibold')).pack(fill='x')
            tk.Label(text, text=description, bg=CARD, fg=TEXT if done else MUTED, anchor='w', justify='left',
                     wraplength=self.px(600), font=self.font(10)).pack(fill='x')
            tk.Label(text, text=grade.upper(), bg=CARD, fg=GRADE_COLOURS[grade] if done else MUTED, anchor='w',
                     font=self.font(8, 'bold')).pack(fill='x', pady=(self.px(2), 0))
        self.trophy_canvas.yview_moveto(0)

    def build_advanced(self):
        ttk = self.ttk
        page = self.page('advanced', 'Advanced', 'Folders, the game log and the shader cache.')
        self.section(page, 'Files')
        for text, command in (('Open the data folder (saves, settings, logs)', self.open_data),
                              ('Show the last game log', self.open_log),
                              ('Clear the shader cache (it is rebuilt while you play)', self.clear_cache)):
            ttk.Button(page, text=text, command=command).grid(row=page.next_row, column=0, columnspan=2, sticky='w',
                                                              pady=self.px(4))
            page.next_row += 1
        self.section(page, 'Folders')
        for name, path in (('Port', PORT), ('Data', DATA)):
            self.row(page, name, ttk.Label(page, text=str(path), style='Muted.TLabel'))

    # ---- settings -----------------------------------------------------------------------------
    def update_summary(self):
        resolution = dict((label, name) for label, name in self.resolution_pairs).get(self.resolution_var.get(), 'native')
        mode = self.mode_var.get()
        parts = [mode, 'native 1080p' if resolution == 'native' else resolution,
                 'VSync' if self.vsync_var.get() else 'VSync off']
        display = display_refresh()
        if display and mode != 'Windowed' and self.match_refresh_var.get() and display[2] % 60 and display[2] >= 120:
            parts.append('120 Hz')
        limit = dict(FPS_LIMITS).get(self.fps_var.get(), '60')
        parts.append('30 FPS' if limit == '30' else '60 FPS')
        self.summary.configure(text='   ·   '.join(parts), fg=MUTED)
        if hasattr(self, 'checks'):
            self.refresh_checks()

    def default_keys(self):
        for action, _, default in KEYS:
            self.key_vars[action].set(default)

    def collect(self):
        self.values['language'] = dict(LANGUAGES)[self.language_var.get()]
        self.values['unlock_addons'] = bool(self.addons_var.get())
        self.values['mods_enabled'] = bool(self.mods_enabled_var.get())
        defaults = {a: d for a, _, d in KEYS}
        self.values['keys'] = {a: v.get().strip() for a, v in self.key_vars.items() if v.get().strip() != defaults[a]}

    def save(self, announce=True):
        self.collect()
        save_settings(self.values)
        self.save_mods()
        resolution = dict((label, name) for label, name in self.resolution_pairs).get(self.resolution_var.get(), 'native')
        write_ini({'display_mode': dict(DISPLAY_MODES)[self.mode_var.get()], 'vsync': int(self.vsync_var.get()),
                   'match_refresh': int(self.match_refresh_var.get()), 'render_resolution': resolution,
                   'anisotropy': dict(ANISOTROPY)[self.anisotropy_var.get()],
                   'fps_limit': dict(FPS_LIMITS)[self.fps_var.get()], 'show_fps': int(self.show_fps_var.get()),
                   'mouse_aim_sensitivity': self.sensitivity_var.get(), 'vibration': self.vibration_var.get()})
        if announce:
            self.summary.configure(text='Settings saved.', fg=ACCENT_HI)
            self.root.after(2500, self.update_summary)

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
        self.version_label.configure(text='' if problem else f'GODZILLA  ·  {game_version(folder)}')
        self.play_button.state(['disabled'] if problem else ['!disabled'])
        self.load_art(None if problem else folder)
        self.draw_banner()
        self.refresh_checks()

    # ---- the game -----------------------------------------------------------------------------
    def play(self):
        if self.process and self.process.poll() is None:
            return
        self.save(announce=False)
        LOG.parent.mkdir(parents=True, exist_ok=True)
        log = open(LOG, 'wb')
        try:
            self.process = start_game(self.values, log)
        except OSError as error:
            log.close()
            self.summary.configure(text=f'Could not start: {error}', fg=RED)
            return
        self.play_button.state(['disabled'])
        self.summary.configure(text='GODZILLA is running…  (the first start prepares the game image)', fg=ACCENT_HI)
        threading.Thread(target=self.wait, args=(self.process, log), daemon=True).start()

    def wait(self, process, log):
        code = process.wait()
        log.close()
        self.root.after(0, self.finished, code)

    def finished(self, code):
        self.play_button.state(['!disabled'])
        if code in (0, 1, None):
            self.update_summary()
        else:
            self.summary.configure(text=f'The game stopped (code {code}): Advanced → Show the last game log.', fg=RED)

    def open_data(self):
        DATA.mkdir(parents=True, exist_ok=True)
        os.startfile(DATA) if sys.platform == 'win32' else subprocess.Popen(['xdg-open', str(DATA)])

    def open_log(self):
        if LOG.is_file():
            os.startfile(LOG) if sys.platform == 'win32' else subprocess.Popen(['xdg-open', str(LOG)])
        else:
            self.summary.configure(text='No game log yet.', fg=MUTED)

    def clear_cache(self):
        import shutil
        removed = 0
        for cache in (DATA / 'user' / 'cache', DATA / 'user' / 'shader_cache'):
            if cache.is_dir():
                shutil.rmtree(cache, ignore_errors=True)
                removed += 1
        self.summary.configure(text='Shader cache cleared.' if removed else 'There was no shader cache.', fg=MUTED)

    def close(self):
        if self.process and self.process.poll() is None:
            from tkinter import messagebox
            if not messagebox.askyesno('Minus Two', 'The game is still running. Close the launcher anyway?'):
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
