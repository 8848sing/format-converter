# -*- coding: utf-8 -*-
"""sync_version.py — 一条命令同步所有版本号。

版本号的唯一来源是 `version.py` 的 `__version__`，其它位置都是它的副本：

```text
version.py                        __version__ = "1.7.0"
cpp/include/fc/version.h          #define FC_VERSION "1.7.0"
cpp/CMakeLists.txt                project(... VERSION 1.7.0 ...)
cpp/android/app/build.gradle.kts  versionName = "1.7.0"   （--android 才改）
```

用法：

```powershell
python sync_version.py            # 只同步桌面端（C++ 核心 + Python）
python sync_version.py --android  # 连 Android 的 versionName 一起同步
```

注意：Android 的 versionCode 需要手动 +1，脚本不会改它。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def read_version() -> str:
    text = (ROOT / "version.py").read_text(encoding="utf-8")
    match = re.search(r'__version__\s*=\s*"([^"]+)"', text)
    if not match:
        raise SystemExit("version.py 里没找到 __version__")
    return match.group(1)


def write(path: Path, text: str) -> None:
    # 统一用 UTF-8（无 BOM）+ LF 写回，避免中文被 GBK 破坏。
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print(f"  已更新 {path.relative_to(ROOT)}")


def update_version_h(version: str) -> None:
    path = ROOT / "cpp" / "include" / "fc" / "version.h"
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    text = re.sub(r'#define FC_VERSION "[^"]*"', f'#define FC_VERSION "{version}"', text)
    text = re.sub(
        r'#define FC_VERSION_TEXT "[^"]*"',
        f'#define FC_VERSION_TEXT "格式转换工具 v{version}"',
        text,
    )
    write(path, text)


def update_cmake(version: str) -> None:
    path = ROOT / "cpp" / "CMakeLists.txt"
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    text = re.sub(
        r"(project\(format_converter_core VERSION )[0-9][0-9.]*",
        rf"\g<1>{version}",
        text,
    )
    write(path, text)


def update_gradle(version: str) -> None:
    path = ROOT / "cpp" / "android" / "app" / "build.gradle.kts"
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    new_text = re.sub(r'versionName\s*=\s*"[^"]*"', f'versionName = "{version}"', text)
    if new_text == text:
        print("  build.gradle.kts 里没有 versionName，跳过")
        return
    write(path, new_text)


def main(argv: list[str]) -> int:
    version = read_version()
    print(f"版本号来源 version.py：{version}")
    update_version_h(version)
    update_cmake(version)
    if "--android" in argv:
        update_gradle(version)
        print("  （记得把 versionCode 手动 +1）")
    else:
        print("  Android 的 versionName 未改动；需要时加 --android")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))