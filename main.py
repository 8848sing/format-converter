# -*- coding: utf-8 -*-
"""main.py — 程序入口。开发期: python main.py；打包时作为 PyInstaller 主脚本。"""
import ctypes


def enable_high_dpi():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
        return
    except Exception:
        pass
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass


enable_high_dpi()

import os
import sys


def _make_streams_safe():
    """??? exe ??????sys.stdout/stderr ?????????
    --version / --self-test ?????????????????????"""
    for name in ("stdout", "stderr"):
        stream = getattr(sys, name, None)
        try:
            if stream is None:
                raise OSError("no console stream")
            stream.write("")
            stream.flush()
        except Exception:
            setattr(sys, name, open(os.devnull, "w", encoding="utf-8"))


if getattr(sys, "frozen", False):
    _make_streams_safe()

if '--version' in sys.argv:
    from version import version_text

    print(version_text())
    raise SystemExit(0)

if '--self-test' in sys.argv:
    from self_test import run_self_test
    raise SystemExit(run_self_test())

from ui import main

if __name__ == "__main__":
    main()
