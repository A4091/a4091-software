#!/usr/bin/env python3
"""
Simple editor for A4091/A4092 drawing headers.

Features
- Load/Save C header arrays of `struct drawing { BYTE type; BYTE pen; SHORT x,y,w,h; }`.
- Tools: Select/Move/Resize, Add Filled Rect (type=1), Add Outline Rect (type=2), Add Zorro (type=3).
- Delete selected shape.
- Overlay: Load PNG/JPEG and render with 30% opacity; overlay can be moved/resized and selected.

Notes
- Requires Python 3 with Tkinter. For JPEG or robust PNG support, Pillow is recommended.
- Colors follow `card2svg.c` mapping: pen 0=#b2b2b2, 1=#000000, 2=#ffffff, 3=#0055aa.
"""

import os
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

try:
    import tkinter as tk
    from tkinter import filedialog, simpledialog, messagebox
except ModuleNotFoundError:
    # Provide actionable guidance and exit cleanly.
    msg = (
        "Tkinter is not available in your Python build.\n\n"
        "On macOS, run brew install python-tk tcl-tk\n"
        "Pillow is recommended for JPEG/alpha overlay: pip install Pillow\n"
    )
    print(msg)
    sys.exit(1)
except Exception as e:  # pragma: no cover
    print("Tkinter is required to run this editor.")
    print(e)
    sys.exit(1)

try:
    from PIL import Image, ImageTk
    PIL_AVAILABLE = True
except Exception:
    PIL_AVAILABLE = False
    # Tk's PhotoImage may still load some PNGs, but not JPEGs.


# Color mapping matching card2svg.c
PEN_COLORS = {
    0: "#b2b2b2",  # background/cutout
    1: "#000000",  # black chips
    2: "#ffffff",  # silver/white
    3: "#6688bb",  # board color
    #3: "#0055aa",  # board color
}

@dataclass
class Shape:
    type: int  # 1 filled, 2 outline, 3 zorro
    pen: int
    x: int
    y: int
    w: int
    h: int
    comment: Optional[str] = None
    # Internal drawing bookkeeping
    canvas_ids: List[int] = field(default_factory=list, repr=False)

    def bounds(self) -> Tuple[int, int, int, int]:
        x1, y1 = self.x, self.y
        x2, y2 = self.x + self.w, self.y + self.h
        # Normalize if width/height are negative
        if x2 < x1:
            x1, x2 = x2, x1
        if y2 < y1:
            y1, y2 = y2, y1
        # Match bootmenu rendering: zero width/height still shows as 1px
        if x2 == x1:
            x2 = x1 + 1
        if y2 == y1:
            y2 = y1 + 1
        return x1, y1, x2, y2

    def normalize(self) -> None:
        # Ensure w,h are positive, adjusting x,y.
        if self.w < 0:
            self.x += self.w
            self.w = -self.w
        if self.h < 0:
            self.y += self.h
            self.h = -self.h


@dataclass
class Overlay:
    image_path: str
    x: int = 0
    y: int = 0
    w: Optional[int] = None  # None -> image natural width
    h: Optional[int] = None
    tk_image: Optional[object] = field(default=None, repr=False)
    canvas_id: Optional[int] = field(default=None, repr=False)

    def bounds(self, native_size: Tuple[int, int]) -> Tuple[int, int, int, int]:
        ow, oh = native_size
        w = self.w if self.w is not None else ow
        h = self.h if self.h is not None else oh
        x1, y1 = self.x, self.y
        x2, y2 = x1 + w, y1 + h
        if x2 < x1:
            x1, x2 = x2, x1
        if y2 < y1:
            y1, y2 = y2, y1
        return x1, y1, x2, y2


class EditorApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("A409x Drawing Editor")

        # State
        self.shapes: List[Shape] = []
        self.array_name: str = "card_custom"
        self.file_path: Optional[str] = None
        self.selected_index: Optional[int] = None
        self.tool: str = "select"  # select|filled|outline|zorro
        self.tool_var = tk.StringVar(value="select")
        self.drag_start: Optional[Tuple[int, int]] = None
        self.drag_mode: Optional[str] = None  # move|resize|create|overlay-move|overlay-resize
        self.resize_handle: Optional[str] = None  # tl,tr,bl,br
        self.current_preview_id: Optional[int] = None
        self.pen_var = tk.IntVar(value=3)
        self.overlay: Optional[Overlay] = None
        self.overlay_native_size: Optional[Tuple[int, int]] = None
        self.state = PersistedState.load()
        self.shift_down: bool = False
        self.resize_initial: Optional[Tuple[int, int, int, int]] = None  # x1,y1,x2,y2 at resize start
        self.undo_stack: List[dict] = []
        self.redo_stack: List[dict] = []
        self.video_mode = tk.StringVar(value="NTSC")  # NTSC: 640x400, PAL: 640x512
        self.dirty: bool = False
        self.modal_active: bool = False

        self._build_ui()
        self._redraw()

    # Defaults
    ZORRO_DEFAULT_W = 163
    ZORRO_DEFAULT_H = 10
    OFFSET_X = 103
    OFFSET_Y = 50

    # UI setup
    def _build_ui(self):
        # Menubar
        menubar = tk.Menu(self.root)
        filem = tk.Menu(menubar, tearoff=0)
        filem.add_command(label="Load…", accelerator="L / Cmd/Ctrl+O", command=self.cmd_load)
        filem.add_command(label="Save…", accelerator="S / Cmd/Ctrl+S", command=self.cmd_save)
        filem.add_separator()
        filem.add_command(label="About", command=lambda:
			messagebox.showinfo("About", "A409x Drawing Editor\n\nCopyright © 2025 Stefan Reinauer\n\nEditor for A409x style artwork, exports C arrays that can directly be used by the driver.\n"))
        menubar.add_cascade(label="File", menu=filem)

        editm = tk.Menu(menubar, tearoff=0)
        try:
            import sys as _sys
            _is_macos = (_sys.platform == 'darwin')
        except Exception:
            _is_macos = False
        undo_accel = 'Cmd+Z' if _is_macos else 'Ctrl+Z'
        redo_accel = 'Shift+Cmd+Z' if _is_macos else 'Ctrl+Y'
        editm.add_command(label='Undo', accelerator=undo_accel, command=self.cmd_undo)
        editm.add_command(label='Redo', accelerator=redo_accel, command=self.cmd_redo)
        editm.add_separator()
        editm.add_command(label="Delete", accelerator="Delete", command=self.cmd_delete)
        editm.add_command(label="Properties…", accelerator="E / Cmd/Ctrl+I", command=self.cmd_properties)
        menubar.add_cascade(label="Edit", menu=editm)

        viewm = tk.Menu(menubar, tearoff=0)
        for label in ("50%","75%","100%","125%","150%","200%","300%"):
            viewm.add_command(label=f"Zoom {label}", command=lambda l=label: self._set_zoom_percent(l))
        viewm.add_separator()
        viewm.add_command(label="Zoom In", accelerator="Cmd/Ctrl+Plus", command=self._zoom_in)
        viewm.add_command(label="Zoom Out", accelerator="Cmd/Ctrl+Minus", command=self._zoom_out)
        viewm.add_command(label="Zoom 100%", accelerator="Cmd/Ctrl+0", command=lambda: self._set_zoom_percent("100%"))
        viewm.add_separator()
        viewm.add_command(label="Video NTSC", command=lambda: self._set_video_mode("NTSC"))
        viewm.add_command(label="Video PAL", command=lambda: self._set_video_mode("PAL"))
        menubar.add_cascade(label="View", menu=viewm)

        overlaym = tk.Menu(menubar, tearoff=0)
        overlaym.add_command(label="Load Overlay…", accelerator="O", command=self.cmd_overlay_load)
        overlaym.add_command(label="Clear Overlay", accelerator="c", command=self.cmd_overlay_clear)
        menubar.add_cascade(label="Overlay", menu=overlaym)

        helpm = tk.Menu(menubar, tearoff=0)
        helpm.add_command(label="About", command=lambda: messagebox.showinfo("About", "A409x Drawing Editor\n\nCopyright © 2025 Stefan Reinauer\n\nEditor for A409x style artwork, exports C arrays that can directly be used by the driver.\n"))
        helpm.add_command(label="Quit", accelerator="Cmd/Ctrl+Q", command=self.on_quit)
        menubar.add_cascade(label="Help", menu=helpm)
        self.root.config(menu=menubar)
        # Remove macOS-injected Edit menu items like Emoji & Dictation for a cleaner UI
        try:
            self._strip_apple_menu_items(menubar)
            self.root.after(200, lambda: self._strip_apple_menu_items(menubar))
        except Exception:
            pass

        # Top toolbar (keep Zoom and Video only)
        toolbar = tk.Frame(self.root)
        toolbar.pack(side=tk.TOP, fill=tk.X)

        tk.Label(toolbar, text=" Video:").pack(side=tk.LEFT, padx=(10, 0))
        self.video_mode = tk.StringVar(value="NTSC")
        vm = tk.OptionMenu(toolbar, self.video_mode, "NTSC", "PAL", command=lambda *_: self._redraw())
        vm.pack(side=tk.LEFT)

        tk.Label(toolbar, text=" Zoom:").pack(side=tk.LEFT, padx=(10, 0))
        self.zoom = tk.DoubleVar(value=1.0)
        self.zoom_percent = tk.StringVar(value="100%")
        zoom_menu = tk.OptionMenu(
            toolbar,
            self.zoom_percent,
            "50%", "75%", "100%", "125%", "150%", "200%", "300%",
            command=lambda v: self._set_zoom_percent(v),
        )
        zoom_menu.pack(side=tk.LEFT)

        # Paned window: main canvas (left) + right sidebar
        paned = tk.PanedWindow(self.root, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True)

        left = tk.Frame(paned)
        paned.add(left, stretch="always")

        # Canvas with scrollbars in left pane
        work = tk.Frame(left)
        work.pack(fill=tk.BOTH, expand=True)
        self.hbar = tk.Scrollbar(work, orient=tk.HORIZONTAL)
        self.hbar.pack(side=tk.BOTTOM, fill=tk.X)
        self.vbar = tk.Scrollbar(work, orient=tk.VERTICAL)
        self.vbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.canvas = tk.Canvas(work, width=640, height=512, bg="#ffffff",
                                xscrollcommand=self.hbar.set, yscrollcommand=self.vbar.set)
        self.canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.hbar.config(command=self.canvas.xview)
        self.vbar.config(command=self.canvas.yview)

        # Quit handling
        self.root.protocol("WM_DELETE_WINDOW", self.on_quit)

        # Right sidebar
        right = tk.Frame(paned, width=200)
        paned.add(right)
        # Tools
        tk.Label(right, text="Tools").pack(anchor="w", padx=8, pady=(8, 2))
        tools = tk.Frame(right)
        tools.pack(anchor="w", padx=8)
        tk.Radiobutton(tools, text="Select (1)", value="select", variable=self.tool_var, indicatoron=False,
                      command=lambda: self.set_tool(self.tool_var.get())).pack(fill=tk.X)
        tk.Radiobutton(tools, text="Filled (2)", value="filled", variable=self.tool_var, indicatoron=False,
                      command=lambda: self.set_tool(self.tool_var.get())).pack(fill=tk.X)
        tk.Radiobutton(tools, text="Outline (3)", value="outline", variable=self.tool_var, indicatoron=False,
                      command=lambda: self.set_tool(self.tool_var.get())).pack(fill=tk.X)
        tk.Radiobutton(tools, text="Zorro (4)", value="zorro", variable=self.tool_var, indicatoron=False,
                      command=lambda: self.set_tool(self.tool_var.get())).pack(fill=tk.X)

        # Pen selector
        tk.Label(right, text="Pen").pack(anchor="w", padx=8, pady=(8, 2))
        pen_frame = tk.Frame(right)
        pen_frame.pack(anchor="w", padx=8)
        self.pen_menu_btn = tk.OptionMenu(pen_frame, self.pen_var, 0, 1, 2, 3)
        self.pen_menu_btn.pack(anchor="w")
        self._pen_imgs = {}
        self._init_pen_menu()
        try:
            self.pen_var.trace_add('write', lambda *a: self._on_pen_change())
        except Exception:
            self.pen_var.trace('w', lambda *a: self._on_pen_change())

        # Overlay controls
        tk.Label(right, text="Overlay").pack(anchor="w", padx=8, pady=(8, 2))
        ov = tk.Frame(right)
        ov.pack(anchor="w", padx=8)
        tk.Button(ov, text="Load…", command=self.cmd_overlay_load).pack(fill=tk.X)
        tk.Button(ov, text="Clear", command=self.cmd_overlay_clear).pack(fill=tk.X)

        # Status bar
        status = tk.Frame(self.root)
        status.pack(side=tk.BOTTOM, fill=tk.X)
        self.status_var = tk.StringVar(value="Ready")
        tk.Label(status, textvariable=self.status_var, anchor="w").pack(side=tk.LEFT, fill=tk.X, expand=True)

        # Mouse bindings
        self.canvas.bind("<Button-1>", self.on_mouse_down)
        self.canvas.bind("<Double-1>", self.on_double_click)
        self.canvas.bind("<B1-Motion>", self.on_mouse_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_mouse_up)
        self.canvas.bind("<Motion>", self.on_mouse_move)

        # Keyboard
        self.root.bind("<Delete>", lambda e: self.cmd_delete())
        self.root.bind("<KeyPress-Shift_L>", self._on_shift_down)
        self.root.bind("<KeyRelease-Shift_L>", self._on_shift_up)
        self.root.bind("<KeyPress-Shift_R>", self._on_shift_down)
        self.root.bind("<KeyRelease-Shift_R>", self._on_shift_up)
        # Undo/Redo bindings
        self.root.bind_all("<Control-z>", lambda e: self.cmd_undo())
        self.root.bind_all("<Control-Z>", lambda e: self.cmd_undo())
        self.root.bind_all("<Command-z>", lambda e: self.cmd_undo())
        self.root.bind_all("<Command-Z>", lambda e: self.cmd_undo())
        self.root.bind_all("<Control-y>", lambda e: self.cmd_redo())
        self.root.bind_all("<Control-Y>", lambda e: self.cmd_redo())
        self.root.bind_all("<Command-Shift-Z>", lambda e: self.cmd_redo())
        self.root.bind_all("<Command-Shift-z>", lambda e: self.cmd_redo())
        # Delete (supports BackSpace on mac keyboards)
        self.root.bind_all("<Delete>", lambda e: (None if e.widget.winfo_class() in ('Entry','Text','Spinbox') else self.cmd_delete()))
        self.root.bind_all("<BackSpace>", lambda e: (None if e.widget.winfo_class() in ('Entry','Text','Spinbox') else self.cmd_delete()))
        # File shortcuts
        self.root.bind_all("<Control-o>", lambda e: self.cmd_load())
        self.root.bind_all("<Control-O>", lambda e: self.cmd_load())
        self.root.bind_all("<Command-o>", lambda e: self.cmd_load())
        self.root.bind_all("<Command-O>", lambda e: self.cmd_load())
        self.root.bind_all("<Control-s>", lambda e: self.cmd_save())
        self.root.bind_all("<Control-S>", lambda e: self.cmd_save())
        self.root.bind_all("<Command-s>", lambda e: self.cmd_save())
        self.root.bind_all("<Command-S>", lambda e: self.cmd_save())
        # Quit shortcuts
        self.root.bind_all("<Command-q>", lambda e: self.on_quit())
        self.root.bind_all("<Control-q>", lambda e: self.on_quit())
        # Properties
        self.root.bind_all("<Control-i>", lambda e: self.cmd_properties())
        self.root.bind_all("<Command-i>", lambda e: self.cmd_properties())
        # Single-key shortcuts (ignore when typing in entries/text)
        self._bind_key('e', self.cmd_properties)
        self._bind_key('L', self.cmd_load)
        self._bind_key('l', self.cmd_load)
        self._bind_key('S', self.cmd_save)
        self._bind_key('s', self.cmd_save)
        self._bind_key('O', self.cmd_overlay_load)
        self._bind_key('o', self.cmd_overlay_load)
        self._bind_key('c', self.cmd_overlay_clear)
        self._bind_key('C', self.cmd_overlay_clear)
        # Tools shortcuts 1-4 (respect modal dialogs)
        self._bind_key('1', lambda: self.set_tool("select"))
        self._bind_key('2', lambda: self.set_tool("filled"))
        self._bind_key('3', lambda: self.set_tool("outline"))
        self._bind_key('4', lambda: self.set_tool("zorro"))
        # Zoom shortcuts
        for seq in ("<Control-plus>", "<Control-KP_Add>", "<Control-equal>", "<Command-plus>", "<Command-equal>"):
            self.root.bind_all(seq, lambda e: self._zoom_in())
        for seq in ("<Control-minus>", "<Control-KP_Subtract>", "<Command-minus>"):
            self.root.bind_all(seq, lambda e: self._zoom_out())
        for seq in ("<Control-0>", "<Command-0>"):
            self.root.bind_all(seq, lambda e: self._set_zoom_percent("100%"))

    def set_tool(self, tool: str):
        self.tool = tool
        # keep sidebar radio buttons in sync
        try:
            self.tool_var.set(tool)
        except Exception:
            pass
        self._update_status()
        self._update_title()
        return True

    def on_quit(self):
        if getattr(self, 'dirty', False):
            resp = messagebox.askyesnocancel("Quit", "Save changes before quitting?", default=messagebox.CANCEL, icon=messagebox.WARNING)
            if resp is None:
                return
            if resp:
                if not self.cmd_save():
                    return
        try:
            self.root.destroy()
        except Exception:
            pass

    def _bind_key(self, key: str, func):
        def handler(e=None):
            try:
                cls = e.widget.winfo_class()
            except Exception:
                cls = ''
            if getattr(self, 'modal_active', False):
                return
            if cls in ('Entry', 'Text', 'Spinbox'):
                return
            func()
        self.root.bind_all(key, handler)

    def _bind_seq(self, sequence: str, func):
        def handler(e=None):
            try:
                cls = e.widget.winfo_class()
            except Exception:
                cls = ''
            if getattr(self, 'modal_active', False):
                return
            if cls in ('Entry', 'Text', 'Spinbox'):
                return
            func()
        self.root.bind_all(sequence, handler)

    def _strip_apple_menu_items(self, menubar: tk.Menu):
        targets = {"Emoji & Symbols", "Start Dictation…", "Start Dictation..."}
        try:
            end = menubar.index('end') or -1
        except Exception:
            end = -1
        for i in range(end + 1):
            try:
                submenu = menubar.entrycget(i, 'menu')
            except Exception:
                submenu = None
            if submenu:
                m = menubar.nametowidget(submenu)
                try:
                    j = m.index('end') or -1
                except Exception:
                    j = -1
                while j >= 0:
                    try:
                        lab = m.entrycget(j, 'label')
                    except Exception:
                        lab = None
                    if lab in targets:
                        try:
                            m.delete(j)
                        except Exception:
                            pass
                    j -= 1

    # Menubar helpers
    def _set_zoom_percent(self, label: str):
        try:
            v = float(label.strip('%')) / 100.0
        except Exception:
            v = 1.0
        self.zoom.set(max(0.1, min(5.0, v)))
        self.zoom_percent.set(label)
        self._redraw()

    def _zoom_in(self):
        steps = [0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0]
        z = float(self.zoom.get())
        for s in steps:
            if s - z > 1e-6:
                self._set_zoom_percent(f"{int(s*100)}%")
                return

    def _zoom_out(self):
        steps = [0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0]
        z = float(self.zoom.get())
        for s in reversed(steps):
            if z - s > 1e-6:
                self._set_zoom_percent(f"{int(s*100)}%")
                return

    def _set_video_mode(self, mode: str):
        self.video_mode.set(mode)
        self._redraw()

    # Pen menu with color swatches
    def _init_pen_menu(self):
        menu = self.pen_menu_btn['menu']
        menu.delete(0, 'end')
        # Create small color swatch images and populate radiobutton entries
        for v in (0, 1, 2, 3):
            color = PEN_COLORS.get(v, "#FF00FF")
            img = tk.PhotoImage(width=12, height=12)
            img.put(color, to=(0, 0, 12, 12))
            self._pen_imgs[v] = img
            menu.add_radiobutton(
                label=str(v),
                variable=self.pen_var,
                value=v,
                image=img,
                compound='right',  # show the swatch after the number
                command=self._on_pen_change,
            )
        self._on_pen_change()

    def _on_pen_change(self):
        # Update the menubutton to show the current pen's swatch after the number
        v = int(self.pen_var.get())
        img = self._pen_imgs.get(v)
        try:
            self.pen_menu_btn.config(image=img, compound='right')
        except Exception:
            pass

    # Drawing
    def _clear_canvas(self):
        self.canvas.delete("all")

    def _virtual_size(self) -> tuple[int, int]:
        mode = (self.video_mode.get() or 'NTSC').upper()
        if mode.startswith('PAL'):
            return 640, 512
        return 640, 400


    def _draw_shape(self, s: Shape, is_selected: bool = False):
        # Remove previous canvas items for this shape and redraw
        for cid in getattr(s, "canvas_ids", []) or []:
            self.canvas.delete(cid)
        s.canvas_ids = []

        z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
        mx1, my1, mx2, my2 = s.bounds()
        # Use inclusive end (x2/y2 + 1) to mirror bootmenu.c behavior
        x1, y1 = int((mx1 + self.OFFSET_X) * z), int((my1 + self.OFFSET_Y) * z)
        x2, y2 = int(((mx2 + self.OFFSET_X) + 1) * z), int(((my2 + self.OFFSET_Y) + 1) * z)
        color = PEN_COLORS.get(s.pen, "#FF00FF")

        if s.type == 1:  # filled rectangle
            cid = self.canvas.create_rectangle(x1, y1, x2, y2, fill=color, outline="")
            s.canvas_ids.append(cid)
        elif s.type == 2:  # outline rectangle
            cid = self.canvas.create_rectangle(x1, y1, x2, y2, outline=color, width=1)
            s.canvas_ids.append(cid)
        elif s.type == 3:  # zorro vertical stripes
            # Draw as vertical stripes every 4 model pixels, scaled by zoom
            for jm in range(mx1, mx2, 4):
                sx1 = int((jm + self.OFFSET_X) * z)
                sx2 = int((jm + 1 + self.OFFSET_X) * z)
                cid = self.canvas.create_rectangle(sx1, y1, max(sx1 + 1, sx2), y2, fill=color, outline=color)
                s.canvas_ids.append(cid)
        else:
            # Unknown type; draw bounding box in magenta
            cid = self.canvas.create_rectangle(x1, y1, x2, y2, outline="#FF00FF", dash=(2, 2))
            s.canvas_ids.append(cid)

        if is_selected:
            self._draw_selection_handles(x1, y1, x2, y2)

    def _draw_selection_handles(self, x1, y1, x2, y2):
        # Draw a highlight rectangle and 4 corner handles
        self.canvas.create_rectangle(x1, y1, x2, y2, outline="#00AAFF", dash=(3, 2))
        size = 6
        for hx, hy in [(x1, y1), (x2, y1), (x1, y2), (x2, y2)]:
            self.canvas.create_rectangle(hx - size / 2, hy - size / 2, hx + size / 2, hy + size / 2,
                                         outline="#0077CC", fill="#AAE1FF")

    def _draw_overlay(self):
        if not self.overlay:
            return
        # Load with PIL if available for alpha; else PhotoImage fallback
        if PIL_AVAILABLE:
            img = Image.open(self.overlay.image_path).convert("RGBA")
            self.overlay_native_size = img.size
            # Apply 30% opacity
            alpha = int(255 * 0.30)
            r, g, b, a = img.split()
            a = a.point(lambda p: alpha)
            img = Image.merge("RGBA", (r, g, b, a))

            # Resize to match zoom and overlay size if set
            z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
            ow, oh = img.size
            if self.overlay.w and self.overlay.h:
                tw, th = int(abs(self.overlay.w) * z), int(abs(self.overlay.h) * z)
            else:
                tw, th = int(ow * z), int(oh * z)
            if tw > 0 and th > 0:
                img = img.resize((tw, th), Image.LANCZOS)
            self.overlay.tk_image = ImageTk.PhotoImage(img)
            if self.overlay.canvas_id:
                self.canvas.delete(self.overlay.canvas_id)
            ox, oy = int(self.overlay.x * z), int(self.overlay.y * z)
            self.overlay.canvas_id = self.canvas.create_image(ox, oy, image=self.overlay.tk_image, anchor="nw")
        else:
            # Fallback: attempt PhotoImage (PNG only in many Tk builds, JPEG likely unsupported)
            try:
                img = tk.PhotoImage(file=self.overlay.image_path)
            except Exception as e:
                messagebox.showerror("Overlay", f"Failed to load image without Pillow: {e}")
                self.overlay = None
                return
            self.overlay_native_size = (img.width(), img.height())
            self.overlay.tk_image = img
            if self.overlay.canvas_id:
                self.canvas.delete(self.overlay.canvas_id)
            z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
            ox, oy = int(self.overlay.x * z), int(self.overlay.y * z)
            self.overlay.canvas_id = self.canvas.create_image(ox, oy, image=self.overlay.tk_image, anchor="nw")
            # Note: opacity not supported without PIL

        # Selection box for overlay if selected_index == 'overlay'
        if self.selected_index == -1 and self.overlay_native_size:
            z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
            x1, y1, x2, y2 = self.overlay.bounds(self.overlay_native_size)
            self._draw_selection_handles(int(x1 * z), int(y1 * z), int(x2 * z), int(y2 * z))

    def _redraw(self):
        self._clear_canvas()
        # Background area corresponding to virtual screen size
        vw, vh = self._virtual_size()
        z_bg = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
        self.canvas.create_rectangle(0, 0, int(vw * z_bg), int(vh * z_bg), fill="#dddddd", outline="#bbbbbb")
        # Draw shapes in order
        for i, s in enumerate(self.shapes):
            self._draw_shape(s, is_selected=(self.selected_index == i))
        # Overlay drawn on top
        self._draw_overlay()
        # Draw origin crosshair last
        self._draw_origin_crosshair()
        # Update scrollable region to match virtual screen and zoom
        vw, vh = self._virtual_size()
        z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
        self.canvas.config(scrollregion=(0, 0, int(vw * z), int(vh * z)))

    def _draw_origin_crosshair(self):
        zc = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
        ox, oy = int(self.OFFSET_X * zc), int(self.OFFSET_Y * zc)
        size = max(4, int(8 * zc))
        self.canvas.create_line(ox - size, oy, ox + size, oy, fill="#ff0000")
        self.canvas.create_line(ox, oy - size, ox, oy + size, fill="#ff0000")

    # Mouse helpers
    def _hit_test_overlay_handle(self, x: int, y: int) -> Optional[str]:
        if not self.overlay or not self.overlay_native_size:
            return None
        z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
        ox1, oy1, ox2, oy2 = self.overlay.bounds(self.overlay_native_size)
        ox1, oy1, ox2, oy2 = int(ox1 * z), int(oy1 * z), int(ox2 * z), int(oy2 * z)
        size = max(6, int(6 * z))
        handles = {
            "tl": (ox1, oy1),
            "tr": (ox2, oy1),
            "bl": (ox1, oy2),
            "br": (ox2, oy2),
        }
        for name, (hx, hy) in handles.items():
            if abs(x - hx) <= size and abs(y - hy) <= size:
                return name
        return None

    def _hit_test_shape_handle(self, s: Shape, x: int, y: int) -> Optional[str]:
        z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
        mx1, my1, mx2, my2 = s.bounds()
        # Inclusive end to mirror bootmenu.c
        x1, y1 = int((mx1 + self.OFFSET_X) * z), int((my1 + self.OFFSET_Y) * z)
        x2, y2 = int(((mx2 + self.OFFSET_X) + 1) * z), int(((my2 + self.OFFSET_Y) + 1) * z)
        size = max(6, int(6 * z))
        handles = {
            "tl": (x1, y1),
            "tr": (x2, y1),
            "bl": (x1, y2),
            "br": (x2, y2),
        }
        for name, (hx, hy) in handles.items():
            if abs(x - hx) <= size and abs(y - hy) <= size:
                return name
        return None

    def _fixed_corner_for_handle(self, x1: int, y1: int, x2: int, y2: int, handle: str) -> Tuple[int, int]:
        # Given normalized bounds and the active handle, return the opposite (fixed) corner
        if handle == "tl":
            return x2, y2
        if handle == "tr":
            return x1, y2
        if handle == "bl":
            return x2, y1
        # "br" or fallback
        return x1, y1

    def _constrain_point_to_aspect(self, fx: int, fy: int, mx: int, my: int, aspect: float) -> Tuple[int, int]:
        # Constrain moving corner (mx,my) so box (fx,fy)-(nx,ny) keeps given aspect ratio (w/h)
        dx = mx - fx
        dy = my - fy
        sgnx = 1 if dx >= 0 else -1
        sgny = 1 if dy >= 0 else -1
        adx = abs(dx)
        ady = abs(dy)
        if aspect <= 0:
            aspect = 1.0
        if adx == 0 and ady == 0:
            return mx, my
        # Adjust width/height to maintain aspect ratio
        if ady == 0:
            width = adx
            height = max(1, int(round(width / aspect)))
        elif adx == 0:
            height = ady
            width = max(1, int(round(height * aspect)))
        else:
            ratio = adx / ady
            if ratio > aspect:
                # too wide -> increase height
                width = adx
                height = int(round(width / aspect))
            else:
                # too tall -> increase width
                height = ady
                width = int(round(height * aspect))
        nx = fx + sgnx * width
        ny = fy + sgny * height
        return nx, ny

    def _shape_at_point(self, x: int, y: int) -> Optional[int]:
        # Return topmost shape index whose bounds contain the point
        z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
        for i in reversed(range(len(self.shapes))):
            s = self.shapes[i]
            mx1, my1, mx2, my2 = s.bounds()
            # Inclusive end to match drawing logic
            x1, y1 = int((mx1 + self.OFFSET_X) * z), int((my1 + self.OFFSET_Y) * z)
            x2, y2 = int(((mx2 + self.OFFSET_X) + 1) * z), int(((my2 + self.OFFSET_Y) + 1) * z)
            if x1 <= x <= x2 and y1 <= y <= y2:
                return i
        return None

    def _cursor_for_handle(self, handle: Optional[str]) -> str:
        # Use platform-friendly diagonal resize cursors
        if handle == "tl":
            return "top_left_corner"
        if handle == "tr":
            return "top_right_corner"
        if handle == "bl":
            return "bottom_left_corner"
        if handle == "br":
            return "bottom_right_corner"
        return "arrow"

    def _apply_cursor(self, name: str) -> None:
        # Try preferred cursor; fall back for platform differences
        try:
            self.canvas.config(cursor=name)
            return
        except Exception:
            pass
        # Fallbacks for move and resize
        fallback = {
            "fleur": "size_all",
            "top_left_corner": "ul_angle",
            "top_right_corner": "ur_angle",
            "bottom_left_corner": "ll_angle",
            "bottom_right_corner": "lr_angle",
        }.get(name, "arrow")
        try:
            self.canvas.config(cursor=fallback)
        except Exception:
            self.canvas.config(cursor="arrow")

    def _update_cursor(self, x: int, y: int) -> None:
        # During drag, reflect current operation
        if self.drag_mode == "move" or self.drag_mode == "overlay-move":
            self._apply_cursor("fleur")
            return
        if self.drag_mode in ("resize", "overlay-resize"):
            self._apply_cursor(self._cursor_for_handle(self.resize_handle))
            return

        # 1) Handle-resize hover takes precedence, regardless of tool
        if self.selected_index == -1 and self.overlay and self.overlay_native_size:
            rh = self._hit_test_overlay_handle(x, y)
            if rh:
                self._apply_cursor(self._cursor_for_handle(rh))
                return
        if isinstance(self.selected_index, int) and self.selected_index is not None and 0 <= self.selected_index < len(self.shapes):
            rh = self._hit_test_shape_handle(self.shapes[self.selected_index], x, y)
            if rh:
                self._apply_cursor(self._cursor_for_handle(rh))
                return

        # 2) Non-handle hover depends on tool
        if self.tool == "select":
            hit_shape = self._shape_at_point(x, y)
            if self.selected_index is None:
                # Nothing selected yet: indicate selectable with a hand cursor
                if hit_shape is not None:
                    self._apply_cursor("hand2")
                    return
                # Overlay body hover when nothing selected -> also show selectable
                if self.overlay and self.overlay_native_size:
                    z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
                    ox1, oy1, ox2, oy2 = self.overlay.bounds(self.overlay_native_size)
                    ox1, oy1, ox2, oy2 = int(ox1 * z), int(oy1 * z), int(ox2 * z), int(oy2 * z)
                    if ox1 <= x <= ox2 and oy1 <= y <= oy2:
                        self._apply_cursor("hand2")
                        return
            else:
                # Something is selected: show move when hovering selected body
                if self.selected_index >= 0:
                    # shape selected
                    if hit_shape == self.selected_index:
                        self._apply_cursor("fleur")
                        return
                elif self.selected_index == -1 and self.overlay and self.overlay_native_size:
                    z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
                    ox1, oy1, ox2, oy2 = self.overlay.bounds(self.overlay_native_size)
                    ox1, oy1, ox2, oy2 = int(ox1 * z), int(oy1 * z), int(ox2 * z), int(oy2 * z)
                    if ox1 <= x <= ox2 and oy1 <= y <= oy2:
                        self._apply_cursor("fleur")
                        return
            # Default in select mode when not over selectable body
            self._apply_cursor("arrow")
            return
        else:
            # Drawing tools: show crosshair to indicate drawing
            self._apply_cursor("crosshair")
            return

    # Mouse events
    def on_mouse_down(self, event):
        x, y = int(self.canvas.canvasx(event.x)), int(self.canvas.canvasy(event.y))
        self.drag_start = (x, y)
        self.drag_mode = None
        self.resize_handle = None

        # Selection logic: give shapes priority over overlay when overlapping
        z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
        if self.tool == 'select':
            idx = self._shape_at_point(x, y)
            if idx is not None:
                self.selected_index = idx
                rh = self._hit_test_shape_handle(self.shapes[idx], x, y)
                if rh:
                    self.push_undo()
                    self.drag_mode = 'resize'
                    self.resize_handle = rh
                    bx1, by1, bx2, by2 = self.shapes[idx].bounds()
                    self.resize_initial = (bx1, by1, bx2, by2)
                else:
                    self.push_undo()
                    self.drag_mode = 'move'
                self._redraw()
                return
            if self.overlay and self.overlay_native_size:
                ox1, oy1, ox2, oy2 = self.overlay.bounds(self.overlay_native_size)
                sox1, soy1, sox2, soy2 = int(ox1 * z), int(oy1 * z), int(ox2 * z), int(oy2 * z)
                rh = self._hit_test_overlay_handle(x, y)
                if rh:
                    self.selected_index = -1
                    self.push_undo()
                    self.drag_mode = 'overlay-resize'
                    self.resize_handle = rh
                    self.resize_initial = (ox1, oy1, ox2, oy2)
                    self._redraw()
                    return
                if sox1 <= x <= sox2 and soy1 <= y <= soy2:
                    self.selected_index = -1
                    self.push_undo()
                    self.drag_mode = 'overlay-move'
                    self._redraw()
                    return
            self.selected_index = None
            self._redraw()
            self.dirty = True
            self._update_title()
        elif self.tool == "zorro":
            # Place a default-sized Zorro shape centered at click; allow dragging to position
            mx = x / z - self.OFFSET_X
            my = y / z - self.OFFSET_Y
            cx = max(0, mx - self.ZORRO_DEFAULT_W // 2)
            cy = max(0, my - self.ZORRO_DEFAULT_H // 2)
            self.push_undo()
            s = Shape(3, int(self.pen_var.get()), int(cx), int(cy), int(self.ZORRO_DEFAULT_W), int(self.ZORRO_DEFAULT_H))
            s.normalize()
            self.shapes.append(s)
            self.selected_index = len(self.shapes) - 1
            self.drag_mode = "move"
            self._redraw()
        else:
            # Begin creating new shape
            self.drag_mode = "create"
            # preview rectangle drawn during drag
            if self.current_preview_id:
                self.canvas.delete(self.current_preview_id)
                self.current_preview_id = None
        self._update_cursor(x, y)

    def on_mouse_drag(self, event):
        if not self.drag_start:
            return
        x0, y0 = self.drag_start
        x, y = int(self.canvas.canvasx(event.x)), int(self.canvas.canvasy(event.y))

        if self.drag_mode == "move" and self.selected_index is not None:
            s = self.shapes[self.selected_index]
            z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
            dx, dy = (x - x0) / z, (y - y0) / z
            s.x = max(0, int(round(s.x + dx)))
            s.y = max(0, int(round(s.y + dy)))
            self.drag_start = (x, y)
            self._redraw()
            self._update_cursor(x, y)
            self.dirty = True
            self._update_title()
            self.dirty = True
            self._update_title()
        elif self.drag_mode == "resize" and self.selected_index is not None:
            s = self.shapes[self.selected_index]
            # Use initial normalized bounds for stable aspect calculations
            if self.resize_initial is None:
                self.resize_initial = s.bounds()
            ix1, iy1, ix2, iy2 = self.resize_initial
            fx, fy = self._fixed_corner_for_handle(ix1, iy1, ix2, iy2, self.resize_handle or "br")
            # Keep aspect ratio with Shift
            if self.shift_down:
                iw = max(1, ix2 - ix1)
                ih = max(1, iy2 - iy1)
                z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
                nx, ny = self._constrain_point_to_aspect(fx, fy, x / z - self.OFFSET_X, y / z - self.OFFSET_Y, iw / ih)
            else:
                z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
                nx, ny = x / z - self.OFFSET_X, y / z - self.OFFSET_Y
            x1, y1 = min(fx, nx), min(fy, ny)
            x2, y2 = max(fx, nx), max(fy, ny)
            nx1, ny1 = max(0, x1), max(0, y1)
            s.x, s.y = int(round(nx1)), int(round(ny1))
            s.w, s.h = int(round(max(1, x2 - nx1))), int(round(max(1, y2 - ny1)))
            self._redraw()
            self._update_cursor(x, y)
        elif self.drag_mode == "create" and self.tool in ("filled", "outline"):
            # Draw a transient preview rectangle (Shift -> square)
            if self.current_preview_id:
                self.canvas.delete(self.current_preview_id)
            rx, ry = x, y
            if self.shift_down:
                dx, dy = x - x0, y - y0
                side = max(abs(dx), abs(dy))
                rx = x0 + (side if dx >= 0 else -side)
                ry = y0 + (side if dy >= 0 else -side)
            self.current_preview_id = self.canvas.create_rectangle(x0, y0, rx, ry, outline="#888", dash=(2, 2))
            self._update_cursor(x, y)
        elif self.drag_mode == "overlay-move" and self.overlay:
            z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
            dx, dy = (x - x0) / z, (y - y0) / z
            self.overlay.x += dx
            self.overlay.y += dy
            self.drag_start = (x, y)
            self._redraw()
            self._update_cursor(x, y)
        elif self.drag_mode == "overlay-resize" and self.overlay and self.overlay_native_size:
            # Use initial bounds for stable aspect calculations
            if self.resize_initial is None:
                self.resize_initial = self.overlay.bounds(self.overlay_native_size)
            ix1, iy1, ix2, iy2 = self.resize_initial
            fx, fy = self._fixed_corner_for_handle(ix1, iy1, ix2, iy2, self.resize_handle or "br")
            if self.shift_down:
                iw = max(1, ix2 - ix1)
                ih = max(1, iy2 - iy1)
                z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
                nx, ny = self._constrain_point_to_aspect(fx, fy, x / z, y / z, iw / ih)
            else:
                z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
                nx, ny = x / z, y / z
            ox1, oy1 = min(fx, nx), min(fy, ny)
            ox2, oy2 = max(fx, nx), max(fy, ny)
            self.overlay.x, self.overlay.y = ox1, oy1
            self.overlay.w, self.overlay.h = ox2 - ox1, oy2 - oy1
            self._redraw()
            self._update_cursor(x, y)

    def on_mouse_up(self, event):
        x, y = int(self.canvas.canvasx(event.x)), int(self.canvas.canvasy(event.y))
        if self.drag_mode == "create" and self.tool in ("filled", "outline") and self.drag_start:
            x0, y0 = self.drag_start
            # Convert to model coordinates and clamp to >= 0
            z = float(getattr(self, 'zoom', 1.0).get() if hasattr(self, 'zoom') else 1.0)
            x1, y1 = x0 / z - self.OFFSET_X, y0 / z - self.OFFSET_Y
            x2, y2 = x / z - self.OFFSET_X, y / z - self.OFFSET_Y
            if int(round(x0)) == int(round(x)) and int(round(y0)) == int(round(y)):
                if self.current_preview_id:
                    self.canvas.delete(self.current_preview_id)
                    self.current_preview_id = None
                self.drag_mode = None
                self.drag_start = None
                self.resize_handle = None
                self.resize_initial = None
                self._update_cursor(x, y)
                return
            if x2 < x1:
                x1, x2 = x2, x1
            if y2 < y1:
                y1, y2 = y2, y1
            new_type = {"filled": 1, "outline": 2}[self.tool]
            x1 = max(0, x1)
            y1 = max(0, y1)
            w = max(1, x2 - x1)
            h = max(1, y2 - y1)
            self.push_undo()
            s = Shape(new_type, int(self.pen_var.get()), int(round(x1)), int(round(y1)), int(round(w)), int(round(h)))
            s.normalize()
            self.shapes.append(s)
            self.selected_index = len(self.shapes) - 1
            if self.current_preview_id:
                self.canvas.delete(self.current_preview_id)
                self.current_preview_id = None
            self._redraw()
        # Always clear drag state and recalc cursor at mouse up
        self.drag_mode = None
        self.drag_start = None
        self.resize_handle = None
        self.resize_initial = None
        self._update_cursor(x, y)

    def on_double_click(self, event):
        # Open properties for the shape under cursor (or current selection)
        x, y = int(self.canvas.canvasx(event.x)), int(self.canvas.canvasy(event.y))
        idx = self._shape_at_point(x, y)
        if idx is not None:
            self.selected_index = idx
            self._redraw()
            self.cmd_properties()
        elif self.selected_index is not None and self.selected_index >= 0:
            self.cmd_properties()

        self.drag_mode = None
        self.drag_start = None
        self.resize_handle = None
        self.resize_initial = None
        self._update_cursor(x, y)

    def on_mouse_move(self, event):
        x, y = int(self.canvas.canvasx(event.x)), int(self.canvas.canvasy(event.y))
        self._update_cursor(x, y)
        self._update_status(x, y)

    def _update_status(self, cx: Optional[int]=None, cy: Optional[int]=None):
        try:
            z = float(self.zoom.get())
        except Exception:
            z = 1.0
        if cx is None or cy is None:
            try:
                cx = int(self.canvas.canvasx(0))
                cy = int(self.canvas.canvasy(0))
            except Exception:
                cx, cy = 0, 0
        # Canvas -> model coords
        mx = int(round(cx / z - self.OFFSET_X))
        my = int(round(cy / z - self.OFFSET_Y))
        sel = "None"
        if isinstance(self.selected_index, int) and 0 <= self.selected_index < len(self.shapes):
            s = self.shapes[self.selected_index]
            sel = f"type={s.type} pen={s.pen} x={s.x} y={s.y} w={s.w} h={s.h}"
        elif self.selected_index == -1 and self.overlay:
            sel = f"overlay x={int(self.overlay.x)} y={int(self.overlay.y)} w={int(self.overlay.w or 0)} h={int(self.overlay.h or 0)}"
        vw, vh = self._virtual_size()
        self.status_var.set(f"Zoom {int(z*100)}% | Video {self.video_mode.get()} | Cursor x={mx} y={my} | Sel {sel} | Shapes {len(self.shapes)}")


    def _update_title(self):
        name = self.array_name or 'Untitled'
        fname = os.path.basename(self.file_path) if self.file_path else None
        dirty = getattr(self, 'dirty', False)
        suffix = ' *' if dirty else ''
        if fname:
            self.root.title(f'A409x Drawing Editor — {fname} [{name}]' + suffix)
        else:
            self.root.title(f'A409x Drawing Editor — {name}' + suffix)

    # Shift tracking
    def _on_shift_down(self, event=None):
        self.shift_down = True

    def _on_shift_up(self, event=None):
        self.shift_down = False

    # Commands
    def cmd_delete(self):
        if self.selected_index is None:
            return
        if self.selected_index == -1:  # overlay
            self.push_undo()
            self.overlay = None
            self.overlay_native_size = None
            self.selected_index = None
            self._redraw()
            self.dirty = True
            self._update_title()
            return
        idx = self.selected_index
        if 0 <= idx < len(self.shapes):
            self.push_undo()
            del self.shapes[idx]
            self.selected_index = None
            self._redraw()
            self.dirty = True
            self._update_title()

    def cmd_load(self):
        path = filedialog.askopenfilename(
            title="Load header",
            filetypes=[("C headers", "*.h"), ("All", "*.*")],
            initialdir=self._initial_dir_for_headers(),
        )
        if not path:
            return False
        try:
            with open(path, "r", encoding="utf-8") as f:
                text = f.read()
        except Exception as e:
            messagebox.showerror("Load", f"Failed to read file: {e}")
            return
        arr_name, shapes = parse_header(text)
        if not shapes:
            # Provide basic diagnostics to help spot format issues
            msg = "No shapes found in file. Ensure it contains entries like { type, pen, x, y, w, h }."
            messagebox.showerror("Load", msg)
            return
        self.array_name = arr_name or os.path.splitext(os.path.basename(path))[0]
        self.file_path = path
        self.shapes = shapes
        self.selected_index = None
        self.undo_stack.clear()
        self.redo_stack.clear()
        self._redraw()
        self.dirty = False
        self._update_title()
        # Remember last directory
        self.state.last_dir = os.path.dirname(path)
        self.state.save()

    def cmd_save(self):
        if not self.shapes:
            messagebox.showinfo("Save", "No shapes to save.")
            return False
        # Ask array name
        name = simpledialog.askstring("Array name", "Name of the C array:", initialvalue=self.array_name)
        if not name:
            return False
        self.array_name = name
        path = filedialog.asksaveasfilename(
            title="Save header",
            defaultextension=".h",
            filetypes=[("C headers", "*.h"), ("All", "*.*")],
            initialdir=self._initial_dir_for_save(),
            initialfile=(os.path.basename(self.file_path) if self.file_path else f"{self.array_name}.h"),
        )
        if not path:
            return False
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(serialize_header(self.array_name, self.shapes))
        except Exception as e:
            messagebox.showerror("Save", f"Failed to write file: {e}")
            return False
        self.file_path = path
        # Remember last directory
        self.state.last_dir = os.path.dirname(path)
        self.state.save()
        self.dirty = False
        self._update_title()

    def cmd_properties(self):
        if self.selected_index is None or self.selected_index < 0 or self.selected_index >= len(self.shapes):
            messagebox.showinfo("Properties", "Select a shape first.")
            return
        s = self.shapes[self.selected_index]

        win = tk.Toplevel(self.root)
        win.title("Shape Properties")
        win.transient(self.root)
        win.grab_set()
        self.modal_active = True

        x_var = tk.IntVar(value=max(0, s.x))
        y_var = tk.IntVar(value=max(0, s.y))
        w_var = tk.IntVar(value=max(1, s.w))
        h_var = tk.IntVar(value=max(1, s.h))
        pen_var = tk.IntVar(value=s.pen)
        cmt_var = tk.StringVar(value=s.comment or "")

        # Row 0: X | Y
        tk.Label(win, text="X:").grid(row=0, column=0, sticky='e', padx=6, pady=4)
        tk.Entry(win, textvariable=x_var, width=10).grid(row=0, column=1, sticky='w', padx=6, pady=4)
        tk.Label(win, text="Y:").grid(row=0, column=2, sticky='e', padx=6, pady=4)
        tk.Entry(win, textvariable=y_var, width=10).grid(row=0, column=3, sticky='w', padx=6, pady=4)

        # Row 1: Width | Height
        tk.Label(win, text="Width:").grid(row=1, column=0, sticky='e', padx=6, pady=4)
        tk.Entry(win, textvariable=w_var, width=10).grid(row=1, column=1, sticky='w', padx=6, pady=4)
        tk.Label(win, text="Height:").grid(row=1, column=2, sticky='e', padx=6, pady=4)
        tk.Entry(win, textvariable=h_var, width=10).grid(row=1, column=3, sticky='w', padx=6, pady=4)

        # Row 2: Pen with swatch like main screen
        tk.Label(win, text="Pen:").grid(row=2, column=0, sticky='e', padx=6, pady=4)
        pen_menu = tk.OptionMenu(win, pen_var, 0, 1, 2, 3)
        menu = pen_menu['menu']
        menu.delete(0, 'end')
        win._pen_imgs_local = {}
        for v in (0, 1, 2, 3):
            color = PEN_COLORS.get(v, "#FF00FF")
            img = tk.PhotoImage(width=12, height=12)
            img.put(color, to=(0, 0, 12, 12))
            win._pen_imgs_local[v] = img
            menu.add_radiobutton(label=str(v), variable=pen_var, value=v, image=img, compound='right')
        # Keep the menubutton image in sync, like toolbar
        def _update_pen_btn(*_):
            v = int(pen_var.get())
            img = win._pen_imgs_local.get(v)
            try:
                pen_menu.config(image=img, compound='right')
            except Exception:
                pass
        try:
            pen_var.trace_add('write', _update_pen_btn)
        except Exception:
            pen_var.trace('w', _update_pen_btn)
        _update_pen_btn()
        pen_menu.grid(row=2, column=1, sticky='w', padx=6, pady=4, columnspan=3)

        # Row 3: Comment across columns
        tk.Label(win, text="Comment:").grid(row=3, column=0, sticky='e', padx=6, pady=4)
        tk.Entry(win, textvariable=cmt_var, width=40).grid(row=3, column=1, sticky='we', padx=6, pady=4, columnspan=3)

        # Buttons row
        btns = tk.Frame(win)
        btns.grid(row=4, column=0, columnspan=4, pady=8)
        def cancel_close():
            try:
                win.destroy()
            finally:
                self.modal_active = False
        def apply_and_close():
            try:
                nx = max(0, int(x_var.get()))
                ny = max(0, int(y_var.get()))
                nw = max(1, int(w_var.get()))
                nh = max(1, int(h_var.get()))
            except Exception:
                messagebox.showerror("Properties", "X, Y, Width, and Height must be integers.")
                return
            self.push_undo()
            s.x, s.y, s.w, s.h = nx, ny, nw, nh
            s.pen = int(pen_var.get())
            cmt = cmt_var.get().strip()
            s.comment = cmt if cmt else None
            self._redraw()
            win.destroy()
            self.dirty = True
            self._update_title()
            self.modal_active = False
        tk.Button(btns, text="OK", command=apply_and_close).pack(side=tk.LEFT, padx=6)
        tk.Button(btns, text="Cancel", command=cancel_close).pack(side=tk.LEFT, padx=6)
        win.bind('<Escape>', lambda e: cancel_close())
        win.protocol('WM_DELETE_WINDOW', cancel_close)

    # Undo/Redo machinery
    def snapshot_state(self) -> dict:
        shapes_copy = [Shape(s.type, s.pen, s.x, s.y, s.w, s.h, getattr(s, 'comment', None)) for s in self.shapes]
        overlay_copy = None
        if self.overlay:
            overlay_copy = Overlay(self.overlay.image_path, self.overlay.x, self.overlay.y, self.overlay.w, self.overlay.h)
        return {
            'shapes': shapes_copy,
            'selected_index': self.selected_index,
            'overlay': overlay_copy,
        }

    def restore_state(self, snap: dict) -> None:
        self.shapes = [Shape(s.type, s.pen, s.x, s.y, s.w, s.h, getattr(s, 'comment', None)) for s in snap.get('shapes', [])]
        self.selected_index = snap.get('selected_index')
        ov = snap.get('overlay')
        if ov:
            self.overlay = Overlay(ov.image_path, ov.x, ov.y, ov.w, ov.h)
            self.overlay_native_size = None
        else:
            self.overlay = None
            self.overlay_native_size = None
        self._redraw()

    def push_undo(self) -> None:
        self.undo_stack.append(self.snapshot_state())
        if len(self.undo_stack) > 100:
            self.undo_stack.pop(0)
        self.redo_stack.clear()

    def cmd_undo(self):
        if not self.undo_stack:
            return
        current = self.snapshot_state()
        snap = self.undo_stack.pop()
        self.redo_stack.append(current)
        self.restore_state(snap)

    def cmd_redo(self):
        if not self.redo_stack:
            return
        current = self.snapshot_state()
        snap = self.redo_stack.pop()
        self.undo_stack.append(current)
        self.restore_state(snap)

    def cmd_overlay_load(self):
        path = filedialog.askopenfilename(
            title="Load overlay image",
            filetypes=[
                ("Images", "*.png;*.jpg;*.jpeg;*.bmp;*.gif"),
                ("All", "*.*"),
            ],
            initialdir=self._initial_dir_for_overlay(),
        )
        if not path:
            return False
        self.push_undo()
        self.overlay = Overlay(path)
        # Fit overlay into the current canvas, preserving aspect ratio with margins
        size = self._get_image_size(path)
        if size:
            self._fit_overlay_to_canvas(self.overlay, size)
        self.selected_index = -1
        self._redraw()
        # Remember last overlay directory
        self.state.last_overlay_dir = os.path.dirname(path)
        self.state.save()

    def cmd_overlay_clear(self):
        self.overlay = None
        self.overlay_native_size = None
        if self.selected_index == -1:
            self.selected_index = None
        self._redraw()

    # Initial directory helpers
    def _initial_dir_for_headers(self) -> str:
        # Prefer last_dir; otherwise current working directory
        return self.state.last_dir or os.getcwd()

    def _initial_dir_for_save(self) -> str:
        # Prefer directory of current file, else last_dir, else CWD
        if self.file_path:
            return os.path.dirname(self.file_path)
        return self.state.last_dir or os.getcwd()

    def _initial_dir_for_overlay(self) -> str:
        return self.state.last_overlay_dir or self.state.last_dir or os.getcwd()

    # Overlay helpers
    def _get_image_size(self, path: str) -> Optional[Tuple[int, int]]:
        try:
            if PIL_AVAILABLE:
                from PIL import Image  # local import to avoid issues if PIL missing
                with Image.open(path) as im:
                    return im.size
            else:
                img = tk.PhotoImage(file=path)
                return (img.width(), img.height())
        except Exception:
            return None

    def _fit_overlay_to_canvas(self, overlay: Overlay, native_size: Tuple[int, int]) -> None:
        # Fit overlay within the virtual screen (model space), independent of zoom.
        vw, vh = self._virtual_size()
        margin = max(12, min(vw, vh) // 32)  # model pixels
        max_w = max(1, vw - 2 * margin)
        max_h = max(1, vh - 2 * margin)

        ow, oh = native_size
        if ow <= 0 or oh <= 0:
            return
        scale = min(max_w / ow, max_h / oh, 1.0)  # don't upscale by default
        overlay.w = int(ow * scale)
        overlay.h = int(oh * scale)
        overlay.x = margin
        overlay.y = margin


# Parsing and serialization
HEADER_ARRAY_RE = re.compile(
    r"(?:(?:static)\s+)?const\s+struct\s+drawing\s+(?P<name>[a-zA-Z_][a-zA-Z0-9_]*)\s*\[\s*\]\s*=\s*\{",
    re.MULTILINE,
)
ENTRY_WITH_COMMENT_RE = re.compile(
    r"\{\s*([-+]?\d+)\s*,\s*([-+]?\d+)\s*,\s*([-+]?\d+)\s*,\s*([-+]?\d+)\s*,\s*([-+]?\d+)\s*,\s*([-+]?\d+)\s*\}\s*,?\s*(?://\s*(.*))?$"
)


def parse_header(text: str) -> Tuple[Optional[str], List[Shape]]:
    """Parse a header file containing a single `struct drawing` array.

    Captures trailing line comments (// ...) as shape comments.
    """
    # Find array name and the opening brace in raw text
    m = HEADER_ARRAY_RE.search(text)
    array_name = m.group("name") if m else None
    shapes: List[Shape] = []
    if not m:
        return array_name, shapes

    # Isolate the array body by matching braces
    start = m.end()  # position after the opening '{'
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
        i += 1
    body = text[start:i-1] if depth == 0 else text[start:]

    # Parse entries line by line to capture trailing // comments
    for raw_line in body.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        m2 = ENTRY_WITH_COMMENT_RE.match(line)
        if not m2:
            continue
        tval, p, x, y, w, h = (int(m2.group(k)) for k in range(1, 7))
        comment = m2.group(7).strip() if m2.group(7) else None
        s = Shape(tval, p, x, y, w, h, comment=comment)
        s.normalize()
        shapes.append(s)
    return array_name, shapes


def serialize_header(array_name: str, shapes: List[Shape]) -> str:
    lines = []
    lines.append(f"static const struct drawing {array_name}[] = {{")
    for i, s in enumerate(shapes):
        # Pretty-print each entry; retain integer values as-is
        entry = f"    {{ {int(s.type)}, {int(s.pen)}, {int(s.x):4d}, {int(s.y):4d}, {int(s.w):4d}, {int(s.h):4d} }}"
        if i < len(shapes) - 1:
            entry += ","
        if getattr(s, 'comment', None):
            entry += f" // {s.comment}"
        lines.append(entry)
    lines.append("};\n")
    return "\n".join(lines)


# Simple persisted state in the user's home directory
STATE_PATH = Path(os.path.expanduser("~/.a409x_editor_state.json"))


@dataclass
class PersistedState:
    last_dir: Optional[str] = None
    last_overlay_dir: Optional[str] = None

    @classmethod
    def load(cls) -> "PersistedState":
        try:
            if STATE_PATH.exists():
                with STATE_PATH.open("r", encoding="utf-8") as f:
                    data = json.load(f)
                return cls(**{k: data.get(k) for k in ("last_dir", "last_overlay_dir")})
        except Exception:
            pass
        return cls()

    def save(self) -> None:
        try:
            with STATE_PATH.open("w", encoding="utf-8") as f:
                json.dump({
                    "last_dir": self.last_dir,
                    "last_overlay_dir": self.last_overlay_dir,
                }, f, indent=2)
        except Exception:
            # Ignore persistence errors to avoid disrupting UX
            pass


def main():
    root = tk.Tk()
    app = EditorApp(root)

    # If a file path is passed as arg, load it
    if len(sys.argv) > 1:
        try:
            with open(sys.argv[1], "r", encoding="utf-8") as f:
                text = f.read()
            arr_name, shapes = parse_header(text)
            if shapes:
                app.array_name = arr_name or app.array_name
                app.shapes = shapes
        except Exception as e:
            messagebox.showerror("Startup load", f"Failed to load {sys.argv[1]}: {e}")
    app._redraw()

    root.mainloop()


if __name__ == "__main__":
    main()
