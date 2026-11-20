# -*- coding: utf-8 -*-
"""settings_store.py — settings.json 的读写与界面偏好记忆。

桌面端的配置集中放在程序旁边的 `settings.json`：

```json
{
  "engine": "auto",          // auto / cpp / python
  "ffmpeg": "",              // 留空则自动找 tools/FFmpeg/ffmpeg.exe
  "soffice": "",             // 留空则自动找 tools/LibreOffice/program/soffice.exe
  "ui": {
    "quality": 92,           // 图片输出质量（JPG / WebP）
    "dpi": 200,              // PDF / Office 转图片的渲染 DPI
    "last_src": "图片",
    "last_dst": "图片",
    "last_mode": "图片格式互转",
    "geometry": "1400x940+0+0",
    "remember_output": true
  }
}
```

写入时只更新后端关心的键，保留用户手动加的其他字段；写坏文件也不会让程序崩溃。
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def base_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def settings_path() -> Path:
    return base_dir() / "settings.json"


def load() -> dict:
    path = settings_path()
    if not path.exists():
        return {}
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
        if isinstance(value, dict):
            return value
    except (OSError, ValueError):
        pass
    return {}


def save(data: dict) -> bool:
    """原子写入 settings.json；失败时返回 False 而不是抛异常。"""
    path = settings_path()
    payload = json.dumps(data, ensure_ascii=False, indent=2, sort_keys=False)
    tmp = path.with_suffix(path.suffix + ".tmp")
    try:
        tmp.write_text(payload + "\n", encoding="utf-8")
        os.replace(tmp, path)
        return True
    except OSError:
        try:
            tmp.unlink(missing_ok=True)
        except OSError:
            pass
        return False


def update(**changes) -> dict:
    """浅合并顶层键后写回，返回合并后的完整配置。"""
    data = load()
    data.update({k: v for k, v in changes.items() if v is not None})
    save(data)
    return data


def ui_prefs() -> dict:
    value = load().get("ui")
    return value if isinstance(value, dict) else {}


def update_ui(**changes) -> None:
    data = load()
    ui = data.get("ui")
    if not isinstance(ui, dict):
        ui = {}
    ui.update({k: v for k, v in changes.items() if v is not None})
    data["ui"] = ui
    save(data)