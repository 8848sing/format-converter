# -*- coding: utf-8 -*-
"""界面冒烟：真实创建窗口、跑一次批量转换、截图留证。"""
import sys
import tempfile
import time
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# 和 main.py 一样先声明 DPI 感知，否则截图坐标会被系统虚拟化，和窗口内容对不上。
try:
    import ctypes

    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from PIL import Image, ImageDraw  # noqa: E402

import ui  # noqa: E402

TMP = Path(tempfile.mkdtemp(prefix="fc_ui_"))
FILES = []
for i in range(5):
    p = TMP / f"界面测试 {i}.png"
    img = Image.new("RGB", (900 + i * 40, 600), (40 + i * 30, 110, 210 - i * 25))
    draw = ImageDraw.Draw(img)
    draw.rectangle([40, 40, 300, 200], fill=(255, 210 - i * 20, 70))
    draw.text((60, 260), f"UI smoke {i}", fill="white")
    img.save(p)
    FILES.append(p)

app = ui.App()
app.update()

app._add_paths(FILES)
app.cmb_src.set("图片")
app._on_src_change()
app.cmb_dst.set("图片")
app._on_dst_change()
# 选「图片格式互转」
labels = list(app.cmb_mode["values"])
print("方式:", labels)
if "图片格式互转" in labels:
    app.cmb_mode.set("图片格式互转")
    app._on_mode_change()
opts = list(app.cmb_opt["values"])
print("目标格式:", opts)
if "WEBP" in opts:
    app.cmb_opt.set("WEBP")
elif opts:
    app.cmb_opt.current(0)

out = TMP / "输出"
app._auto_out = False
app.ent_out.delete(0, "end")
app.ent_out.insert(0, str(out))

print("引擎徽标:", app.engine_btn._text)
print("队列行数:", len(app.tree.get_children()))

app.start_convert()
deadline = time.time() + 120
while time.time() < deadline:
    app.update()
    if app.worker and not app.worker.is_alive():
        break
    time.sleep(0.05)
for _ in range(60):
    app.update()
    time.sleep(0.02)

rows = [app.tree.item(iid, "values") for iid in app.tree.get_children()]
print("状态列:", [row[1] for row in rows])
print("输出文件:", sorted(p.name for p in out.glob('*'))[:8])
print("日志尾部:")
print("\n".join(app.txt_log.get("1.0", "end").strip().splitlines()[-4:]))

app.update_idletasks()
app.lift()
app.attributes("-topmost", True)
app.update()
time.sleep(0.6)
app.update()
x, y = app.winfo_rootx(), app.winfo_rooty()
w, h = app.winfo_width(), app.winfo_height()
try:
    import ctypes
    from ctypes import wintypes

    hwnd = int(app.wm_frame(), 16)
    rect = wintypes.RECT()
    ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(rect))
    bbox = (rect.left, rect.top, rect.right, rect.bottom)
    shot = ROOT / "docs" / "ui-1.7.0-main.png"
    shot.parent.mkdir(exist_ok=True)
    ImageGrab = __import__("PIL.ImageGrab", fromlist=["grab"]).grab
    ImageGrab(bbox=bbox).save(shot)
    print(f"截图: {shot} {shot.stat().st_size} bbox={bbox}")
except Exception as exc:  # noqa: BLE001
    print("截图失败:", exc)

ok = all(str(row[1]).startswith("✔") for row in rows)
print("界面冒烟:", "PASS" if ok and len(rows) == len(FILES) else "FAIL")
app.destroy()
raise SystemExit(0 if ok else 1)
