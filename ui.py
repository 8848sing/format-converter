# -*- coding: utf-8 -*-
"""
ui.py — 现代化深色工作台式图形界面。

保留原有转换注册表与线程模型，重做界面层级、拖放交互、文件队列和状态反馈。
"""
from __future__ import annotations

import os
import queue
import re
import threading
import time
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import ttkbootstrap as tb
from tkinterdnd2 import DND_FILES, TkinterDnD

from converters import (
    CATEGORY_EXTS,
    ConvertError,
    categories_src,
    convs_for,
    dsts_for,
    engine_info,
    pair_exists,
)
import settings_store
from version import version_text

# ---------------------------------------------------------------------------
# 主题
# ---------------------------------------------------------------------------
UI_FONT = "Microsoft YaHei UI"

BG = "#0B101A"
SURFACE = "#141A26"
SURFACE_ALT = "#1B2333"
SURFACE_HOVER = "#243047"
BORDER = "#28344A"
TEXT = "#F3F6FC"
TEXT_SOFT = "#C7D0E0"
MUTED = "#8491A8"
PRIMARY = "#6D8DFF"
PRIMARY_HOVER = "#82A0FF"
PRIMARY_DARK = "#304A9A"
SUCCESS = "#3DD9A4"
WARNING = "#FFB454"
ERROR = "#FF6B7A"
DROP_BLUE = "#253B5A"
DROP_BLUE_EDGE = "#4775C7"
OFFICE_BG = "#3A2D2D"
OFFICE_ACCENT = "#E18A72"
PDF_BG = "#243B3A"
PDF_ACCENT = "#55C8B5"



EXT_KIND = {}
for _cat, _exts in CATEGORY_EXTS.items():
    for _ext in _exts:
        EXT_KIND[_ext] = _cat


FONT = (UI_FONT, 11)
FONT_SMALL = (UI_FONT, 10)
FONT_TINY = (UI_FONT, 9)
FONT_SECTION = (UI_FONT, 12, 'bold')
FONT_TITLE = (UI_FONT, 19, 'bold')
FONT_HERO = (UI_FONT, 16, 'bold')

# 高分屏适配：字号以 pt 跟随系统缩放，这里把固定像素尺寸一并换算，
# 避免 125% / 150% 缩放下控件高度不够导致文字被裁切。
UI_SCALE = 1.0


def px(value):
    return int(round(value * UI_SCALE))


PICK_HINT = "请选择…"
MEDIA_BG = '#302844'
MEDIA_ACCENT = '#B18CFF'
DATA_BG = '#213949'
DATA_ACCENT = '#55C7E8'
TEXT_TILE_BG = '#2B333F'
TEXT_TILE_ACCENT = '#A9B8CE'


def _round_rect(canvas: tk.Canvas, x1, y1, x2, y2, radius, **kwargs):
    """在 Canvas 上绘制圆角矩形。"""
    r = min(radius, max(1, (x2 - x1) / 2), max(1, (y2 - y1) / 2))
    points = [
        x1 + r, y1, x1 + r, y1, x2 - r, y1, x2 - r, y1,
        x2, y1, x2, y1 + r, x2, y1 + r, x2, y2 - r,
        x2, y2 - r, x2, y2, x2 - r, y2, x2 - r, y2,
        x1 + r, y2, x1 + r, y2, x1, y2, x1, y2 - r,
        x1, y2 - r, x1, y1 + r, x1, y1 + r, x1, y1,
    ]
    return canvas.create_polygon(points, smooth=True, splinesteps=24, **kwargs)


def _short_size(n: int) -> str:
    units = ("B", "KB", "MB", "GB")
    value = float(n)
    for unit in units:
        if value < 1024 or unit == units[-1]:
            return f"{value:.0f} {unit}" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024
    return f"{n} B"


class RoundedCard(tk.Canvas):
    """带圆角、细边框和内部 Frame 的卡片容器。"""

    def __init__(
        self,
        master,
        *,
        fill=SURFACE,
        outline=BORDER,
        outer_bg=BG,
        radius=18,
        padding=15,
        height=120,
        **kwargs,
    ):
        super().__init__(
            master,
            height=px(height),
            bg=outer_bg,
            bd=0,
            highlightthickness=0,
            **kwargs,
        )
        self.fill = fill
        self.outline = outline
        self.radius = px(radius)
        self.padding = px(padding)
        self.inner = tk.Frame(self, bg=fill)
        self._window = self.create_window(
            px(padding), px(padding), anchor="nw", window=self.inner
        )
        self.bind("<Configure>", self._redraw)

    def _redraw(self, _event=None):
        w = max(2, self.winfo_width())
        h = max(2, self.winfo_height())
        self.delete("card_bg")
        _round_rect(
            self,
            1,
            1,
            w - 1,
            h - 1,
            self.radius,
            fill=self.fill,
            outline=self.outline,
            width=1,
            tags="card_bg",
        )
        self.tag_lower("card_bg")
        self.itemconfigure(
            self._window,
            width=max(1, w - self.padding * 2),
            height=max(1, h - self.padding * 2),
        )


class IconCircle(tk.Canvas):
    """圆形图标。"""

    def __init__(
        self,
        master,
        *,
        size=54,
        fill=PRIMARY_DARK,
        text="⇄",
        fg=TEXT,
        outer_bg=SURFACE,
        font=(UI_FONT, 19, "bold"),
        outline="",
    ):
        size = px(size)
        super().__init__(
            master,
            width=size,
            height=size,
            bg=outer_bg,
            bd=0,
            highlightthickness=0,
        )
        self.create_oval(
            1, 1, size - 1, size - 1, fill=fill, outline=outline or fill
        )
        self.create_text(size / 2, size / 2, text=text, fill=fg, font=font)


class PillButton(tk.Canvas):
    """可悬停、可启停的圆角按钮。"""

    def __init__(
        self,
        master,
        *,
        text,
        command,
        width=120,
        height=38,
        fill=PRIMARY,
        hover=PRIMARY_HOVER,
        fg="#FFFFFF",
        disabled_fill="#2B3446",
        disabled_fg="#738097",
        outer_bg=SURFACE,
        font=(UI_FONT, 9, "bold"),
        radius=11,
        outline="",
    ):
        super().__init__(
            master,
            width=px(width),
            height=px(height),
            bg=outer_bg,
            bd=0,
            highlightthickness=0,
            cursor="hand2",
        )
        self._text = text
        self._command = command
        self._fill = fill
        self._hover = hover
        self._fg = fg
        self._disabled_fill = disabled_fill
        self._disabled_fg = disabled_fg
        self._font = font
        self._radius = px(radius)
        self._outline = outline
        self._enabled = True
        self._hovered = False
        self.bind("<Configure>", lambda _e: self._draw())
        self.bind("<Enter>", self._on_enter)
        self.bind("<Leave>", self._on_leave)
        self.bind("<Button-1>", self._on_click)

    def _draw(self):
        self.delete("all")
        w = max(2, self.winfo_width())
        h = max(2, self.winfo_height())
        fill = self._fill if self._enabled else self._disabled_fill
        if self._enabled and self._hovered:
            fill = self._hover
        _round_rect(
            self,
            1,
            1,
            w - 1,
            h - 1,
            self._radius,
            fill=fill,
            outline=self._outline or fill,
            width=1,
        )
        self.create_text(
            w / 2,
            h / 2 + 1,
            text=self._text,
            fill=self._fg if self._enabled else self._disabled_fg,
            font=self._font,
        )

    def _on_enter(self, _event=None):
        self._hovered = True
        if self._enabled:
            self.configure(cursor="hand2")
        self._draw()

    def _on_leave(self, _event=None):
        self._hovered = False
        self._draw()

    def _on_click(self, _event=None):
        if self._enabled and self._command:
            self._command()

    def set_enabled(self, enabled: bool):
        self._enabled = bool(enabled)
        self.configure(cursor="hand2" if self._enabled else "arrow")
        self._draw()

    def set_text(self, text: str):
        self._text = text
        self._draw()


class QuickTile(tk.Canvas):
    """参考图的彩色快捷卡片。"""

    def __init__(
        self,
        master,
        *,
        icon,
        title,
        subtitle,
        fill,
        accent,
        command,
        outer_bg=BG,
    ):
        super().__init__(
            master,
            width=px(116),
            height=px(104),
            bg=outer_bg,
            bd=0,
            highlightthickness=0,
            cursor="hand2",
        )
        self.icon = icon
        self.title = title
        self.subtitle = subtitle
        self.fill = fill
        self.accent = accent
        self.command = command
        self.active = False
        self.hovered = False
        self.bind("<Configure>", lambda _e: self._draw())
        self.bind("<Enter>", lambda _e: self._set_hover(True))
        self.bind("<Leave>", lambda _e: self._set_hover(False))
        self.bind("<Button-1>", lambda _e: self.command())

    def _set_hover(self, value: bool):
        self.hovered = value
        self._draw()

    def set_active(self, value: bool):
        self.active = bool(value)
        self._draw()

    def _draw(self):
        self.delete("all")
        w = max(2, self.winfo_width())
        h = max(2, self.winfo_height())
        outline = self.accent if (self.active or self.hovered) else self.fill
        _round_rect(
            self,
            1,
            1,
            w - 1,
            h - 1,
            16,
            fill=self.fill,
            outline=outline,
            width=2 if self.active else 1,
        )
        self.create_oval(
            w / 2 - 20,
            14,
            w / 2 + 20,
            54,
            fill=self.accent,
            outline=self.accent,
        )
        self.create_text(
            w / 2,
            34,
            text=self.icon,
            fill="#FFFFFF",
            font=(UI_FONT, 16, "bold"),
        )
        self.create_text(
            w / 2,
            73,
            text=self.title,
            fill=TEXT,
            font=(UI_FONT, 10, "bold"),
        )
        self.create_text(
            w / 2,
            91,
            text=self.subtitle,
            fill=TEXT_SOFT,
            font=FONT_TINY,
        )

class CompactQuickTile(QuickTile):
    def __init__(self, master, **kwargs):
        super().__init__(master, **kwargs)
        self.configure(height=px(54))

    def _draw(self):
        self.delete('all')
        w = max(2, self.winfo_width())
        h = max(2, self.winfo_height())
        outline = self.accent if (self.active or self.hovered) else self.fill
        _round_rect(self, 1, 1, w - 1, h - 1, px(14), fill=self.fill, outline=outline, width=2 if self.active else 1)
        cy = h / 2
        r = px(14)
        self.create_oval(px(12), cy - r, px(12) + r * 2, cy + r, fill=self.accent, outline=self.accent)
        self.create_text(px(12) + r, cy, text=self.icon, fill='#FFFFFF', font=(UI_FONT, 10, 'bold'))
        self.create_text(w / 2 + px(15), cy - px(9), text=self.title, fill=TEXT, font=(UI_FONT, 9, 'bold'))
        self.create_text(w / 2 + px(15), cy + px(10), text=self.subtitle, fill=TEXT_SOFT, font=(UI_FONT, 8))


class Tooltip:
    """轻量悬浮提示：在控件下方显示一句功能说明。"""

    def __init__(self, widget, text: str, delay: int = 350, wraplength: int = 320):
        self.widget = widget
        self.text = text
        self.delay = delay
        self.wraplength = wraplength
        self._after_id = None
        self._tip = None
        widget.bind("<Enter>", self._schedule, add="+")
        widget.bind("<Leave>", self._hide, add="+")
        widget.bind("<Button-1>", self._hide, add="+")
        widget.bind("<Destroy>", self._hide, add="+")

    def _schedule(self, _event=None):
        self._cancel()
        self._after_id = self.widget.after(self.delay, self._show)

    def _cancel(self):
        if self._after_id is not None:
            try:
                self.widget.after_cancel(self._after_id)
            except Exception:
                pass
            self._after_id = None

    def _show(self):
        if self._tip is not None or not self.text:
            return
        try:
            x = self.widget.winfo_rootx() + 12
            y = self.widget.winfo_rooty() + self.widget.winfo_height() + 8
        except Exception:
            return
        tip = tk.Toplevel(self.widget)
        tip.wm_overrideredirect(True)
        tip.configure(bg=BORDER)
        try:
            tip.attributes("-topmost", True)
        except Exception:
            pass
        label = tk.Label(
            tip,
            text=self.text,
            justify="left",
            bg="#0F1520",
            fg=TEXT_SOFT,
            font=(UI_FONT, 9),
            padx=10,
            pady=7,
            wraplength=px(self.wraplength),
        )
        label.pack(padx=1, pady=1)
        tip.wm_geometry(f"+{x}+{y}")
        self._tip = tip

    def _hide(self, _event=None):
        self._cancel()
        if self._tip is not None:
            try:
                self._tip.destroy()
            except Exception:
                pass
            self._tip = None


