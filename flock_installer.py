"""flock-mini installer - a small GUI wrapper around PlatformIO.

Extracts the firmware source from FIRMWARE.md, makes sure PlatformIO is
present, then compiles and flashes the ESP8266.
"""

import os
import re
import subprocess
import sys
import threading
import queue
import shutil
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

APP_TITLE = "flock-mini installer"
FENCE = chr(96) * 3
BLOCK_RE = re.compile(r"(?ms)^" + FENCE + r"(cpp|ini)\r?\n(.*?)^" + FENCE)
COM_RE = re.compile(r"\((COM\d+)\)")
LIKELY_RE = re.compile(
    r"CH340|CH341|USB-SERIAL|USB Serial|CP210|Silicon Labs|FTDI|Prolific", re.I
)

CREATE_NO_WINDOW = 0x08000000
CREATE_NEW_CONSOLE = 0x00000010

DRIVER_URL = "https://sparks.gogo.co.nz/ch340.html"


def app_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


def bundled_firmware():
    base = getattr(sys, "_MEIPASS", None)
    if not base:
        return None
    path = os.path.join(base, "FIRMWARE.md")
    return path if os.path.isfile(path) else None


def python_exe():
    """A real interpreter, which is not sys.executable once we are frozen."""
    if getattr(sys, "frozen", False):
        for name in ("python", "python3", "py"):
            found = shutil.which(name)
            if found:
                return found
        return None
    return sys.executable


def pio_cmd():
    found = shutil.which("pio") or shutil.which("platformio")
    if found:
        return [found]
    py = python_exe()
    return [py, "-m", "platformio"] if py else None


def list_ports():
    """COM ports with friendly names, via PowerShell so we need no pyserial."""
    script = (
        "Get-CimInstance Win32_PnPEntity | "
        "Where-Object { $_.Name -match '\\(COM\\d+\\)' } | "
        "Select-Object -ExpandProperty Name"
    )
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command", script],
            capture_output=True, text=True, timeout=20,
            creationflags=CREATE_NO_WINDOW,
        ).stdout
    except Exception:
        return []

    ports = []
    for line in out.splitlines():
        line = line.strip()
        m = COM_RE.search(line)
        if m:
            ports.append((m.group(1), line, bool(LIKELY_RE.search(line))))
    # USB adapters first: COM1/COM2 are usually legacy motherboard ports
    ports.sort(key=lambda p: (not p[2], p[0]))
    return ports


