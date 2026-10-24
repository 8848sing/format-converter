# -*- coding: utf-8 -*-
"""version.py — 版本号的唯一来源。"""

__version__ = "1.4.0"
APP_NAME = "格式转换工具"


def version_text() -> str:
    return f"{APP_NAME} v{__version__}"