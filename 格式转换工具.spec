# -*- mode: python ; coding: utf-8 -*-
import os

from PyInstaller.utils.hooks import collect_data_files
from PyInstaller.utils.hooks import collect_all

datas = []
binaries = []
hiddenimports = []

# C++ 核心（可选）：存在时随包分发，Python 界面通过 cpp_bridge 调用。
for _name in ('fccli.exe', 'pdfium.dll'):
    _path = os.path.join('bin', _name)
    if os.path.exists(_path):
        binaries.append((_path, 'bin'))
datas += collect_data_files('pdfminer')
datas += collect_data_files('pdfplumber')
datas += collect_data_files('ttkbootstrap')

# 排除仅交互式环境/开发工具才会用到的包，减小体积（界面与转换流程不会导入）。
# 重点：cv2 / numpy / pymupdf 是旧 Python 回退里 pdf2docx 的依赖，
# 解压后约 170 MB（压缩后约 70 MB），而 PDF→Word 已由 C++ 核心接管，
# 全部排除后 exe 从 127 MB 降到约 50 MB，首次启动解压时间也降到三分之一。
EXCLUDES = [
    'cv2', 'numpy', 'fitz', 'pymupdf', 'pymupdf_fonts', 'pdf2docx',
    'IPython', 'jedi', 'parso', 'pygments', 'traitlets', 'wcwidth',
    'matplotlib', 'pandas', 'scipy', 'sympy',
    'sqlite3', 'requests', 'urllib3', 'certifi', 'idna', 'brotli', 'brotlicffi',
    'lib2to3', 'pydoc_data', 'tkinter.test', 'test', 'pytest', '_pytest',
    'nbformat', 'nbconvert', 'notebook', 'jupyter', 'jupyter_client',
    'jupyter_core', 'zmq', 'tornado', 'ipyparallel',
]

for package in ('pypdfium2', 'tkinterdnd2', 'pillow_heif', 'yaml', 'pypdf'):
    tmp_ret = collect_all(package)
    datas += tmp_ret[0]
    binaries += tmp_ret[1]
    hiddenimports += tmp_ret[2]

a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=EXCLUDES,
    noarchive=False,
    optimize=1,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='格式转换工具',
    icon='app_icon.ico',
    # 声明 per-monitor DPI 感知：否则 125%/150% 缩放下窗口会被系统拉伸，
    # 界面发虚（v1.6 的 exe 就是这个状态）。
    manifest='dpi_aware.manifest',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)