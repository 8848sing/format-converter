# -*- mode: python ; coding: utf-8 -*-
from PyInstaller.utils.hooks import collect_data_files
from PyInstaller.utils.hooks import collect_all

datas = []
binaries = []
hiddenimports = []
datas += collect_data_files('pdfminer')
datas += collect_data_files('pdfplumber')
datas += collect_data_files('ttkbootstrap')

# 排除仅交互式环境/开发工具才会用到的包，减小体积（界面与转换流程不会导入）。
EXCLUDES = [
    'IPython', 'jedi', 'parso', 'pygments', 'traitlets', 'wcwidth',
    'matplotlib', 'pandas', 'scipy', 'sympy',
    'sqlite3', 'requests', 'urllib3', 'certifi', 'idna', 'brotli', 'brotlicffi',
    'lib2to3', 'pydoc_data', 'tkinter.test', 'test', 'pytest', '_pytest',
    'nbformat', 'nbconvert', 'notebook', 'jupyter', 'jupyter_client',
    'jupyter_core', 'zmq', 'tornado', 'ipyparallel',
]

for package in ('pdf2docx', 'pypdfium2', 'tkinterdnd2', 'pillow_heif', 'yaml', 'pypdf'):
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