class App(TkinterDnD.Tk):
    def __init__(self):
        super().__init__()
        global UI_SCALE
        try:
            UI_SCALE = min(1.6, max(1.0, self.winfo_fpixels("1i") / 96.0))
        except Exception:
            UI_SCALE = 1.0
        try:
            self.tk.call('tk', 'scaling', max(1.0, self.winfo_fpixels('1i') / 72.0))
        except Exception:
            pass
        self._set_window_icon()
        self.title(version_text())
        self.geometry(f"{px(1400)}x{px(940)}")
        self.minsize(px(1120), px(700))
        self.configure(bg=BG)

        self.files: list[Path] = []
        self.out_dir: Path | None = None
        self.msg_q: queue.Queue = queue.Queue()
        self.worker: threading.Thread | None = None
        self._cancel = threading.Event()
        self._total = 0
        self._done = 0
        self._scan_q: queue.Queue = queue.Queue()
        self._scan_thread: threading.Thread | None = None
        self._scan_stop = threading.Event()
        self._auto_out = True
        self._quick_tiles: list[QuickTile] = []
        self._autoscroll = True
        self._log_ok = 0
        self._log_err = 0
        self._prefs = settings_store.ui_prefs()
        self._row_state: dict[str, str] = {}
        self._batch_files: list[Path] = []
        self._started_at = 0.0
        self._engine = engine_info()
        self._quality = int(self._prefs.get("quality", 92) or 92)
        self._dpi = int(self._prefs.get("dpi", 200) or 200)
        self._workers = int(self._prefs.get("workers", 0) or 0)
        self._failed_files: list[Path] = []
        self._filter_text = ""
        self._row_states: dict[int, tuple[str, str]] = {}
        self._filter_job = None

        self._setup_styles()
        self._build()
        self._init_direction()
        self._center()
        self._register_drop()
        self.bind("<Control-o>", lambda _e: self.add_files())
        self.bind("<Control-O>", lambda _e: self.add_files())
        self.bind("<Control-l>", lambda _e: self._clear_log())
        self.bind("<Control-L>", lambda _e: self._clear_log())
        self.after(120, self._init_sash)
        self.after(50, self._poll_queue)
        self.after(100, self._poll_scan)
        self.after(200, self._restore_prefs)
        self.after(240, self._refresh_run_state)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _set_window_icon(self):
        """生成与界面一致的蓝色双向转换窗口图标。"""
        try:
            from PIL import Image, ImageDraw, ImageTk

            img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
            draw = ImageDraw.Draw(img)
            draw.rounded_rectangle(
                (2, 2, 62, 62), radius=15, fill="#18233C",
                outline=PRIMARY, width=3,
            )
            draw.ellipse((16, 16, 48, 48), fill=PRIMARY)
            draw.line((23, 29, 41, 29), fill="#FFFFFF", width=4)
            draw.polygon([(39, 22), (49, 29), (39, 36)], fill="#FFFFFF")
            draw.line((41, 39, 23, 39), fill="#FFFFFF", width=4)
            draw.polygon([(25, 32), (15, 39), (25, 46)], fill="#FFFFFF")
            self._window_icon = ImageTk.PhotoImage(img)
            self.iconphoto(True, self._window_icon)
        except Exception:
            pass
    def _work_area(self):
        """屏幕可用区域（物理像素）：已扣掉任务栏，窗口不会压到任务栏底下。"""
        try:
            import ctypes
            from ctypes import wintypes

            rect = wintypes.RECT()
            if ctypes.windll.user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(rect), 0):
                width = int(rect.right - rect.left)
                height = int(rect.bottom - rect.top)
                if width > 400 and height > 300:
                    return width, height
        except Exception:
            pass
        return self.winfo_screenwidth(), self.winfo_screenheight()

    def _frame_padding(self):
        """窗口外框比客户区多出来的部分（标题栏 + 边框），物理像素。"""
        try:
            import ctypes

            user32 = ctypes.windll.user32
            caption = int(user32.GetSystemMetrics(4))      # SM_CYCAPTION
            frame_x = int(user32.GetSystemMetrics(32))     # SM_CXSIZEFRAME
            frame_y = int(user32.GetSystemMetrics(33))     # SM_CYSIZEFRAME
            return max(16, frame_x * 2), max(16, caption + frame_y * 2)
        except Exception:
            return px(16), px(40)

    def _default_size(self):
        """按工作区算一个「撑满大部分屏幕、又不压到任务栏」的窗口尺寸（客户区物理像素）。"""
        work_w, work_h = self._work_area()
        pad_w, pad_h = self._frame_padding()
        # 标题栏的实际高度会随 DPI / 主题差几个像素，多留一点，宁可窗口矮一点。
        pad_w = max(pad_w, px(24))
        pad_h = max(pad_h, px(56))
        width = min(px(1500), max(1, work_w - pad_w))
        height = min(px(1000), max(1, work_h - pad_h))
        return (
            work_w,
            work_h,
            max(width, min(px(1120), work_w)),
            max(height, min(px(700), work_h)),
            pad_w,
            pad_h,
        )

    def _center(self):
        self.update_idletasks()
        # geometry 的单位是物理像素（进程已声明 DPI 感知），工作区也按物理像素返回，
        # 两者可以直接相减。旧代码先把屏幕宽度除以缩放比再减边距，在 150% 缩放的
        # 2560x1440 屏上只会开出一个 1119x639 的小窗口，底部日志区被挤成一条缝。
        work_w, work_h, width, height, _pad_w, _pad_h = self._default_size()
        self.minsize(min(px(1120), work_w), min(px(700), work_h))
        x = max(0, (work_w - width) // 2)
        y = max(0, (work_h - height) // 3)
        self.geometry(f'{width}x{height}+{x}+{y}')

    def _clamp_geometry(self, geo: str) -> str:
        """把上次保存的窗口位置/大小收回到当前屏幕工作区内。"""
        match = re.fullmatch(r"(\d+)x(\d+)([+-]\d+)([+-]\d+)", str(geo).strip())
        if not match:
            return geo
        width, height = int(match.group(1)), int(match.group(2))
        x, y = int(match.group(3)), int(match.group(4))
        work_w, work_h, default_w, default_h, pad_w, pad_h = self._default_size()
        min_w, min_h = min(px(1120), work_w), min(px(700), work_h)
        # 旧版本在 150% 缩放下算出的窗口正好卡在最小尺寸上（1678x959），
        # 这种尺寸不是用户自己选的，直接换成新的自适应尺寸并居中。
        if width <= min_w and height <= min_h:
            width, height = default_w, default_h
            x = max(0, (work_w - width - pad_w) // 2)
            y = max(0, (work_h - height - pad_h) // 3)
        width = max(min(width, work_w - pad_w), min_w)
        height = max(min(height, work_h - pad_h), min_h)
        x = min(max(0, x), max(0, work_w - width - pad_w))
        y = min(max(0, y), max(0, work_h - height - pad_h))
        return f"{width}x{height}+{x}+{y}"

    # ------------------------------------------------------------ 样式
    def _setup_styles(self):
        s = tb.Style(theme="darkly")
        self.style = s
        s.configure(".", font=FONT)
        s.configure("TCombobox", fieldbackground=SURFACE_ALT,
                    background=SURFACE_ALT, foreground=TEXT,
                    arrowcolor=TEXT_SOFT, bordercolor=BORDER,
                    lightcolor=SURFACE_ALT, darkcolor=SURFACE_ALT,
                    padding=7)
        s.map("TCombobox",
              fieldbackground=[("readonly", SURFACE_ALT)],
              background=[("readonly", SURFACE_ALT)],
              foreground=[("readonly", TEXT)],
              selectbackground=[("readonly", SURFACE_ALT)],
              selectforeground=[("readonly", TEXT)],
              bordercolor=[("focus", PRIMARY)])
        s.configure("TEntry", fieldbackground=SURFACE_ALT,
                    background=SURFACE_ALT, foreground=TEXT,
                    bordercolor=BORDER, lightcolor=SURFACE_ALT,
                    darkcolor=SURFACE_ALT, insertcolor=TEXT, padding=8)
        s.map("TEntry", bordercolor=[("focus", PRIMARY)])
        s.configure("Modern.Treeview",
                    background=SURFACE,
                    fieldbackground=SURFACE,
                    foreground=TEXT_SOFT,
                    bordercolor=SURFACE,
                    lightcolor=SURFACE,
                    darkcolor=SURFACE,
                    rowheight=px(34),
                    borderwidth=0,
                    relief="flat")
        s.configure("Modern.Treeview.Heading",
                    background=SURFACE_ALT,
                    foreground=MUTED,
                    bordercolor=SURFACE_ALT,
                    lightcolor=SURFACE_ALT,
                    darkcolor=SURFACE_ALT,
                    relief="flat",
                    font=(UI_FONT, 8, "bold"),
                    padding=(px(8), px(7)))
        s.map("Modern.Treeview",
              background=[("selected", "#2C4270")],
              foreground=[("selected", "#FFFFFF")])
        s.map("Modern.Treeview.Heading", background=[("active", SURFACE_HOVER)])
        s.configure("Modern.Horizontal.TProgressbar",
                    troughcolor=SURFACE_ALT,
                    background=PRIMARY,
                    bordercolor=SURFACE_ALT,
                    lightcolor=PRIMARY,
                    darkcolor=PRIMARY,
                    thickness=px(9))
        s.configure("Vertical.TScrollbar",
                    background=SURFACE_ALT,
                    troughcolor=SURFACE,
                    bordercolor=SURFACE,
                    arrowcolor=MUTED,
                    lightcolor=SURFACE_ALT,
                    darkcolor=SURFACE_ALT)

        s.configure('Modern.Treeview', rowheight=px(34))
        s.configure('Modern.Treeview.Heading', font=(UI_FONT, 9, 'bold'), padding=(px(8), px(8)))

    # ---------------------------------------------------------------- UI
    def _build(self):
        self.grid_rowconfigure(1, weight=1)
        self.grid_columnconfigure(0, weight=1)

        self._build_header()

        body = tk.Frame(self, bg=BG)
        body.grid(row=1, column=0, sticky="nsew", padx=px(20), pady=(0, px(16)))
        body.grid_columnconfigure(0, weight=1)
        body.grid_rowconfigure(0, weight=1)

        # 主工作区与运行日志之间使用可拖动分割条：日志默认更高，也可以随时拉大。
        self.split = tk.PanedWindow(
            body,
            orient="vertical",
            bg=BG,
            bd=0,
            sashwidth=px(10),
            sashrelief="flat",
            sashpad=0,
            showhandle=False,
            opaqueresize=True,
            sashcursor="sb_v_double_arrow",
        )
        self.split.grid(row=0, column=0, sticky="nsew")

        dashboard = tk.Frame(self.split, bg=BG)
        dashboard.grid_columnconfigure(0, weight=0, minsize=px(430))
        dashboard.grid_columnconfigure(1, weight=1)
        dashboard.grid_rowconfigure(0, weight=1)

        left = tk.Frame(dashboard, bg=BG, width=px(430))
        left.grid(row=0, column=0, sticky="nsew", padx=(0, px(14)))
        left.grid_columnconfigure(0, weight=1)
        left.grid_rowconfigure(2, weight=1)
        right = tk.Frame(dashboard, bg=BG)
        right.grid(row=0, column=1, sticky="nsew")
        right.grid_columnconfigure(0, weight=1)
        right.grid_rowconfigure(1, weight=1)

        self._build_drop_zone_v2(left)
        self._build_quick_tiles_v2(left)
        self._build_direction(left)
        self._build_output(right)
        self._build_queue(right)
        self._build_execute(right)

        log_holder = tk.Frame(self.split, bg=BG)
        log_holder.grid_rowconfigure(0, weight=1)
        log_holder.grid_columnconfigure(0, weight=1)
        self._build_log(log_holder)

        self.split.add(dashboard, minsize=px(380), stretch="always")
        self.split.add(log_holder, minsize=px(36), height=px(40), stretch="never")

    def _build_header(self):
        header = tk.Frame(self, bg=BG, height=px(78))
        header.grid(row=0, column=0, sticky="ew", padx=px(22), pady=(px(12), px(10)))
        header.grid_columnconfigure(1, weight=1)
        header.grid_propagate(False)

        logo = IconCircle(
            header,
            size=44,
            fill=PRIMARY_DARK,
            text="⇄",
            fg="#FFFFFF",
            outer_bg=BG,
            font=(UI_FONT, 16, "bold"),
        )
        logo.grid(row=0, column=0, rowspan=2, padx=(0, 12), pady=(0, 0))
        tk.Label(
            header,
            text="格式转换工具",
            font=FONT_TITLE,
            bg=BG,
            fg=TEXT,
        ).grid(row=0, column=1, sticky="sw")
        tk.Label(
            header,
            text="拖一拖，转换完成 · 图片 / PDF / Office / 音视频 / 数据批量互转",
            font=FONT_SMALL,
            bg=BG,
            fg=MUTED,
        ).grid(row=1, column=1, sticky="nw", pady=(2, 0))

        self.header_status = tk.Label(
            header,
            text="● 就绪",
            font=(UI_FONT, 9, "bold"),
            bg=SURFACE_ALT,
            fg=SUCCESS,
            padx=14,
            pady=8,
        )
        self.header_status.grid(row=0, column=2, rowspan=2, padx=(10, 10))
        self.engine_btn = PillButton(
            header,
            text=self._engine_badge_text(),
            command=self._show_settings,
            width=110,
            height=32,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=PRIMARY if self._engine.get("available") else WARNING,
            outer_bg=BG,
            font=(UI_FONT, 9, "bold"),
            radius=10,
        )
        self.engine_btn.grid(row=0, column=3, rowspan=2, padx=(0, 8))
        Tooltip(
            self.engine_btn,
            self._engine_tooltip(),
            wraplength=360,
        )
        help_btn = PillButton(
            header,
            text="使用说明",
            command=self._show_help,
            width=96,
            height=38,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=TEXT_SOFT,
            outer_bg=BG,
            font=(UI_FONT, 9, "bold"),
        )
        help_btn.grid(row=0, column=4, rowspan=2)

    def _engine_badge_text(self) -> str:
        info = getattr(self, "_engine", {}) or {}
        if info.get("available"):
            return "⚡ C++ 核心"
        mode = info.get("mode", "auto")
        if mode == "python":
            return "🐍 Python 引擎"
        return "⚠ 核心缺失"

    def _engine_tooltip(self) -> str:
        info = getattr(self, "_engine", {}) or {}
        if info.get("available"):
            lines = ["当前由 C++ 核心执行转换（速度更快、支持取消）。"]
            cli = info.get("cli", "")
            if cli:
                lines.append(f"核心路径：{cli}")
            core = info.get("core_version", "")
            if core:
                lines.append(f"核心版本：{core}")
            lines.append("点击打开设置：切换引擎、指定 LibreOffice / FFmpeg、调整默认参数。")
            return "\n".join(lines)
        lines = ["没有找到 bin/fccli.exe，将使用内置 Python 引擎（较慢，部分格式不支持）。"]
        lines.append("点击打开设置，可查看原因与手动指定核心位置。")
        return "\n".join(lines)

    def _build_drop_zone_v2(self, parent):
        card = RoundedCard(parent, fill=DROP_BLUE, outline=DROP_BLUE_EDGE, radius=18, padding=11, height=162)
        card.grid(row=0, column=0, sticky='nsew')
        self.drop_card = card
        box = card.inner
        icon = IconCircle(box, size=38, fill='#315A91', text='↓', fg='#E6EFFF', outer_bg=DROP_BLUE, font=(UI_FONT, 16, 'bold'))
        icon.pack(pady=(0, 4))
        tk.Label(box, text='拖入文件，立即转换', font=(UI_FONT, 14, 'bold'), bg=DROP_BLUE, fg='#FFFFFF').pack()
        tk.Label(box, text='点击这里选择文件，或把文件 / 文件夹直接拖进来', font=FONT_SMALL, bg=DROP_BLUE, fg='#D3DFF1').pack(pady=(2, 0))
        tk.Label(box, text='支持多选与整个文件夹；分类卡片只切换源格式', font=FONT_TINY, bg=DROP_BLUE, fg='#9FB4D1').pack(pady=(2, 0))
        tk.Label(box, text='图片 · PDF · Office · 音视频 · 数据 · 文本', font=FONT_TINY, bg=DROP_BLUE, fg='#8FA9CB').pack(pady=(2, 0))

        # 整个蓝色区域都可以点击选文件；拖放仍然有效。
        for widget in [card, box, icon, *box.winfo_children()]:
            widget.bind("<Button-1>", lambda _e: self.add_files())
            try:
                widget.configure(cursor="hand2")
            except Exception:
                pass

    def _build_quick_tiles_v2(self, parent):
        wrap = tk.Frame(parent, bg=BG)
        wrap.grid(row=1, column=0, sticky='ew', pady=(8, 8))
        for col in range(3):
            wrap.grid_columnconfigure(col, weight=1, uniform='quick')
        tiles = [
            ('▧', '图片', 'HEIC · AVIF', DROP_BLUE, '#5FB7DD', ['图片'],
             '图片格式互转：HEIC / AVIF / JPG / PNG / WebP / BMP / TIFF / GIF / ICO，\n还可以多图合成 GIF，或把多张图分别/合并导出为 PDF。'),
            ('◈', 'Office', 'Word · Excel · PPT', OFFICE_BG, OFFICE_ACCENT, ['Word', 'Excel', 'PPT'],
             'Office 系列：Word / Excel / PPT 与 PDF、图片、纯文本互转，\n也支持 doc / xls / ppt / odt / rtf / csv 等新旧格式互转。'),
            ('▤', 'PDF', '合并 · 拆分', PDF_BG, PDF_ACCENT, ['PDF'],
             'PDF 工具箱：转图片 / Word / Excel / PPT / 纯文本，\n多文件合并、逐页拆分、页面旋转和内容流压缩。'),
            ('▶', '音视频', 'MP4 · MP3 · GIF', MEDIA_BG, MEDIA_ACCENT, ['音视频'],
             '音视频转换：MP4 / MKV / MOV / WebM / AVI / WMV / GIF 互转，\nMP3 / WAV / FLAC / M4A / AAC / OGG / OPUS / WMA，视频可直接提取音轨。'),
            ('{}', '数据', 'JSON · YAML', DATA_BG, DATA_ACCENT, ['数据'],
             '结构化数据：JSON / YAML / XML 互转，可展开为 Excel 表格；\n也能把 Excel / CSV 的内容转回 JSON。'),
            ('T', '文本', 'TXT · Markdown', TEXT_TILE_BG, TEXT_TILE_ACCENT, ['文本'],
             '文本类：TXT / Markdown 直接排版为 PDF 或 Word 文档。'),
        ]
        self._quick_groups = []
        self._quick_tiles = []
        for index, (icon, title, subtitle, fill, accent, group, tip) in enumerate(tiles):
            row, col = divmod(index, 3)
            tile = CompactQuickTile(
                wrap, icon=icon, title=title, subtitle=subtitle,
                fill=fill, accent=accent, outer_bg=BG,
                command=lambda idx=index: self._quick_pick(tiles[idx][5]),
            )
            tile.grid(row=row, column=col, sticky='nsew', padx=4, pady=3)
            Tooltip(tile, f'点击把源格式切换为「{title}」，不会弹出选文件窗口；\n选文件请点上方蓝色区域或右侧「添加文件」。\n{tip}', wraplength=340)
            self._quick_tiles.append(tile)
            self._quick_groups.append(group)

    def _build_direction(self, parent):
        card = RoundedCard(
            parent,
            fill=SURFACE,
            outline=BORDER,
            radius=18,
            padding=10,
        )
        card.grid(row=2, column=0, sticky="nsew")
        box = card.inner

        head = tk.Frame(box, bg=SURFACE)
        head.pack(fill="x")
        tk.Label(
            head,
            text="转换方向",
            font=FONT_SECTION,
            bg=SURFACE,
            fg=TEXT,
        ).pack(side="left")
        tk.Label(
            head,
            text="① 选源格式  ② 选目标格式",
            font=FONT_TINY,
            bg=SURFACE,
            fg=PRIMARY,
        ).pack(side="right")

        controls = tk.Frame(box, bg=SURFACE)
        controls.pack(fill="x")
        controls.grid_columnconfigure(0, weight=1)
        controls.grid_columnconfigure(2, weight=1)

        src_wrap = tk.Frame(controls, bg=SURFACE)
        src_wrap.grid(row=0, column=0, sticky="ew")
        tk.Label(src_wrap, text="源格式 · 文件现在是什么", font=FONT_TINY, bg=SURFACE,
                 fg=MUTED).pack(anchor="w")
        self.cmb_src = tb.Combobox(src_wrap, state="readonly",
                                   font=FONT, width=11)
        self.cmb_src.pack(fill="x", pady=(3, 0))

        self.swap_btn = PillButton(
            controls,
            text="⇄",
            command=self.swap_direction,
            width=42,
            height=36,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=PRIMARY,
            outer_bg=SURFACE,
            font=(UI_FONT, 15, "bold"),
            radius=10,
        )
        self.swap_btn.grid(row=0, column=1, padx=8, sticky="s", pady=(20, 0))
        Tooltip(self.swap_btn, "一键互换源格式与目标格式（反方向没有实现时会提示）")

        dst_wrap = tk.Frame(controls, bg=SURFACE)
        dst_wrap.grid(row=0, column=2, sticky="ew")
        tk.Label(dst_wrap, text="目标格式 · 想变成什么", font=FONT_TINY, bg=SURFACE,
                 fg=MUTED).pack(anchor="w")
        self.cmb_dst = tb.Combobox(dst_wrap, state="readonly",
                                   font=FONT, width=11)
        self.cmb_dst.pack(fill="x", pady=(3, 0))

        self.detail = tk.Frame(box, bg=SURFACE)

        self.lbl_summary = tk.Label(
            self.detail,
            text="",
            font=(UI_FONT, 9, "bold"),
            bg=SURFACE,
            fg=PRIMARY_HOVER,
            anchor="w",
            justify="left",
        )
        self.lbl_summary.pack(fill="x", pady=(6, 0))

        self.options_row = tk.Frame(self.detail, bg=SURFACE)
        self.options_row.pack(fill="x", pady=(4, 0))
        self.options_row.grid_columnconfigure(0, weight=1)
        self.options_row.grid_columnconfigure(1, weight=0)

        self.frm_mode = tk.Frame(self.options_row, bg=SURFACE)
        tk.Label(self.frm_mode, text="方式", font=FONT_TINY, bg=SURFACE,
                 fg=MUTED).pack(side="left", padx=(0, 6))
        self.cmb_mode = tb.Combobox(self.frm_mode, state="readonly",
                                    font=FONT, width=10)
        self.cmb_mode.pack(side="left", fill="x", expand=True)

        self.frm_opt = tk.Frame(self.options_row, bg=SURFACE)
        self.lbl_opt = tk.Label(self.frm_opt, text="", font=FONT_TINY,
                                 bg=SURFACE, fg=MUTED)
        self.lbl_opt.pack(side="left", padx=(0, 6))
        self.cmb_opt = tb.Combobox(self.frm_opt, state="readonly",
                                    font=FONT, width=8)
        self.cmb_opt.pack(side="left")
        self.frm_opt.grid(row=0, column=1, sticky="e", padx=(10, 0))
        self.frm_opt.grid_remove()


        self.desc_box = tk.Frame(self.detail, bg=SURFACE_ALT)
        self.desc_box.pack(fill="x", pady=(4, 0))
        self.lbl_desc = tk.Label(
            self.desc_box,
            text="",
            font=FONT_TINY,
            bg=SURFACE_ALT,
            fg=TEXT_SOFT,
            justify="left",
            anchor="w",
            padx=10,
            pady=6,
            wraplength=330,
        )
        self.lbl_desc.pack(fill="x")

        self.cmb_src.bind("<<ComboboxSelected>>", self._on_src_change)
        self.cmb_dst.bind("<<ComboboxSelected>>", self._on_dst_change)
        self.cmb_mode.bind("<<ComboboxSelected>>", self._on_mode_change)

        self.direction_hint = tk.Label(
            box,
            text="请先在上方选择源格式和目标格式；选好后这里才会出现可用的转换方式。",
            font=FONT_TINY,
            bg=SURFACE_ALT,
            fg=MUTED,
            anchor="w",
            justify="left",
            padx=10,
            pady=8,
            wraplength=330,
        )
        self.direction_hint.pack(fill="x", pady=(6, 0))

        # 左侧栏在 1080p 以上会多出不少空白，这里放常驻的快捷键提示，
        # 既不浪费空间，也把「还有什么操作」告诉第一次用的人。
        self.tips_box = tk.Frame(box, bg=SURFACE)
        # 用 side="bottom" 占住卡片底部：详情区（当前任务/方式/说明）是随后按需
        # pack 的，若这里用 expand 会把详情挤到最下面。
        self.tips_box.pack(side="bottom", fill="x", pady=(12, 0))
        tk.Frame(self.tips_box, bg=BORDER, height=1).pack(fill="x", pady=(0, 8))
        tk.Label(
            self.tips_box, text="快捷键与小技巧", font=(UI_FONT, 9, "bold"),
            bg=SURFACE, fg=MUTED,
        ).pack(anchor="w")
        for _line in (
            "Ctrl+O 添加文件　Ctrl+L 清空日志",
            "把整个文件夹拖进来会递归收集文件",
            "转换中可以随时取消，已完成的文件保留",
            "双击队列里的文件，直接打开所在文件夹",
        ):
            tk.Label(
                self.tips_box, text="· " + _line, font=FONT_TINY, bg=SURFACE,
                fg=TEXT_SOFT, anchor="w", justify="left",
            ).pack(fill="x", pady=(3, 0))

        box.bind(
            "<Configure>",
            lambda e: (
                self.lbl_desc.configure(wraplength=max(240, e.width - 46)),
                self.direction_hint.configure(wraplength=max(240, e.width - 46)),
            ),
        )

    def _build_output(self, parent):
        card = RoundedCard(
            parent,
            fill=SURFACE,
            outline=BORDER,
            radius=18,
            padding=12,
            height=88,
        )
        card.grid(row=0, column=0, sticky="ew", pady=(0, 12))
        box = card.inner
        top = tk.Frame(box, bg=SURFACE)
        top.pack(fill="x")
        tk.Label(top, text="输出位置", font=FONT_SECTION, bg=SURFACE,
                 fg=TEXT).pack(side="left")
        tk.Label(top, text="默认放在首个文件旁的「格式转换输出」；同名自动编号，不覆盖原文件", font=FONT_TINY,
                 bg=SURFACE, fg=MUTED).pack(side="left", padx=(10, 0))
        row = tk.Frame(box, bg=SURFACE)
        row.pack(fill="x", pady=(8, 0))
        self.ent_out = tb.Entry(row, font=FONT)
        self.ent_out.pack(side="left", fill="x", expand=True, padx=(0, 8))
        self.ent_out.bind("<KeyRelease>", self._mark_output_manual)
        self.out_btn = PillButton(
            row,
            text="浏览文件夹",
            command=self.pick_out_dir,
            width=104,
            height=36,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=TEXT_SOFT,
            outer_bg=SURFACE,
            font=(UI_FONT, 9, "bold"),
        )
        self.out_btn.pack(side="right")
        Tooltip(self.out_btn, "选择一个文件夹作为输出目录；修改后本批次将改用新目录")
        self.open_out_btn = PillButton(
            row,
            text="打开目录",
            command=self.open_out_dir,
            width=84,
            height=36,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=TEXT_SOFT,
            outer_bg=SURFACE,
            font=(UI_FONT, 9, "bold"),
        )
        self.open_out_btn.pack(side="right", padx=(0, 7))
        Tooltip(self.open_out_btn, "在资源管理器中打开当前输出文件夹；不存在时会自动创建")

    def _build_queue(self, parent):
        card = RoundedCard(
            parent,
            fill=SURFACE,
            outline=BORDER,
            radius=18,
            padding=12,
            height=160,
        )
        card.grid(row=1, column=0, sticky="nsew")
        box = card.inner
        box.grid_rowconfigure(2, weight=1)
        box.grid_columnconfigure(0, weight=1)

        head = tk.Frame(box, bg=SURFACE)
        head.grid(row=0, column=0, sticky="ew")
        tk.Label(head, text="文件队列", font=FONT_SECTION, bg=SURFACE,
                 fg=TEXT).pack(side="left")
        self.lbl_file_count = tk.Label(
            head,
            text="0 个文件",
            font=(UI_FONT, 9, "bold"),
            bg=SURFACE_ALT,
            fg=PRIMARY,
            padx=10,
            pady=4,
        )
        self.lbl_file_count.pack(side="left", padx=(9, 0))
        # 队列一大就难找文件，这里给一个即时筛选框（只影响显示，不影响转换范围）。
        self.search_entry = tb.Entry(head, font=FONT_TINY, width=16)
        self.search_entry.pack(side="right", padx=(6, 0))
        self.search_entry.bind("<KeyRelease>", lambda _e: self._schedule_filter())
        tk.Label(
            head,
            text="筛选",
            font=FONT_TINY,
            bg=SURFACE,
            fg=MUTED,
        ).pack(side="right")

        toolbar = tk.Frame(box, bg=SURFACE)
        toolbar.grid(row=1, column=0, sticky="ew", pady=(10, 9))
        self.add_file_btn = PillButton(
            toolbar,
            text="＋ 添加文件",
            command=self.add_files,
            width=104,
            height=34,
            fill=PRIMARY,
            hover=PRIMARY_HOVER,
            fg="#FFFFFF",
            outer_bg=SURFACE,
        )
        self.add_file_btn.pack(side="left", padx=(0, 7))
        Tooltip(self.add_file_btn, "一次可以选择多个文件（Ctrl+O 也可以）")
        self.add_folder_btn = PillButton(
            toolbar,
            text="＋ 添加文件夹",
            command=self.add_folder,
            width=112,
            height=34,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=TEXT_SOFT,
            outer_bg=SURFACE,
        )
        self.add_folder_btn.pack(side="left", padx=(0, 7))
        Tooltip(self.add_folder_btn, "递归扫描文件夹内所有支持的格式，每 200 个一批加入队列")
        self.btn_scan_stop = PillButton(
            toolbar,
            text="停止扫描",
            command=lambda: self._scan_stop.set(),
            width=86,
            height=34,
            fill=WARNING,
            hover="#FFC87A",
            fg="#1A1208",
            outer_bg=SURFACE,
        )
        self.remove_btn = PillButton(
            toolbar,
            text="移除选中",
            command=self.remove_selected,
            width=86,
            height=34,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=TEXT_SOFT,
            outer_bg=SURFACE,
        )
        self.remove_btn.pack(side="left", padx=(0, 7))
        self.clear_btn = PillButton(
            toolbar,
            text="清空",
            command=self.clear_files,
            width=62,
            height=34,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=MUTED,
            outer_bg=SURFACE,
        )
        self.clear_btn.pack(side="left")

        tree_wrap = tk.Frame(box, bg=SURFACE)
        tree_wrap.grid(row=2, column=0, sticky="nsew")
        tree_wrap.grid_rowconfigure(0, weight=1)
        tree_wrap.grid_columnconfigure(0, weight=1)

        self.tree = ttk.Treeview(
            tree_wrap,
            columns=("name", "state", "kind", "size", "folder"),
            show="headings",
            selectmode="extended",
            style="Modern.Treeview",
        )
        self.tree.heading("name", text="文件名")
        self.tree.heading("state", text="状态")
        self.tree.heading("kind", text="类型")
        self.tree.heading("size", text="大小")
        self.tree.heading("folder", text="所在文件夹")
        # 列宽总和控制在右侧卡片约 780px 内，避免出现横向滚动把状态列挤出去。
        self.tree.column("name", width=px(200), minwidth=px(120), stretch=True)
        self.tree.column("state", width=px(122), minwidth=px(88), stretch=False)
        self.tree.column("kind", width=px(78), minwidth=px(60), stretch=False)
        self.tree.column("size", width=px(68), minwidth=px(56), stretch=False)
        self.tree.column("folder", width=px(170), minwidth=px(90), stretch=True)
        self.tree.tag_configure("ok", foreground=SUCCESS)
        self.tree.tag_configure("err", foreground=ERROR)
        self.tree.tag_configure("running", foreground=WARNING)
        self.tree.tag_configure("skip", foreground=MUTED)
        self.tree.grid(row=0, column=0, sticky="nsew")
        self.tree.bind("<Double-1>", self._open_selected_location)
        sb = ttk.Scrollbar(tree_wrap, orient="vertical", command=self.tree.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.tree.configure(yscrollcommand=sb.set)

        self.empty_hint = tk.Label(
            tree_wrap,
            text="还没有添加文件\n拖进来，或者点击“添加文件”",
            font=FONT_SMALL,
            bg=SURFACE,
            fg=MUTED,
            justify="center",
        )
        self.empty_hint.place(relx=0.5, rely=0.52, anchor="center")
        self._update_empty_state()

    def _build_execute(self, parent):
        card = RoundedCard(
            parent,
            fill=SURFACE,
            outline=BORDER,
            radius=18,
            padding=14,
            height=150,
        )
        card.grid(row=2, column=0, sticky="ew", pady=(12, 0))
        box = card.inner
        top = tk.Frame(box, bg=SURFACE)
        top.pack(fill="x")
        tk.Label(top, text="准备转换", font=FONT_SECTION, bg=SURFACE,
                 fg=TEXT).pack(side="left")
        self.lbl_status = tk.Label(
            top,
            text="等待添加文件",
            font=FONT_TINY,
            bg=SURFACE,
            fg=MUTED,
        )
        self.lbl_status.pack(side="right")
        self.lbl_progress_text = tk.Label(
            top,
            text="0 / 0",
            font=(UI_FONT, 9, "bold"),
            bg=SURFACE,
            fg=TEXT_SOFT,
        )
        self.lbl_progress_text.pack(side="right", padx=(0, 12))

        # 输出质量 / 渲染 DPI：只在对应转换方式下出现。
        # 放在执行卡片里而不是方向卡片里，避免把左侧内容撑高、挤掉开始按钮。
        self.frm_tuning = tk.Frame(box, bg=SURFACE)
        self.frm_tuning.pack(fill="x", pady=(8, 0))

        self.row_quality = tk.Frame(self.frm_tuning, bg=SURFACE)
        tk.Label(self.row_quality, text="输出质量", font=FONT_TINY, bg=SURFACE,
                 fg=MUTED).pack(side="left", padx=(0, 8))
        self.scale_quality = tk.Scale(
            self.row_quality,
            from_=10,
            to=100,
            orient="horizontal",
            bg=SURFACE,
            fg=TEXT_SOFT,
            troughcolor=SURFACE_ALT,
            activebackground=PRIMARY,
            highlightthickness=0,
            bd=0,
            width=px(12),
            sliderrelief="flat",
            sliderlength=px(14),
            showvalue=False,
            length=px(180),
            command=self._on_quality_change,
        )
        self.scale_quality.set(self._quality)
        self.scale_quality.pack(side="left", padx=(0, 6))
        self.lbl_quality = tk.Label(
            self.row_quality, text=f"{self._quality}", font=(UI_FONT, 9, "bold"),
            bg=SURFACE, fg=PRIMARY_HOVER, width=4,
        )
        self.lbl_quality.pack(side="left")
        Tooltip(
            self.scale_quality,
            "仅影响 JPG / WebP 等有损格式：数值越高越清晰、体积越大。\n默认 92，通常 80~95 足够。",
            wraplength=320,
        )

        self.row_dpi = tk.Frame(self.frm_tuning, bg=SURFACE)
        tk.Label(self.row_dpi, text="渲染 DPI", font=FONT_TINY, bg=SURFACE,
                 fg=MUTED).pack(side="left", padx=(0, 8))
        self.cmb_dpi = tb.Combobox(
            self.row_dpi, state="readonly", font=FONT_SMALL, width=6,
            values=["100", "150", "200", "300", "450"],
        )
        self.cmb_dpi.set(str(self._dpi))
        self.cmb_dpi.pack(side="left", padx=(0, 10))
        self.cmb_dpi.bind("<<ComboboxSelected>>", self._on_dpi_change)
        tk.Label(
            self.row_dpi, text="屏幕看 200，打印用 300", font=FONT_TINY,
            bg=SURFACE, fg=MUTED,
        ).pack(side="left")
        Tooltip(
            self.cmb_dpi,
            "PDF / Office 转图片时的渲染精度。\n200 适合屏幕查看，300 适合打印。",
            wraplength=320,
        )
        self.row_quality.pack_forget()
        self.row_dpi.pack_forget()

        toolbar = tk.Frame(box, bg=SURFACE)
        toolbar.pack(fill="x", pady=(8, 0))
        self.btn_run = PillButton(
            toolbar,
            text="▶  开始转换",
            command=self.start_convert,
            width=160,
            height=48,
            fill=PRIMARY,
            hover=PRIMARY_HOVER,
            fg="#FFFFFF",
            outer_bg=SURFACE,
            font=(UI_FONT, 11, "bold"),
            radius=14,
        )
        self.btn_run.pack(side="left", padx=(0, 14))
        Tooltip(self.btn_run, "按当前的方向和输出位置开始转换；转换中不能重复点击")
        self.btn_cancel = PillButton(
            toolbar,
            text="取消",
            command=self.cancel_convert,
            width=78,
            height=48,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=TEXT_SOFT,
            disabled_fill="#1A2130",
            disabled_fg="#4E5A70",
            outer_bg=SURFACE,
            font=(UI_FONT, 10, "bold"),
            radius=14,
        )
        self.btn_cancel.pack(side="left", padx=(0, 14))
        self.btn_cancel.set_enabled(False)
        Tooltip(self.btn_cancel, "停止后续文件；正在转换的那一个会先完成")
        progress_wrap = tk.Frame(toolbar, bg=SURFACE)
        progress_wrap.pack(side="left", fill="x", expand=True)
        self.pbar = ttk.Progressbar(
            progress_wrap,
            mode="determinate",
            style="Modern.Horizontal.TProgressbar",
        )
        self.pbar.pack(fill="x", pady=(10, 0))
        self.lbl_hint = tk.Label(
            progress_wrap,
            text="确认方向与输出位置后，点左侧按钮开始",
            font=FONT_TINY,
            bg=SURFACE,
            fg=MUTED,
        )
        self.lbl_hint.pack(anchor="w", pady=(6, 0))

    def _build_log(self, parent):
        card = RoundedCard(
            parent,
            fill=SURFACE,
            outline=BORDER,
            radius=18,
            padding=8,
            height=40,
        )
        card.grid(row=0, column=0, sticky="nsew")
        box = card.inner
        box.grid_rowconfigure(1, weight=1)
        box.grid_columnconfigure(0, weight=1)

        head = tk.Frame(box, bg=SURFACE)
        head.grid(row=0, column=0, sticky="ew")
        tk.Label(head, text="运行日志", font=FONT_SECTION, bg=SURFACE,
                 fg=TEXT).pack(side="left")

        self.lbl_log_ok = tk.Label(
            head, text="成功 0", font=FONT_TINY, bg=SURFACE_ALT, fg=SUCCESS,
            padx=8, pady=3,
        )
        self.lbl_log_ok.pack(side="left", padx=(10, 0))
        self.lbl_log_err = tk.Label(
            head, text="失败 0", font=FONT_TINY, bg=SURFACE_ALT, fg=MUTED,
            padx=8, pady=3,
        )
        self.lbl_log_err.pack(side="left", padx=(6, 0))

        self.clear_log_btn = PillButton(
            head,
            text="清空日志",
            command=self._clear_log,
            width=82,
            height=28,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=MUTED,
            outer_bg=SURFACE,
            font=FONT_TINY,
            radius=9,
        )
        self.clear_log_btn.pack(side="right")
        self.copy_log_btn = PillButton(
            head,
            text="复制全部",
            command=self._copy_log,
            width=82,
            height=28,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=TEXT_SOFT,
            outer_bg=SURFACE,
            font=FONT_TINY,
            radius=9,
        )
        self.copy_log_btn.pack(side="right", padx=(0, 6))
        self.scroll_btn = PillButton(
            head,
            text="自动滚动：开",
            command=self._toggle_autoscroll,
            width=104,
            height=28,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=TEXT_SOFT,
            outer_bg=SURFACE,
            font=FONT_TINY,
            radius=9,
        )
        self.scroll_btn.pack(side="right", padx=(0, 6))
        self.log_toggle = PillButton(
            head,
            text="展开日志",
            command=self._toggle_log,
            width=86,
            height=28,
            fill=SURFACE_ALT,
            hover=SURFACE_HOVER,
            fg=PRIMARY_HOVER,
            outer_bg=SURFACE,
            font=FONT_TINY,
            radius=9,
        )
        self.log_toggle.pack(side="right", padx=(0, 6))
        Tooltip(
            self.log_toggle,
            "日志默认收起，给文件队列留出空间。\n也可以直接拖动上方分割条调整高度。",
            wraplength=300,
        )

        self.lbl_log_hint = tk.Label(
            head,
            text="成功与失败原因都会完整记录，可拖动上方分割条调高日志区",
            font=FONT_TINY,
            bg=SURFACE,
            fg=MUTED,
            anchor="w",
        )
        self.lbl_log_hint.pack(side="left", padx=(12, 0))

        log_wrap = tk.Frame(box, bg=SURFACE)
        log_wrap.grid(row=1, column=0, sticky="nsew", pady=(6, 0))
        log_wrap.grid_rowconfigure(0, weight=1)
        log_wrap.grid_columnconfigure(0, weight=1)

        self.txt_log = tk.Text(
            log_wrap,
            height=6,
            state="disabled",
            wrap="word",
            relief="flat",
            font=(UI_FONT, 10),
            bg="#0D121B",
            fg=TEXT_SOFT,
            insertbackground=TEXT,
            padx=12,
            pady=9,
            highlightthickness=1,
            highlightbackground=BORDER,
            highlightcolor=PRIMARY,
            borderwidth=0,
            spacing1=1,
            spacing3=2,
        )
        self.txt_log.grid(row=0, column=0, sticky="nsew")
        log_sb = ttk.Scrollbar(log_wrap, orient="vertical",
                               command=self.txt_log.yview)
        log_sb.grid(row=0, column=1, sticky="ns")
        self.txt_log.configure(yscrollcommand=log_sb.set)
        self.txt_log.tag_configure("ok", foreground=SUCCESS)
        self.txt_log.tag_configure("err", foreground=ERROR)
        self.txt_log.tag_configure("warn", foreground=WARNING)
        self.txt_log.tag_configure("info", foreground=TEXT_SOFT)
        self.txt_log.tag_configure("muted", foreground=MUTED)
        self.txt_log.tag_configure("time", foreground="#5C6B84", font=(UI_FONT, 9))

        self.log_empty = tk.Label(
            log_wrap,
            text="暂无日志\n转换开始后，这里的每一行都会说明做了什么、成功还是失败",
            font=FONT_SMALL,
            bg="#0D121B",
            fg=MUTED,
            justify="center",
        )
        self.log_empty.place(relx=0.5, rely=0.5, anchor="center")

    # ------------------------------------------------------ 拖放与快捷选择
    def _register_drop(self):
        widgets = [self, self.drop_card, self.drop_card.inner]
        try:
            for widget in widgets:
                widget.drop_target_register(DND_FILES)
                widget.dnd_bind("<<Drop>>", self._on_drop)
        except tk.TclError:
            pass

    def _on_drop(self, event):
        raw = event.data or ""
        try:
            parts = self.tk.splitlist(raw)
        except tk.TclError:
            parts = [raw]
        files: list[Path] = []
        dirs: list[Path] = []
        for part in parts:
            p = Path(str(part).strip("{}"))
            if p.is_dir():
                dirs.append(p)
            elif p.is_file():
                files.append(p)
        if files:
            self._add_paths(files)
        if dirs:
            self._start_scan(dirs)
        return "break"

    def _quick_pick(self, categories: list[str]):
        """分类卡片只切换源格式，不再弹出文件选择框。"""
        if self._src() in categories:
            return
        primary = categories[0]
        if primary not in self.cmb_src["values"]:
            return
        self.cmb_src.set(primary)
        self._on_src_change()
        if len(categories) > 1:
            self._log(
                f"源格式已切换为「{primary}」，如果文件是 {' / '.join(categories[1:])}，请在上方下拉框改选",
                "muted",
            )
        else:
            self._log(f"源格式已切换为「{primary}」，请选择目标格式", "muted")

    # ------------------------------------------------------ 转换方向联动
    def _init_direction(self):
        self.cmb_src["values"] = categories_src()
        self.cmb_src.set(PICK_HINT)
        self.cmb_dst["values"] = ()
        self.cmb_dst.set(PICK_HINT)
        self.cmb_dst.configure(state="disabled")
        self._rebuild_modes()

    def _src(self) -> str:
        value = self.cmb_src.get()
        return "" if value == PICK_HINT else value

    def _dst(self) -> str:
        value = self.cmb_dst.get()
        return "" if value == PICK_HINT else value

    def _on_src_change(self, _event=None):
        src = self._src()
        if not src:
            self.cmb_dst["values"] = ()
            self.cmb_dst.set(PICK_HINT)
            self.cmb_dst.configure(state="disabled")
            self._rebuild_modes()
            self._sync_quick_tiles()
            return
        self.cmb_dst["values"] = dsts_for(src)
        self.cmb_dst.configure(state="readonly")
        if self.cmb_dst.get() not in self.cmb_dst["values"]:
            self.cmb_dst.set(PICK_HINT)
        self._rebuild_modes()
        self._sync_quick_tiles()

    def _on_dst_change(self, _event=None):
        self._rebuild_modes()

    def _show_direction_detail(self, show: bool):
        """两个格式都选好后，才展开方式/参数/说明区域。"""
        if show:
            self.direction_hint.pack_forget()
            self.detail.pack(fill="x")
        else:
            self.detail.pack_forget()
            self.direction_hint.pack(fill="x", pady=(6, 0))

    def _rebuild_modes(self):
        src, dst = self._src(), self._dst()
        if not src or not dst:
            self.frm_mode.grid_remove()
            self.direction_hint.config(
                text="请先在上方选择源格式和目标格式；选好后这里才会出现可用的转换方式。"
            )
            self._show_direction_detail(False)
            return
        convs = convs_for(src, dst)
        if not convs:
            self.frm_mode.grid_remove()
            self.direction_hint.config(
                text=f"暂无「{src} → {dst}」的转换实现，可以换一个目标格式，或点中间 ⇄ 试试反方向。"
            )
            self._show_direction_detail(False)
            return
        self._show_direction_detail(True)
        if len(convs) > 1:
            self.cmb_mode["values"] = [c.label for c in convs]
            self.cmb_mode.current(0)
            self.frm_mode.grid(row=0, column=0, sticky="ew", padx=(0, 10))
        else:
            self.frm_mode.grid_remove()
        self._on_mode_change()

    def _on_mode_change(self, _event=None):
        conv = self._current_conv()
        if conv is None:
            return
        self.lbl_summary.config(text=f"当前任务：{conv.src} → {conv.dst} · {conv.label}")
        self.lbl_desc.config(text=conv.description or "")
        if conv.accepts:
            self.lbl_opt.config(text=conv.accepts_label)
            self.cmb_opt["values"] = list(conv.accepts)
            self.cmb_opt.current(0)
            self.frm_opt.grid(row=0, column=1, sticky="e", padx=(10, 0))
        else:
            self.frm_opt.grid_remove()
        self._sync_tuning(conv)

    # 支持输出质量与渲染 DPI 的转换方式
    QUALITY_OPS = {"image_convert"}
    DPI_OPS = {"pdf_to_image", "word_to_image", "excel_to_image", "ppt_to_image"}

    def _sync_tuning(self, conv):
        op = getattr(conv, "op", "")
        show_quality = op in self.QUALITY_OPS
        show_dpi = op in self.DPI_OPS
        if show_quality:
            self.row_quality.pack(fill="x")
        else:
            self.row_quality.pack_forget()
        if show_dpi:
            self.row_dpi.pack(fill="x")
        else:
            self.row_dpi.pack_forget()

    def _on_quality_change(self, value):
        try:
            self._quality = max(10, min(100, int(float(value))))
        except (TypeError, ValueError):
            return
        self.lbl_quality.config(text=str(self._quality))

    def _on_dpi_change(self, _event=None):
        try:
            self._dpi = int(self.cmb_dpi.get())
        except (TypeError, ValueError):
            self._dpi = 200
        settings_store.update_ui(dpi=self._dpi)

    def _current_options(self, conv) -> dict:
        """把界面上的质量 / DPI / 并发转成 C++ 核心认识的 options。"""
        op = getattr(conv, "op", "")
        options: dict = {}
        if op in self.QUALITY_OPS:
            options["quality"] = self._quality
        if op in self.DPI_OPS:
            options["dpi"] = self._dpi
        if self._workers:
            options["workers"] = self._workers
        return options

    def _current_conv(self):
        src, dst = self._src(), self._dst()
        convs = convs_for(src, dst)
        if not convs:
            return None
        if len(convs) == 1:
            return convs[0]
        i = self.cmb_mode.current()
        if i < 0:
            i = 0
        return convs[i]

    def swap_direction(self):
        src, dst = self._src(), self._dst()
        if not src or not dst:
            messagebox.showinfo("提示", "请先选择源格式和目标格式，再使用互换。")
            return
        if not pair_exists(dst, src):
            messagebox.showinfo(
                "不支持的反方向",
                f"暂无「{dst} → {src}」的转换实现，可先从下拉框选择其他目标格式。",
            )
            return
        self.cmb_src.set(dst)
        self.cmb_dst.set(src)
        self._rebuild_modes()
        self._sync_quick_tiles()

    def _auto_detect_src(self):
        exts = {f.suffix.lower() for f in self.files}
        if not exts:
            return
        cats = [
            cat for cat, cat_exts in CATEGORY_EXTS.items()
            if exts <= set(cat_exts)
        ]
        if len(cats) == 1 and not self._src():
            if cats[0] in self.cmb_src["values"]:
                self.cmb_src.set(cats[0])
                self._on_src_change()
                self._log(f"已根据文件自动选择源格式：{cats[0]}，请选择目标格式", "info")

    # ------------------------------------------------------------- 输入操作
    def add_files(self):
        picked = filedialog.askopenfilenames(
            title="选择输入文件",
            filetypes=[("所有文件", "*.*")],
        )
        if picked:
            self._add_paths([Path(p) for p in picked])

    def _add_paths(self, paths):
        existing = set(self.files)
        added_paths: list[Path] = []
        for path in paths:
            if path.is_file() and path not in existing:
                existing.add(path)
                added_paths.append(path)
        if not added_paths:
            return
        # 只追加新增行，避免大批量拖入时反复重建整个列表。
        start = len(self.files)
        self.files.extend(added_paths)
        if self._filter_text:
            # 正在筛选时新增文件也要走一次过滤，避免出现不符合条件的行。
            self._apply_filter()
        else:
            for i, path in enumerate(added_paths, start=start):
                self._insert_tree_row(i, path)
        self._update_file_count()
        self._auto_detect_src()
        self._set_default_output()
        self._log(f"已添加 {len(added_paths)} 个文件", "info")

    def add_folder(self):
        if self._scan_thread and self._scan_thread.is_alive():
            return
        d = filedialog.askdirectory(title="选择输入文件夹")
        if d:
            self._start_scan([Path(d)])

    def _start_scan(self, roots: list[Path]):
        if self._scan_thread and self._scan_thread.is_alive():
            return
        all_exts = set()
        for exts in CATEGORY_EXTS.values():
            all_exts.update(exts)
        self._scan_stop.clear()
        self.btn_scan_stop.pack(side="left", padx=(0, 7))
        self.lbl_file_count.config(text="扫描中…")
        self._scan_thread = threading.Thread(
            target=self._scan_folder,
            args=(roots, all_exts),
            daemon=True,
        )
        self._scan_thread.start()

    def _scan_folder(self, roots: list[Path], exts: set):
        added = scanned = 0
        batch: list[Path] = []
        try:
            for root in roots:
                if self._scan_stop.is_set():
                    break
                for f in root.rglob("*"):
                    if self._scan_stop.is_set():
                        break
                    scanned += 1
                    if f.is_file() and f.suffix.lower() in exts:
                        batch.append(f)
                        added += 1
                    if len(batch) >= 200:
                        self._scan_q.put(("batch", batch))
                        batch = []
                    if scanned % 2000 == 0:
                        self._scan_q.put(("progress", scanned))
                    if scanned > 50000:
                        self._scan_q.put(("log", "扫描文件数超过 50000，已停止。请缩小文件夹范围。"))
                        break
                if scanned > 50000:
                    break
        except OSError as e:
            self._scan_q.put(("log", f"扫描文件夹出错（已加入 {added} 个）：{e}"))
        if batch:
            self._scan_q.put(("batch", batch))
        self._scan_q.put(("scan_done", added))

    def _poll_scan(self):
        batches: list[Path] = []
        progress = 0
        done = None
        logs: list[str] = []
        try:
            while True:
                kind, payload = self._scan_q.get_nowait()
                if kind == "batch":
                    batches.extend(payload)
                elif kind == "progress":
                    progress = payload
                elif kind == "log":
                    logs.append(payload)
                elif kind == "scan_done":
                    done = payload
        except queue.Empty:
            pass
        for msg in logs:
            self._log(msg, "err")
        if batches:
            new = [f for f in batches if f not in self.files]
            start = len(self.files)
            self.files.extend(new)
            for i, path in enumerate(new, start=start):
                self._insert_tree_row(i, path)
            self._update_file_count()
            if new:
                self._auto_detect_src()
                self._set_default_output()
        if progress:
            self.lbl_file_count.config(text=f"扫描中 {progress} · 匹配 {len(self.files)}")
        if done is not None:
            self.btn_scan_stop.pack_forget()
            self._update_file_count()
            self._log(f"文件夹扫描完成：当前队列共 {len(self.files)} 个文件", "info")
        busy = bool(self._scan_thread and self._scan_thread.is_alive())
        self.after(120 if busy else 400, self._poll_scan)

    def clear_files(self):
        self.files.clear()
        self._row_states.clear()
        self._filter_text = ""
        try:
            self.search_entry.delete(0, "end")
        except Exception:
            pass
        self._refresh_list()
        self._log("已清空文件队列", "muted")

    def remove_selected(self):
        selected = {int(iid) for iid in self.tree.selection()}
        if not selected:
            return
        self.files = [f for i, f in enumerate(self.files) if i not in selected]
        self._refresh_list()
        self._log(f"已移除 {len(selected)} 个文件", "info")

    def _insert_tree_row(self, index: int, path: Path):
        ext = path.suffix.lower()
        kind = EXT_KIND.get(ext, "其他")
        type_text = f"{kind} · {ext.lstrip('.').upper()}" if ext else kind
        try:
            size = _short_size(path.stat().st_size)
        except OSError:
            size = "—"
        state, tag = self._row_states.get(index, ("等待中", ""))
        self.tree.insert(
            "",
            "end",
            iid=str(index),
            values=(path.name, state, type_text, size, str(path.parent)),
            tags=(tag,) if tag else ("",),
        )

    def _set_row_state(self, index: int, text: str, tag: str = ""):
        """更新队列中某一行的状态文字与颜色。"""
        iid = str(index)
        if not self.tree.exists(iid):
            return
        values = list(self.tree.item(iid, "values"))
        if len(values) < 5:
            return
        values[1] = text
        self.tree.item(iid, values=values, tags=(tag,) if tag else ())
        self._row_states[index] = (text, tag)

    def _schedule_filter(self):
        """输入筛选词时防抖，避免每敲一个字符就重建整张表。"""
        if self._filter_job:
            try:
                self.after_cancel(self._filter_job)
            except Exception:
                pass
        self._filter_job = self.after(220, self._apply_filter)

    def _apply_filter(self):
        self._filter_job = None
        try:
            keyword = self.search_entry.get().strip().lower()
        except Exception:
            keyword = ""
        self._filter_text = keyword
        self.tree.delete(*self.tree.get_children())
        shown = 0
        for index, path in enumerate(self.files):
            if keyword and keyword not in path.name.lower():
                continue
            self._insert_tree_row(index, path)
            shown += 1
        if keyword:
            self.lbl_file_count.config(text=f"{shown} / {len(self.files)} 个文件")
        else:
            self.lbl_file_count.config(text=f"{len(self.files)} 个文件")

    def _refresh_list(self):
        self.tree.delete(*self.tree.get_children())
        self._row_states.clear()
        for i, path in enumerate(self.files):
            self._insert_tree_row(i, path)
        self._update_file_count()
        self._auto_detect_src()
        self._set_default_output()

    def _update_file_count(self):
        count = len(self.files)
        if self._filter_text:
            shown = len(self.tree.get_children())
            self.lbl_file_count.config(text=f"{shown} / {count} 个文件")
        else:
            self.lbl_file_count.config(text=f"{count} 个文件")
        self._update_empty_state()
        self._refresh_run_state()

    def _refresh_run_state(self):
        """队列为空时把「开始转换」按下去变灰，别让人点了才发现没文件。"""
        button = getattr(self, "btn_run", None)
        if button is None:
            return
        if self.worker and self.worker.is_alive():
            button.set_enabled(False)
            return
        button.set_enabled(bool(self.files))

    def _update_empty_state(self):
        if self.files:
            self.empty_hint.place_forget()
        else:
            self.empty_hint.place(relx=0.5, rely=0.52, anchor="center")
            self.empty_hint.lift()

    def _set_default_output(self):
        if not self._auto_out or not self.files:
            return
        current = self.ent_out.get().strip()
        if current:
            return
        try:
            out = self.files[0].parent / "格式转换输出"
        except Exception:
            return
        self.ent_out.delete(0, "end")
        self.ent_out.insert(0, str(out))

    def _mark_output_manual(self, _event=None):
        self._auto_out = False

    def open_out_dir(self):
        text = self.ent_out.get().strip()
        if not text:
            messagebox.showinfo("提示", "请先选择输出文件夹")
            return
        folder = Path(text)
        try:
            folder.mkdir(parents=True, exist_ok=True)
            os.startfile(str(folder))
        except OSError as e:
            messagebox.showerror("无法打开输出目录", str(e))

    def _open_selected_location(self, _event=None):
        selected = self.tree.selection()
        if not selected:
            return
        try:
            index = int(selected[0])
        except ValueError:
            return
        if 0 <= index < len(self.files):
            try:
                os.startfile(str(self.files[index].parent))
            except OSError:
                pass

    def pick_out_dir(self):
        d = filedialog.askdirectory(title="选择输出文件夹")
        if d:
            self._auto_out = False
            self.ent_out.delete(0, "end")
            self.ent_out.insert(0, d)

    # ------------------------------------------------------------- 转换执行
    def start_convert(self, only_files: list[Path] | None = None):
        if self.worker and self.worker.is_alive():
            messagebox.showwarning("请稍候", "当前批次还在运行中")
            return
        pool = list(only_files) if only_files is not None else list(self.files)
        if not pool:
            messagebox.showinfo("提示", "请先添加输入文件")
            return
        if not self._src() or not self._dst():
            messagebox.showinfo("提示", "请先在上方选择源格式和目标格式")
            return
        conv = self._current_conv()
        if conv is None:
            messagebox.showinfo("提示", "当前方向暂无可用的转换方式，请换一个目标格式")
            return
        out_text = self.ent_out.get().strip()
        if not out_text:
            messagebox.showinfo("提示", "请选择输出文件夹")
            return
        out = Path(out_text)
        try:
            out.mkdir(parents=True, exist_ok=True)
        except OSError as e:
            messagebox.showerror("输出目录无效", str(e))
            return
        try:
            probe = out / ".~write_test"
            probe.write_bytes(b"")
            probe.unlink(missing_ok=True)
        except OSError as e:
            messagebox.showerror("输出目录不可写", f"无法写入 {out}：\n{e}")
            return

        target = self.cmb_opt.get() if conv.accepts else ""
        files = [
            f for f in pool
            if f.exists() and f.suffix.lower() in conv.extensions
        ]
        skipped = len(pool) - len(files)
        if skipped:
            self._log(f"注意：{skipped} 个文件与所选方向的扩展名不符，已自动跳过", "err")
        if not files:
            messagebox.showinfo(
                "提示",
                f"没有可作为「{conv.src} → {conv.dst}」输入的文件",
            )
            return

        self._failed_files = []
        self._total, self._done = 0, 0
        self.pbar["value"] = 0
        self.lbl_progress_text.config(text=f"0 / {len(files)}")
        self.lbl_status.config(text="正在转换…", fg=PRIMARY)
        self.lbl_hint.config(text=f"{conv.label} · {len(files)} 个任务", fg=MUTED, cursor="")
        self._set_header_status("● 转换中", PRIMARY)
        self.btn_run.set_enabled(False)
        self.btn_cancel.set_enabled(True)
        self.remove_btn.set_enabled(False)
        self.clear_btn.set_enabled(False)
        self._cancel.clear()
        self._batch_files = list(files)
        self._prepare_rows(files)
        try:
            self.log_toggle._fg = PRIMARY_HOVER
            self.log_toggle.set_text("收起日志" if self._log_expanded else "展开日志")
        except Exception:
            pass
        options = self._current_options(conv)
        detail = conv.label
        if "quality" in options:
            detail += f" · 质量 {options['quality']}"
        if "dpi" in options:
            detail += f" · {options['dpi']} DPI"
        if options.get("workers"):
            detail += f" · 并发 {options['workers']}"
        self._log(f"开始转换：{detail}（{len(files)} 个输入）→ {out}", "info")
        if options:
            settings_store.update_ui(quality=self._quality)
        self._remember_direction()
        self._started_at = time.perf_counter()

        self.worker = threading.Thread(
            target=self._worker_run,
            args=(conv, files, out, target, options),
            daemon=True,
        )
        self.worker.start()

    def _prepare_rows(self, chosen: list[Path]):
        """把本批要转换的行标成等待中，其余标成跳过。"""
        selected = set(chosen)
        for index, path in enumerate(self.files):
            if path in selected:
                self._set_row_state(index, "等待中", "running")
            else:
                self._set_row_state(index, "已跳过", "skip")

    def _worker_run(self, conv, files, out, target, options):
        # iid 始终是文件在 self.files 里的下标：重试失败项时本批只是子集，
        # 直接用 batch 下标会和界面行错位。
        position = {path: i for i, path in enumerate(self.files)}
        index_of: dict[str, int] = {}
        for path in files:
            idx = position.get(path)
            if idx is None:
                continue
            try:
                index_of[os.path.normcase(str(path.resolve()))] = idx
            except OSError:
                index_of[os.path.normcase(str(path))] = idx

        def on_file(kind: str, message: str, path):
            idx = None
            if path is not None:
                try:
                    key = os.path.normcase(str(Path(path).resolve()))
                except OSError:
                    key = os.path.normcase(str(path))
                idx = index_of.get(key)
            if kind != "ok" and idx is not None and 0 <= idx < len(self.files):
                self.msg_q.put(("failed_path", self.files[idx]))
            self.msg_q.put(("file_ok" if kind == "ok" else "file_err", (idx, message)))

        elapsed = 0.0
        try:
            self.msg_q.put(("total", len(files)))
            started = time.perf_counter()
            result = conv.run_batch(
                files,
                out,
                target,
                options=options,
                on_file=on_file,
                cancel=self._cancel,
            )
            elapsed = time.perf_counter() - started
            ok, fail = result.ok, result.failed
            cancelled = result.cancelled
            if fail and result.errors:
                self.msg_q.put(("err", f"{fail} 个文件失败，首个原因：{result.errors[0]}"))
            self.msg_q.put(("done", (ok, fail, cancelled, elapsed)))
        except ConvertError as exc:
            self.msg_q.put(("err", str(exc)))
            self.msg_q.put(("done", (0, len(files), False, elapsed)))
        except Exception as exc:  # noqa: BLE001 - 兜底，避免线程静默死掉
            self.msg_q.put(("err", f"转换异常: {exc}"))
            self.msg_q.put(("done", (0, len(files), False, elapsed)))

    def cancel_convert(self):
        """停止后续文件；正在处理的文件会先完成，避免产生半成品。"""
        if not (self.worker and self.worker.is_alive()):
            return
        self._cancel.set()
        self.btn_cancel.set_enabled(False)
        self.lbl_status.config(text="正在取消…", fg=WARNING)
        self._log("已请求取消：正在处理的文件会跑完，其余停止", "warn")

    def _advance_progress(self):
        self._done += 1
        self.pbar["value"] = self._done
        self.lbl_progress_text.config(text=f"{self._done} / {max(1, self._total)}")

    def _finish_rows(self, cancelled: bool):
        """把仍处于「等待中」的行标成取消 / 未完成。"""
        text = "已取消" if cancelled else "未完成"
        for iid in self.tree.get_children():
            values = list(self.tree.item(iid, "values"))
            if len(values) >= 5 and values[1] == "等待中":
                values[1] = text
                self.tree.item(iid, values=values, tags=("skip",))

    def _poll_queue(self):
        processed = 0
        try:
            # 单次最多处理 600 条消息，避免几万条日志把界面卡住。
            while processed < 600:
                kind, text = self.msg_q.get_nowait()
                processed += 1
                if kind == "total":
                    self._total = int(text or 0)
                    self.pbar["maximum"] = max(1, self._total)
                    self.lbl_progress_text.config(text=f"0 / {self._total}")
                elif kind == "tick":
                    self._advance_progress()
                elif kind == "failed_path":
                    if text not in self._failed_files:
                        self._failed_files.append(text)
                elif kind == "file_ok":
                    index, message = text
                    if index is not None:
                        self._set_row_state(index, "✔ 完成", "ok")
                    self.lbl_status.config(text=message[:46], fg=SUCCESS)
                    self._log(message, "ok")
                    self._advance_progress()
                elif kind == "file_err":
                    index, message = text
                    if index is not None:
                        self._set_row_state(index, "✘ 失败", "err")
                    self.lbl_status.config(text="有文件失败，见日志", fg=ERROR)
                    self._log(message, "err")
                    self._advance_progress()
                    self._auto_expand_log()
                elif kind == "ok":
                    self.lbl_status.config(text=text[:46], fg=SUCCESS)
                    self._log(text, "ok")
                elif kind == "err":
                    self.lbl_status.config(text="有文件失败，见日志", fg=ERROR)
                    self._log(text, "err")
                    self._auto_expand_log()
                elif kind == "done":
                    self._on_batch_done(text)
        except queue.Empty:
            pass
        # 还有积压时快速继续；空闲时降低轮询频率，避免后台常驻占用 CPU。
        backlog = not self.msg_q.empty()
        busy = bool(self.worker and self.worker.is_alive())
        self.after(30 if backlog else (80 if busy else 300), self._poll_queue)

    def _on_batch_done(self, payload):
        if isinstance(payload, tuple) and len(payload) == 4:
            ok, fail, cancelled, elapsed = payload
        else:  # 兼容旧格式（纯文本）
            ok, fail, cancelled, elapsed = 0, 0, False, 0.0
            self.lbl_status.config(text=str(payload))
        if not cancelled:
            self._done = self._total
        self.pbar["value"] = self._done
        self.lbl_progress_text.config(text=f"{self._done} / {max(1, self._total)}")
        self._finish_rows(cancelled)

        if cancelled:
            summary = f"已取消：成功 {ok} 项，失败 {fail} 项"
            color = WARNING
        elif fail == 0:
            summary = f"完成：成功 {ok} 项，失败 0 项"
            color = SUCCESS
        else:
            summary = f"完成：成功 {ok} 项，失败 {fail} 项"
            color = WARNING
        if elapsed:
            summary += f" · 用时 {elapsed:.1f} 秒"
        self.lbl_status.config(text=summary, fg=color)
        if fail > 0 and self._failed_files and not cancelled:
            self.lbl_hint.config(
                text=f"{len(self._failed_files)} 个文件失败 · 点这里重试这些文件",
                fg=PRIMARY_HOVER,
                cursor="hand2",
            )
            self.lbl_hint.bind("<Button-1>", lambda _e: self._retry_failed())
        else:
            self.lbl_hint.config(text="任务结束，可继续添加下一批文件", fg=MUTED, cursor="")
        self._log("—— " + summary + " ——", "info")
        self._set_header_status("● 就绪" if not cancelled else "● 已取消",
                                SUCCESS if not cancelled else WARNING)
        self._refresh_run_state()
        self.btn_cancel.set_enabled(False)
        self.remove_btn.set_enabled(True)
        self.clear_btn.set_enabled(True)
        self._beep(cancelled or fail > 0)

    def _beep(self, has_problem: bool):
        """任务结束时给一个很轻的提示音（可在设置里关掉）。"""
        if not self._prefs.get("beep", True):
            return
        try:
            import winsound

            winsound.MessageBeep(
                winsound.MB_ICONHAND if has_problem else winsound.MB_ICONASTERISK
            )
        except Exception:
            pass

    # ---------------------------------------------------------------- 杂项
    def _set_header_status(self, text: str, color: str):
        self.header_status.config(text=text, fg=color)

    def _sync_quick_tiles(self):
        src = self._src()
        groups = getattr(self, '_quick_groups', [])
        for tile, group in zip(self._quick_tiles, groups):
            tile.set_active(src in group)

    def _auto_expand_log(self):
        """出错时不再抢布局，只把日志按钮标红，提示去点开。"""
        try:
            self.log_toggle.set_text("查看失败")
            self.log_toggle._fg = ERROR
            self.log_toggle._draw()
        except Exception:
            pass

    def _retry_failed(self):
        """只重跑上一批失败的文件。"""
        files = [f for f in self._failed_files if f.exists()]
        if not files:
            self.lbl_hint.config(text="失败的文件已不在原位置，无法重试", fg=MUTED, cursor="")
            return
        self._log(f"重试 {len(files)} 个失败文件", "info")
        self.start_convert(only_files=files)

    def _init_sash(self):
        try:
            total = self.split.winfo_height()
            if total > px(420):
                self.split.sash_place(0, 0, max(px(300), total - px(44)))
                self._log_expanded = False
        except Exception:
            pass

    def _toggle_log(self, expand: bool | None = None):
        """日志区默认收起，需要时展开。"""
        try:
            total = self.split.winfo_height()
        except Exception:
            return
        if expand is None:
            expand = not getattr(self, "_log_expanded", False)
        self.log_toggle._fg = PRIMARY_HOVER
        if expand:
            # 按比例展开，别把文件队列整个挤掉：日志最多占 45%。
            self.split.sash_place(0, 0, max(px(260), int(total * 0.55)))
            self.log_toggle.set_text("收起日志")
        else:
            self.split.sash_place(0, 0, max(px(260), total - px(44)))
            self.log_toggle.set_text("展开日志")
        self._log_expanded = expand
        if expand:
            try:
                self.txt_log.see("end")
            except Exception:
                pass

    def _on_close(self):
        if self.worker and self.worker.is_alive():
            if not messagebox.askyesno(
                "仍有任务在运行",
                "转换还没有结束。现在退出会中断当前文件，确定要退出吗？",
            ):
                return
            self._cancel.set()
            self.worker.join(timeout=2.0)
        try:
            settings_store.update_ui(geometry=self.geometry())
            self._remember_direction()
        except Exception:
            pass
        self.destroy()

    def _restore_prefs(self):
        """恢复上次的方向、质量与窗口大小。"""
        prefs = self._prefs or {}
        geo = prefs.get("geometry")
        if isinstance(geo, str) and geo:
            try:
                self.geometry(self._clamp_geometry(geo))
            except tk.TclError:
                pass
        src = prefs.get("last_src")
        if src in self.cmb_src["values"]:
            self.cmb_src.set(src)
            self._on_src_change()
            dst = prefs.get("last_dst")
            if dst in self.cmb_dst["values"]:
                self.cmb_dst.set(dst)
                self._on_dst_change()
                mode = prefs.get("last_mode")
                if mode in list(self.cmb_mode["values"]):
                    self.cmb_mode.set(mode)
                    self._on_mode_change()

    def _remember_direction(self):
        settings_store.update_ui(
            last_src=self._src(),
            last_dst=self._dst(),
            last_mode=self.cmb_mode.get() if self.cmb_mode["values"] else "",
        )

    # ---------------------------------------------------------------- 设置
    def _show_settings(self):
        win = tk.Toplevel(self)
        win.title("设置 · 格式转换工具")
        win.configure(bg=BG)
        win.geometry(f"{px(700)}x{px(600)}")
        win.resizable(False, False)
        win.transient(self)
        try:
            win.iconphoto(True, self._window_icon)
        except Exception:
            pass

        wrap = tk.Frame(win, bg=BG, padx=22, pady=18)
        wrap.pack(fill="both", expand=True)
        tk.Label(wrap, text="设置", font=(UI_FONT, 16, "bold"), bg=BG, fg=TEXT).pack(anchor="w")
        tk.Label(
            wrap,
            text="修改后立即写入程序旁边的 settings.json，下一次转换生效。",
            font=FONT_SMALL, bg=BG, fg=MUTED,
        ).pack(anchor="w", pady=(4, 12))

        # 1) 引擎
        card1 = tk.Frame(wrap, bg=SURFACE, padx=14, pady=12)
        card1.pack(fill="x", pady=(0, 10))
        tk.Label(card1, text="转换引擎", font=FONT_SECTION, bg=SURFACE, fg=TEXT).pack(anchor="w")
        current = settings_store.load().get("engine", "auto")
        var_engine = tk.StringVar(value=current if current in ("auto", "cpp", "python") else "auto")
        for value, label, hint in (
            ("auto", "自动（推荐）", "有 C++ 核心就用核心，没有就回退 Python"),
            ("cpp", "只用 C++ 核心", "找不到核心时直接报错，便于排查"),
            ("python", "只用 Python 回退", "兼容模式，速度较慢、部分格式不可用"),
        ):
            row = tk.Frame(card1, bg=SURFACE)
            row.pack(fill="x", pady=(6, 0))
            tk.Radiobutton(
                row, text=label, variable=var_engine, value=value,
                bg=SURFACE, fg=TEXT_SOFT, selectcolor=SURFACE_ALT,
                activebackground=SURFACE, activeforeground=TEXT,
                font=(UI_FONT, 10), anchor="w", relief="flat",
                highlightthickness=0, bd=0,
            ).pack(side="left")
            tk.Label(row, text=hint, font=FONT_TINY, bg=SURFACE, fg=MUTED).pack(
                side="left", padx=(10, 0)
            )
        cli = self._engine.get("cli") or "未找到 fccli.exe"
        core = self._engine.get("core_version") or ""
        tk.Label(
            card1,
            text=f"核心：{cli}" + (f"  ·  {core}" if core else ""),
            font=FONT_TINY, bg=SURFACE, fg=MUTED, justify="left", wraplength=px(580),
        ).pack(anchor="w", pady=(8, 0))

        # 2) 外部程序
        card2 = tk.Frame(wrap, bg=SURFACE, padx=14, pady=12)
        card2.pack(fill="x", pady=(0, 10))
        tk.Label(card2, text="外部程序", font=FONT_SECTION, bg=SURFACE, fg=TEXT).pack(anchor="w")
        tk.Label(
            card2, text="留空则自动使用程序旁边的 tools/；换机或自定义安装时再填绝对路径。",
            font=FONT_TINY, bg=SURFACE, fg=MUTED,
        ).pack(anchor="w", pady=(2, 8))

        data = settings_store.load()
        ent_ffmpeg = self._path_row(card2, "FFmpeg", data.get("ffmpeg", ""), "ffmpeg.exe")
        ent_soffice = self._path_row(card2, "LibreOffice", data.get("soffice", ""), "soffice.exe")

        # 3) 其他
        card3 = tk.Frame(wrap, bg=SURFACE, padx=14, pady=12)
        card3.pack(fill="x")
        tk.Label(card3, text="其他", font=FONT_SECTION, bg=SURFACE, fg=TEXT).pack(anchor="w")
        var_beep = tk.BooleanVar(value=bool(self._prefs.get("beep", True)))
        tk.Checkbutton(
            card3, text="任务结束时播放提示音", variable=var_beep,
            bg=SURFACE, fg=TEXT_SOFT, selectcolor=SURFACE_ALT,
            activebackground=SURFACE, activeforeground=TEXT,
            font=(UI_FONT, 10), anchor="w", relief="flat", highlightthickness=0, bd=0,
        ).pack(anchor="w", pady=(6, 0))

        word_row = tk.Frame(card3, bg=SURFACE)
        word_row.pack(fill="x", pady=(8, 0))
        tk.Label(word_row, text="Word 转文本", font=FONT_TINY, bg=SURFACE, fg=MUTED,
                 width=11, anchor="w").pack(side="left")
        word_modes = ["内置提取（快，推荐）", "LibreOffice（高保真）"]
        cmb_word = tb.Combobox(word_row, state="readonly", font=FONT_SMALL, width=18,
                               values=word_modes)
        saved_word = data.get("word_text_engine", "builtin")
        cmb_word.set(word_modes[1] if saved_word == "libreoffice" else word_modes[0])
        cmb_word.pack(side="left", padx=(0, 8))
        tk.Label(
            word_row,
            text=".docx 约 0.1 秒；.doc/.rtf/.odt 才需要 LibreOffice",
            font=FONT_TINY, bg=SURFACE, fg=MUTED,
        ).pack(side="left")
        worker_row = tk.Frame(card3, bg=SURFACE)
        worker_row.pack(fill="x", pady=(8, 0))
        tk.Label(worker_row, text="同时处理", font=FONT_TINY, bg=SURFACE, fg=MUTED,
                 width=11, anchor="w").pack(side="left")
        worker_modes = ["自动（推荐）", "2 个", "4 个", "6 个", "8 个", "12 个", "16 个"]
        worker_values = [0, 2, 4, 6, 8, 12, 16]
        cmb_workers = tb.Combobox(worker_row, state="readonly", font=FONT_SMALL, width=12,
                                  values=worker_modes)
        try:
            cmb_workers.current(worker_values.index(self._workers))
        except ValueError:
            cmb_workers.current(0)
        cmb_workers.pack(side="left", padx=(0, 8))
        tk.Label(
            worker_row,
            text="默认按 CPU 核数自动决定",
            font=FONT_TINY, bg=SURFACE, fg=MUTED,
        ).pack(side="left")

        tk.Label(
            card3,
            text="提示：转换过程中可以随时点「取消」，正在处理的文件会跑完再停，不会留下半成品。",
            font=FONT_TINY, bg=SURFACE, fg=MUTED, justify="left", wraplength=px(580),
        ).pack(anchor="w", pady=(6, 0))

        btn_row = tk.Frame(wrap, bg=BG)
        btn_row.pack(fill="x", pady=(14, 0))

        def open_settings_file():
            try:
                os.startfile(str(settings_store.settings_path()))
            except OSError as exc:
                messagebox.showerror("无法打开", str(exc))

        PillButton(
            btn_row, text="打开配置文件", command=open_settings_file, width=120, height=36,
            fill=SURFACE_ALT, hover=SURFACE_HOVER, fg=TEXT_SOFT, outer_bg=BG,
        ).pack(side="left")

        def save():
            payload = settings_store.load()
            payload["engine"] = var_engine.get()
            payload["ffmpeg"] = ent_ffmpeg.get().strip()
            payload["soffice"] = ent_soffice.get().strip()
            payload["word_text_engine"] = (
                "libreoffice" if cmb_word.get() == word_modes[1] else "builtin"
            )
            if not settings_store.save(payload):
                messagebox.showerror("保存失败", "无法写入 settings.json，请检查程序目录权限。")
                return
            settings_store.update_ui(beep=bool(var_beep.get()))
            worker_values = [0, 2, 4, 6, 8, 12, 16]
            index = cmb_workers.current()
            selected = worker_values[index] if 0 <= index < len(worker_values) else 0
            settings_store.update_ui(workers=selected)
            self._workers = selected
            self._prefs = settings_store.ui_prefs()
            self._engine = engine_info()
            self.engine_btn.set_text(self._engine_badge_text())
            self.engine_btn._fg = PRIMARY if self._engine.get("available") else WARNING
            self.engine_btn._draw()
            self._log(
                f"设置已保存：引擎={payload['engine']}，"
                f"核心={'可用' if self._engine.get('available') else '不可用'}",
                "info",
            )
            win.destroy()

        PillButton(
            btn_row, text="取消", command=win.destroy, width=86, height=36,
            fill=SURFACE_ALT, hover=SURFACE_HOVER, fg=TEXT_SOFT, outer_bg=BG,
        ).pack(side="right")
        PillButton(
            btn_row, text="保存", command=save, width=96, height=36,
            fill=PRIMARY, hover=PRIMARY_HOVER, fg="#FFFFFF", outer_bg=BG,
        ).pack(side="right", padx=(0, 8))
        win.bind("<Escape>", lambda _e: win.destroy())

    def _path_row(self, parent, label: str, value: str, exe_name: str):
        row = tk.Frame(parent, bg=SURFACE)
        row.pack(fill="x", pady=(2, 0))
        tk.Label(row, text=label, font=FONT_TINY, bg=SURFACE, fg=MUTED, width=11,
                 anchor="w").pack(side="left")
        entry = tb.Entry(row, font=FONT_SMALL)
        entry.pack(side="left", fill="x", expand=True, padx=(0, 6))
        if value:
            entry.insert(0, str(value))

        def browse():
            chosen = filedialog.askopenfilename(
                title=f"选择 {exe_name}",
                filetypes=[(exe_name, exe_name), ("所有文件", "*.*")],
            )
            if chosen:
                entry.delete(0, "end")
                entry.insert(0, chosen)

        PillButton(
            row, text="浏览", command=browse, width=64, height=30,
            fill=SURFACE_ALT, hover=SURFACE_HOVER, fg=TEXT_SOFT, outer_bg=SURFACE,
            font=FONT_TINY, radius=8,
        ).pack(side="right")
        return entry

    def _show_help(self):
        win = tk.Toplevel(self)
        win.title("使用说明 · 格式转换工具")
        win.configure(bg=BG)
        win.geometry(f"{px(800)}x{px(720)}")
        win.minsize(px(640), px(480))
        win.transient(self)
        try:
            win.iconphoto(True, self._window_icon)
        except Exception:
            pass

        wrap = tk.Frame(win, bg=BG, padx=22, pady=18)
        wrap.pack(fill="both", expand=True)
        tk.Label(
            wrap, text="使用说明", font=(UI_FONT, 16, "bold"), bg=BG, fg=TEXT
        ).pack(anchor="w")
        tk.Label(
            wrap,
            text="三步完成一次批量转换；下面的说明对应界面上的每一块功能。",
            font=FONT_SMALL, bg=BG, fg=MUTED,
        ).pack(anchor="w", pady=(4, 12))

        holder = tk.Frame(wrap, bg=SURFACE, highlightthickness=1,
                          highlightbackground=BORDER)
        holder.pack(fill="both", expand=True)
        txt = tk.Text(
            holder, wrap="word", relief="flat", bg=SURFACE, fg=TEXT_SOFT,
            font=(UI_FONT, 10), padx=18, pady=14, insertbackground=TEXT,
            bd=0, highlightthickness=0, spacing1=2, spacing3=5,
        )
        sb = ttk.Scrollbar(holder, orient="vertical", command=txt.yview)
        txt.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        txt.pack(side="left", fill="both", expand=True)
        txt.tag_configure("h", font=(UI_FONT, 11, "bold"), foreground=PRIMARY_HOVER,
                          spacing1=14, spacing3=6)
        txt.tag_configure("k", font=(UI_FONT, 10, "bold"), foreground=TEXT)
        txt.tag_configure("n", font=(UI_FONT, 10), foreground=TEXT_SOFT)
        txt.tag_configure("m", font=FONT_TINY, foreground=MUTED)

        def h(text):
            txt.insert("end", text + "\n", "h")

        def item(k, v):
            txt.insert("end", "•  " + k, "k")
            txt.insert("end", v + "\n", "n")

        h("1. 三步开始")
        item("加入文件：", "点击上方蓝色的「拖入文件，立即转换」区域选择文件，或把文件 / 文件夹直接拖进去；右侧「添加文件 / 添加文件夹」同样可用。分类卡片只切换源格式，不会弹出选文件窗口。")
        item("确定方向：", "源格式选文件现在是什么，目标格式选想得到什么；两个都选好后，下方才会展开可用的方式、目标格式与说明。")
        item("开始转换：", "确认输出位置后点「开始转换」，进度和每一条结果都会写入下方运行日志。")

        h("2. 六个分类分别能做什么")
        item("图片：", "HEIC / AVIF / JPG / PNG / WebP / BMP / TIFF / GIF / ICO 互转；多图合成 GIF；多图分别或合并导出 PDF。")
        item("PDF：", "转图片 / Word / Excel / PPT / 纯文本；多文件合并、逐页拆分、页面旋转、内容流压缩。")
        item("Office：", "Word / Excel / PPT 与 PDF、图片、纯文本互转；也支持 doc / xls / ppt / odt / rtf / csv 等新旧格式互转。")
        item("音视频：", "MP4 / MKV / MOV / WebM / AVI / WMV / GIF 互转；MP3 / WAV / FLAC / M4A / AAC / OGG / OPUS / WMA；视频可直接提取音轨。")
        item("数据：", "JSON / YAML / XML 互转并展开为 Excel；Excel / CSV 也能反向转 JSON。")
        item("文本：", "TXT / Markdown 直接排版为 PDF 或 Word 文档。")

        h("3. 输出与文件规则")
        item("默认输出：", "第一个文件旁边的「格式转换输出」文件夹，可以随时改成其它目录；点「打开目录」可直接在资源管理器里查看结果。")
        item("不覆盖：", "输出文件同名时自动加 _1、_2 后缀，原文件不会被修改。")
        item("自动跳过：", "与当前方向扩展名不符的文件会跳过，并在日志里提示数量。")
        item("大文件夹：", "扫描时每 200 个一批加入队列；超过 50000 个文件会自动停止，避免卡顿。")

        h("4. 运行日志怎么用")
        item("实时进度：", "每条日志带时间戳，成功为绿色、失败为红色，数量统计显示在标题右侧；双击队列中的文件可打开它所在的文件夹。")
        item("调大窗口：", "拖动主界面与日志之间的横向分割条即可拉高日志区，默认比旧版本高很多。")
        item("保留现场：", "可关闭「自动滚动」向上翻看历史；点「复制全部」可把完整日志发给其他人排查。")

        h("5. 队列状态与取消")
        item("状态列：", "每个文件一行状态：等待中 → 完成（绿色，显示输出名）/ 失败（红色，显示原因）/ 已取消。转换中可以继续拖入新文件。")
        item("取消：", "点「取消」后，正在处理的那几个文件会跑完，其余立即停止；不会留下半成品，已完成的输出保留。")
        item("质量 / DPI：", "图片互转时出现「输出质量」滑杆（10–100）；PDF / Office 转图片时出现「渲染 DPI」，200 适合屏幕查看，300 适合打印。")
        item("记忆：", "上次选择的格式方向、质量、DPI 和窗口大小会自动记住，下次打开直接接着用。")

        h("6. 设置里的三件事")
        item("转换引擎：", "右上角徽标显示当前用的是 C++ 核心还是 Python 回退，点它打开设置。默认「自动」，有核心就用核心。")
        item("外部程序：", "FFmpeg 与 LibreOffice 默认用程序旁边的 tools/；换机、便携盘或自定义安装时可手动指定 exe 路径。")
        item("提示音：", "任务结束的轻提示音可以在这里关掉。")

        h("7. 常见问题")
        item("扫描件 PDF：", "没有文字层，转 Word / Excel / 文本会得到空内容或图片版，这是源文件限制。")
        item("Office 转换慢：", "依赖包内 LibreOffice，首次启动会多等几秒，后续批量会明显加快。")
        item("PDF 转 PPT：", "导出为整页图片版，文字不可编辑；需要可编辑文字请先转 Word。")
        item("转换失败：", "日志会保留完整错误原因，优先看红色行末尾的异常信息。")

        h("8. 快捷键")
        item("Ctrl+O：", "添加文件。")
        item("Ctrl+L：", "清空运行日志。")
        txt.insert("end", "\n提示：鼠标悬停在分类卡片和按钮上，会显示对应功能的简短说明。", "m")
        txt.configure(state="disabled")

        btn_row = tk.Frame(wrap, bg=BG)
        btn_row.pack(fill="x", pady=(12, 0))
        PillButton(
            btn_row, text="知道了", command=win.destroy, width=110, height=36,
            fill=PRIMARY, hover=PRIMARY_HOVER, fg="#FFFFFF", outer_bg=BG,
        ).pack(side="right")
        win.bind("<Escape>", lambda _e: win.destroy())
        win.focus_set()

    def _clear_log(self):
        self.txt_log.configure(state="normal")
        self.txt_log.delete("1.0", "end")
        self.txt_log.configure(state="disabled")
        self._log_ok = 0
        self._log_err = 0
        self._update_log_stats()
        self.log_empty.place(relx=0.5, rely=0.5, anchor="center")
        self.log_empty.lift()
        self.lbl_log_hint.config(
            text="日志已清空。继续转换时，新的进度与错误会重新记录在这里。"
        )

    def _copy_log(self):
        content = self.txt_log.get("1.0", "end-1c").strip()
        if not content:
            self.lbl_log_hint.config(text="目前没有可复制的日志内容。")
            return
        self.clipboard_clear()
        self.clipboard_append(content)
        self.lbl_log_hint.config(text=f"已复制 {len(content.splitlines())} 行日志到剪贴板。")

    def _toggle_autoscroll(self):
        self._autoscroll = not self._autoscroll
        self.scroll_btn.set_text("自动滚动：开" if self._autoscroll else "自动滚动：关")
        self.scroll_btn._fill = SURFACE_ALT if self._autoscroll else "#26324A"
        self.scroll_btn._draw()
        if self._autoscroll:
            self.txt_log.see("end")

    def _update_log_stats(self):
        self.lbl_log_ok.config(text=f"成功 {self._log_ok}")
        self.lbl_log_err.config(
            text=f"失败 {self._log_err}",
            fg=ERROR if self._log_err else MUTED,
        )

    def _log(self, msg: str, tag: str = "info"):
        t = self.txt_log
        try:
            at_bottom = t.yview()[1] >= 0.98
        except Exception:
            at_bottom = True
        if tag == "ok":
            self._log_ok += 1
        elif tag == "err":
            self._log_err += 1
        self._update_log_stats()
        try:
            self.log_empty.place_forget()
        except Exception:
            pass
        prefix = {"ok": "✔  ", "err": "✘  ", "warn": "⚠  ",
                  "info": "·  ", "muted": "·  "}.get(tag, "·  ")
        if tag not in ("ok", "err", "warn", "info", "muted"):
            tag = "info"
        t.configure(state="normal")
        t.insert("end", time.strftime("%H:%M:%S") + "    ", "time")
        t.insert("end", prefix + msg + "\n", tag)
        lines = int(t.index("end-1c").split(".")[0])
        if lines > 1500:
            t.delete("1.0", f"{lines - 1200}.0")
        if self._autoscroll and at_bottom:
            t.see("end")
        t.configure(state="disabled")


def main():
    app = App()
    app.mainloop()


if __name__ == "__main__":
    main()