class Installer(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.geometry("760x560")
        self.minsize(640, 480)

        self.msgs = queue.Queue()
        self.worker = None
        self.project = tk.StringVar(value=app_dir())
        self.port = tk.StringVar()
        self.extract = tk.BooleanVar(value=True)
        self.do_flash = tk.BooleanVar(value=True)
        self.status = tk.StringVar(value="Ready")

        self._build_ui()
        self.refresh_ports()
        self.after(100, self._pump)

    # ---------------------------------------------------------------- ui

    def _build_ui(self):
        pad = {"padx": 10, "pady": 6}

        head = ttk.Frame(self)
        head.pack(fill="x", **pad)
        ttk.Label(head, text="flock-mini", font=("Segoe UI", 15, "bold")).pack(side="left")
        ttk.Label(head, text="  passive Flock Safety detector for ESP8266",
                  foreground="#666").pack(side="left")

        proj = ttk.Frame(self)
        proj.pack(fill="x", **pad)
        ttk.Label(proj, text="Project folder").pack(side="left")
        ttk.Entry(proj, textvariable=self.project).pack(
            side="left", fill="x", expand=True, padx=8)
        ttk.Button(proj, text="Browse", command=self.browse).pack(side="left")

        row = ttk.Frame(self)
        row.pack(fill="x", **pad)
        ttk.Label(row, text="Board port").pack(side="left")
        self.port_box = ttk.Combobox(row, textvariable=self.port, width=48, state="readonly")
        self.port_box.pack(side="left", padx=8)
        ttk.Button(row, text="Refresh", command=self.refresh_ports).pack(side="left")

        opts = ttk.Frame(self)
        opts.pack(fill="x", **pad)
        ttk.Checkbutton(opts, text="Re-extract source from FIRMWARE.md",
                        variable=self.extract).pack(side="left")
        ttk.Checkbutton(opts, text="Flash after building",
                        variable=self.do_flash).pack(side="left", padx=16)

        btns = ttk.Frame(self)
        btns.pack(fill="x", **pad)
        self.go_btn = ttk.Button(btns, text="Install and Flash", command=self.run_install)
        self.go_btn.pack(side="left")
        self.mon_btn = ttk.Button(btns, text="Serial Monitor", command=self.run_monitor)
        self.mon_btn.pack(side="left", padx=8)
        ttk.Button(btns, text="Close", command=self.destroy).pack(side="right")

        self.bar = ttk.Progressbar(self, mode="indeterminate")
        self.bar.pack(fill="x", padx=10)

        box = ttk.Frame(self)
        box.pack(fill="both", expand=True, **pad)
        self.log = tk.Text(box, wrap="word", height=18, bg="#111", fg="#ddd",
                           insertbackground="#ddd", font=("Consolas", 9))
        self.log.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(box, command=self.log.yview)
        sb.pack(side="right", fill="y")
        self.log.configure(yscrollcommand=sb.set)
        self.log.tag_config("ok", foreground="#7ddc7d")
        self.log.tag_config("err", foreground="#ff7b72")
        self.log.tag_config("warn", foreground="#e3c565")
        self.log.tag_config("step", foreground="#79c0ff")

        ttk.Label(self, textvariable=self.status, foreground="#666").pack(
            fill="x", padx=10, pady=(0, 8))

    def browse(self):
        chosen = filedialog.askdirectory(initialdir=self.project.get())
        if chosen:
            self.project.set(chosen)

    def refresh_ports(self):
        ports = list_ports()
        labels = ["{}  -  {}".format(p, name) for p, name, _ in ports]
        self.port_box["values"] = labels
        if labels:
            self.port_box.current(0)
        else:
            self.port.set("")
            self.say("No COM ports found. Plug the board in, then Refresh.", "warn")
            self.say("If it never appears, install the CH340 driver: " + DRIVER_URL, "warn")

    def selected_port(self):
        value = self.port.get()
        m = re.match(r"(COM\d+)", value)
        return m.group(1) if m else None

    # ---------------------------------------------------------------- log plumbing

    def say(self, text, tag=None):
        self.msgs.put(("log", text, tag))

    def _pump(self):
        try:
            while True:
                kind, text, tag = self.msgs.get_nowait()
                if kind == "log":
                    self.log.insert("end", text + "\n", tag or "")
                    self.log.see("end")
                elif kind == "status":
                    self.status.set(text)
                elif kind == "done":
                    self.bar.stop()
                    self.go_btn.state(["!disabled"])
                    self.worker = None
        except queue.Empty:
            pass
        self.after(100, self._pump)

    def run_cmd(self, cmd, cwd):
        self.say("$ " + " ".join(cmd))
        proc = subprocess.Popen(
            cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", bufsize=1,
            creationflags=CREATE_NO_WINDOW,
        )
        for line in proc.stdout:
            self.say(line.rstrip())
        proc.wait()
        return proc.returncode

    # ---------------------------------------------------------------- actions

    def run_install(self):
        if self.worker:
            return
        self.log.delete("1.0", "end")
        self.go_btn.state(["disabled"])
        self.bar.start(12)
        self.worker = threading.Thread(target=self._install, daemon=True)
        self.worker.start()

    def _install(self):
        try:
            self._install_steps()
        except Exception as exc:
            self.say("")
            self.say("FAILED: {}".format(exc), "err")
            self.msgs.put(("status", "Failed", None))
        finally:
            self.msgs.put(("done", "", None))

    def _install_steps(self):
        root = self.project.get()
        if not os.path.isdir(root):
            raise RuntimeError("Project folder does not exist: " + root)

        # --- source
        firmware = os.path.join(root, "FIRMWARE.md")
        if not os.path.isfile(firmware):
            packed = bundled_firmware()
            if not packed:
                raise RuntimeError("FIRMWARE.md not found in " + root)
            shutil.copyfile(packed, firmware)
            self.say("Wrote bundled FIRMWARE.md into the project folder", "ok")

        if self.extract.get():
            self.msgs.put(("status", "Extracting source", None))
            self.say("== Extracting source from FIRMWARE.md", "step")
            with open(firmware, encoding="utf-8") as fh:
                blocks = BLOCK_RE.findall(fh.read())
            cpp = [body for kind, body in blocks if kind == "cpp"]
            ini = [body for kind, body in blocks if kind == "ini"]
            if len(cpp) < 2 or len(ini) < 1:
                raise RuntimeError("FIRMWARE.md is missing the expected code blocks")
            for name, body in (("flock_sigs.h", cpp[0]),
                               ("flock-mini.ino", cpp[1]),
                               ("platformio.ini", ini[0])):
                path = os.path.join(root, name)
                if os.path.isfile(path) and name != "platformio.ini":
                    with open(path, encoding="utf-8") as fh:
                        if fh.read() != body:
                            shutil.copyfile(path, path + ".bak")
                            self.say("  saved previous " + name + " as " + name + ".bak", "warn")
                with open(path, "w", encoding="utf-8", newline="") as fh:
                    fh.write(body)
                self.say("  {} ({} lines)".format(name, body.count(chr(10))), "ok")
        else:
            self.say("== Using the source already on disk", "step")

        # --- platformio
        self.msgs.put(("status", "Checking PlatformIO", None))
        self.say("== Checking PlatformIO", "step")
        pio = pio_cmd()
        if pio is None:
            raise RuntimeError(
                "No Python interpreter found on PATH. Install Python from python.org "
                "and tick 'Add python.exe to PATH'."
            )
        if self.run_cmd(pio + ["--version"], root) != 0:
            self.say("  not installed, installing now (this takes a minute)", "warn")
            py = python_exe()
            if py is None:
                raise RuntimeError("Cannot install PlatformIO without Python on PATH")
            if self.run_cmd([py, "-m", "pip", "install", "--upgrade", "platformio"], root) != 0:
                raise RuntimeError("pip install platformio failed")
            pio = pio_cmd()
            if self.run_cmd(pio + ["--version"], root) != 0:
                raise RuntimeError("PlatformIO installed but will not run")
        self.say("  ok", "ok")

        # --- build
        self.msgs.put(("status", "Compiling", None))
        self.say("== Compiling", "step")
        self.say("  first run downloads the ESP8266 toolchain and U8g2, about 300 MB")
        if self.run_cmd(pio + ["run"], root) != 0:
            raise RuntimeError("Compile failed - the first error above is the real one")
        self.say("  build succeeded", "ok")

        # --- flash
        if not self.do_flash.get():
            self.msgs.put(("status", "Built, not flashed", None))
            self.say("== Done (compile only)", "step")
            return

        port = self.selected_port()
        if not port:
            self.say("No port selected, skipping flash.", "warn")
            self.say("Plug the board in and hit Refresh. Driver: " + DRIVER_URL, "warn")
            self.msgs.put(("status", "Built, not flashed", None))
            return

        self.msgs.put(("status", "Flashing " + port, None))
        self.say("== Flashing " + port, "step")
        if self.run_cmd(pio + ["run", "-t", "upload", "--upload-port", port], root) != 0:
            self.say("  Upload failed. Two things fix almost every case:", "warn")
            self.say("  1. Hold FLASH, tap RST, release FLASH, then try again.", "warn")
            self.say("  2. Weak USB power - use a rear port or a powered hub.", "warn")
            raise RuntimeError("Upload failed")

        self.say("  flashed", "ok")
        self.say("")
        self.say("Expect on serial: '[flock-mini] sniffing, 32 signatures'", "ok")
        self.say("Expect on screen: splash, then a scan screen cycling channels 1/6/11", "ok")
        self.msgs.put(("status", "Done", None))

    def run_monitor(self):
        port = self.selected_port()
        if not port:
            messagebox.showinfo(APP_TITLE, "Pick a COM port first.")
            return
        pio = pio_cmd()
        if pio is None:
            messagebox.showerror(APP_TITLE, "PlatformIO not available.")
            return
        # its own console window, so it stays interactive
        subprocess.Popen(
            pio + ["device", "monitor", "-b", "115200", "-p", port],
            cwd=self.project.get(), creationflags=CREATE_NEW_CONSOLE,
        )


if __name__ == "__main__":
    Installer().mainloop()
