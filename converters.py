# -*- coding: utf-8 -*-
"""
converters.py — 所有转换功能的实现与注册表。
- 纯 Python 库：Pillow / pypdfium2 / pdf2docx / pdfplumber / python-pptx / openpyxl
- 外部工具：LibreOffice（soffice.exe），统一放在便携包的 tools/ 目录，可用 settings.json 覆盖路径
所有函数失败时抛出 ConvertError，调用方逐文件捕获，保证单个失败不中断批次。
"""
from __future__ import annotations

import csv
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from io import BytesIO, StringIO
from pathlib import Path


class ConvertError(RuntimeError):
    """转换失败的统一异常。"""


# ---------------------------------------------------------------------------
# 路径与配置
# ---------------------------------------------------------------------------

def base_dir() -> Path:
    """便携包根目录：打包后为 exe 所在目录，开发期为源码目录。"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


TOOLS_DIR = base_dir() / "tools"
SETTINGS_FILE = base_dir() / "settings.json"


def load_settings() -> dict:
    p = SETTINGS_FILE
    if p.exists():
        try:
            return json.loads(p.read_text(encoding="utf-8"))
        except Exception:
            return {}
    return {}


_SOFFICE_CACHE: Path | None = None


def find_soffice(refresh: bool = False) -> Path:
    """查找结果进程内缓存；settings.json 修改后需重启程序生效。"""
    global _SOFFICE_CACHE
    if _SOFFICE_CACHE is not None and not refresh:
        return _SOFFICE_CACHE
    _SOFFICE_CACHE = _discover_soffice()
    return _SOFFICE_CACHE


def _discover_soffice() -> Path:
    """
    查找 LibreOffice 的 soffice.exe：
    1. settings.json 中的 "soffice" 绝对/相对路径
    2. 便携包 tools/ 目录下的 soffice.exe（LibreOffice / LibreOfficePortable / 任意子目录）
    3. 系统 PATH 与常见安装位置（兜底，方便开发调试）
    """
    cfg = load_settings().get("soffice")
    if cfg:
        p = Path(cfg)
        if not p.is_absolute():
            p = base_dir() / p
        if p.exists():
            return p

    if TOOLS_DIR.exists():
        patterns = [
            "LibreOffice*/program/soffice.exe",
            "*/program/soffice.exe",
            "*/soffice.exe",
            "LibreOffice*/program/soffice",
            "*/program/soffice",
        ]
        for pat in patterns:
            for hit in sorted(TOOLS_DIR.glob(pat)):
                if hit.exists():
                    return hit
        # 最后来一次深度扫描兜底：仅接受路径中含 'libreoffice' 的候选，
        # 避免命中 tools/ 下随便放置的同名 exe（误放/U 盘残留/被替换）。
        for hit in sorted(TOOLS_DIR.rglob("soffice.exe")):
            if "libreoffice" in str(hit).lower():
                return hit

    which = shutil.which("soffice") or shutil.which("soffice.exe")
    if which:
        return Path(which)

    for cand in (
        Path(r"C:\Program Files\LibreOffice\program\soffice.exe"),
        Path(r"C:\Program Files (x86)\LibreOffice\program\soffice.exe"),
    ):
        if cand.exists():
            return cand

    raise ConvertError(
        "未找到 LibreOffice（soffice.exe）。请将其便携版解压到便携包的 tools/ 目录，"
        "或在程序根目录的 settings.json 中写入 {\"soffice\": \"绝对路径\"}。详见 tools/README.md。"
    )


_OUTPUT_LOCK = threading.Lock()
_OUTPUT_CLAIMS: set[str] = set()


def unique_path(path: Path) -> Path:
    """同名输出文件已存在时自动加 _1/_2 后缀。

    并行转换时多个线程可能同时输出同名文件，这里对“检查 + 占位”加锁，
    并把已分配但尚未落盘的路径登记到内存，避免并行任务互相覆盖。
    """
    path = Path(path)
    key = os.path.normcase(os.path.abspath(str(path)))
    with _OUTPUT_LOCK:
        if not path.exists() and key not in _OUTPUT_CLAIMS:
            _OUTPUT_CLAIMS.add(key)
            return path
        i = 1
        while True:
            cand = path.with_name(f"{path.stem}_{i}{path.suffix}")
            k = os.path.normcase(os.path.abspath(str(cand)))
            if not cand.exists() and k not in _OUTPUT_CLAIMS:
                _OUTPUT_CLAIMS.add(k)
                return cand
            i += 1


# ---------------------------------------------------------------------------
# 图片类转换（Pillow）
# ---------------------------------------------------------------------------

IMAGE_EXTS = (
    '.jpg', '.jpeg', '.png', '.webp', '.bmp', '.tif', '.tiff', '.gif',
    '.ico', '.avif', '.heic', '.heif', '.ppm', '.pgm', '.tga',
)


_IMAGE_PLUGIN_LOCK = threading.Lock()
_IMAGE_PLUGINS_READY = False


def _ensure_image_plugins(path: Path | None = None):
    """注册 HEIC/HEIF 等 Pillow 插件；并行调用时只执行一次。"""
    global _IMAGE_PLUGINS_READY
    if _IMAGE_PLUGINS_READY:
        return
    with _IMAGE_PLUGIN_LOCK:
        if _IMAGE_PLUGINS_READY:
            return
        try:
            import pillow_heif
            pillow_heif.register_heif_opener()
        except ImportError:
            if path is not None and path.suffix.lower() in ('.heic', '.heif'):
                raise ConvertError('缺少 HEIC/HEIF 支持，请重新安装完整版本。')
        except Exception:
            pass
        _IMAGE_PLUGINS_READY = True


def _open_image(path: Path):
    from PIL import Image  # 延迟导入，加快 GUI 启动

    _ensure_image_plugins(path)

    try:
        return Image.open(str(path))
    except Exception as e:
        raise ConvertError(f"无法读取图片 {path.name}: {e}") from e


def _flatten_for_save(img, fmt: str):
    from PIL import Image

    fmt = fmt.upper()
    if fmt in ("JPEG", "JPG", "PDF", "TIFF", "BMP"):
        # 这些格式不支持 alpha / 调色板透明
        if img.mode not in ("RGB", "L", "CMYK"):
            bg = Image.new("RGB", img.size, (255, 255, 255))
            if img.mode == "RGBA" or "transparency" in img.info:
                mask = img.convert("RGBA")
                bg.paste(mask, mask=mask.split()[-1])
            else:
                bg.paste(img)
            img = bg
    return img


def convert_images_to_pdf(files, out_dir: Path) -> list[Path]:
    """每张图片单独生成一个同名 PDF（逐文件调用）。"""
    outs = []
    for f in files:
        img = _flatten_for_save(_open_image(f), "PDF")
        out = unique_path(Path(out_dir) / (f.stem + ".pdf"))
        try:
            img.save(str(out), "PDF", resolution=150.0)
        except Exception as e:
            raise ConvertError(f"{f.name} 转 PDF 失败: {e}") from e
        outs.append(out)
    return outs


def merge_images_to_pdf(files, out_dir: Path) -> list[Path]:
    """多张图片合并为一个 PDF，取第一张的名字。"""
    if not files:
        raise ConvertError("没有可合并的图片")
    imgs = [_flatten_for_save(_open_image(f), "PDF") for f in files]
    first, rest = imgs[0], imgs[1:]
    out = unique_path(Path(out_dir) / (Path(files[0]).stem + "-合并.pdf" if len(files) > 1 else Path(files[0]).stem + ".pdf"))
    try:
        first.save(str(out), "PDF", save_all=True, append_images=rest, resolution=150.0)
    except Exception as e:
        raise ConvertError(f"合并 PDF 失败: {e}") from e
    return [out]


# ---------------------------------------------------------------------------
# 图片格式互转 / PDF 转图片（pypdfium2，纯 pip 安装，无需 Poppler）
# ---------------------------------------------------------------------------

def _image_target_info(target: str):
    target = target.lower()
    aliases = {'jpg': 'JPEG', 'jpeg': 'JPEG', 'tif': 'TIFF', 'tiff': 'TIFF', 'heic': 'HEIF', 'heif': 'HEIF'}
    fmt = aliases.get(target, target.upper())
    suffix = '.jpg' if fmt == 'JPEG' else ('.tif' if fmt == 'TIFF' else '.' + target)
    return fmt, suffix


def convert_images_format_plus(files, out_dir: Path, target: str = 'png') -> list[Path]:
    from PIL import Image

    _ensure_image_plugins()

    target = target.lower()
    allowed = {'jpg', 'jpeg', 'png', 'webp', 'bmp', 'tif', 'tiff', 'gif', 'ico', 'avif', 'heic', 'heif', 'ppm', 'pgm', 'tga'}
    if target not in allowed:
        raise ConvertError(f'不支持的图片目标格式：{target}')
    fmt, suffix = _image_target_info(target)
    outs: list[Path] = []

    for f in files:
        try:
            with _open_image(f) as source:
                source.seek(0)
                img = source.copy()
            if fmt in ('JPEG', 'BMP'):
                img = _flatten_for_save(img, fmt)
            if fmt == 'ICO':
                img = img.convert('RGBA')
                img.thumbnail((256, 256), Image.Resampling.LANCZOS)
                canvas = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
                canvas.alpha_composite(img, ((256 - img.width) // 2, (256 - img.height) // 2))
                img = canvas

            out = unique_path(Path(out_dir) / (f.stem + suffix))
            kwargs = {}
            if fmt == 'JPEG':
                kwargs = {'quality': 94, 'subsampling': 0, 'optimize': True}
            elif fmt == 'WEBP':
                kwargs = {'quality': 93, 'method': 6}
            elif fmt == 'AVIF':
                kwargs = {'quality': 80}
            elif fmt == 'HEIF':
                kwargs = {'quality': 85}
            elif fmt == 'PNG':
                kwargs = {'optimize': True}
            elif fmt == 'TIFF':
                kwargs = {'compression': 'tiff_lzw'}
            elif fmt == 'GIF':
                if img.mode not in ('P', 'L'):
                    img = img.convert('P', palette=Image.Palette.ADAPTIVE, colors=256)
                kwargs = {'optimize': True}
            elif fmt == 'ICO':
                kwargs = {'sizes': [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]}
            img.save(str(out), fmt, **kwargs)
            outs.append(out)
        except ConvertError:
            raise
        except Exception as e:
            raise ConvertError(f'{f.name} 转 {target.upper()} 失败: {e}') from e
    return outs


def images_to_animated_gif(files, out_dir: Path, duration_ms: int = 600) -> list[Path]:
    from PIL import Image, ImageOps

    if not files:
        raise ConvertError('没有可合成 GIF 的图片')
    try:
        frames = []
        for f in files:
            with _open_image(f) as src:
                img = ImageOps.exif_transpose(src).convert('RGBA')
                img.thumbnail((1280, 1280), Image.Resampling.LANCZOS)
                frames.append(img.copy())
        width = max(im.width for im in frames)
        height = max(im.height for im in frames)
        paletted = []
        for im in frames:
            canvas = Image.new('RGBA', (width, height), (0, 0, 0, 0))
            canvas.alpha_composite(im, ((width - im.width) // 2, (height - im.height) // 2))
            paletted.append(canvas.convert('P', palette=Image.Palette.ADAPTIVE, colors=256))
        out = unique_path(Path(out_dir) / (Path(files[0]).stem + '-动图.gif'))
        paletted[0].save(str(out), 'GIF', save_all=True, append_images=paletted[1:], duration=max(80, int(duration_ms)), loop=0, disposal=2, optimize=True)
        return [out]
    except Exception as e:
        raise ConvertError(f'合成 GIF 失败: {e}') from e


def merge_pdfs(files, out_dir: Path) -> list[Path]:
    from pypdf import PdfReader, PdfWriter

    if not files:
        raise ConvertError('没有可合并的 PDF')
    writer = PdfWriter()
    try:
        for f in files:
            reader = PdfReader(str(f))
            for page in reader.pages:
                writer.add_page(page)
        name = f'{Path(files[0]).stem}-合并.pdf' if len(files) > 1 else f'{Path(files[0]).stem}-副本.pdf'
        out = unique_path(Path(out_dir) / name)
        with out.open('wb') as fh:
            writer.write(fh)
        return [out]
    except Exception as e:
        raise ConvertError(f'合并 PDF 失败: {e}') from e
    finally:
        writer.close()


def split_pdf(files, out_dir: Path) -> list[Path]:
    from pypdf import PdfReader, PdfWriter

    outs: list[Path] = []
    for f in files:
        try:
            reader = PdfReader(str(f))
            for i, page in enumerate(reader.pages, 1):
                writer = PdfWriter()
                writer.add_page(page)
                out = unique_path(Path(out_dir) / f'{f.stem}-第{i}页.pdf')
                with out.open('wb') as fh:
                    writer.write(fh)
                writer.close()
                outs.append(out)
        except Exception as e:
            raise ConvertError(f'{f.name} 拆分失败: {e}') from e
    return outs


def rotate_pdf(files, out_dir: Path, target: str = '90') -> list[Path]:
    from pypdf import PdfReader, PdfWriter

    angle = int(target)
    if angle not in (90, 180, 270):
        raise ConvertError('旋转角度只支持 90 / 180 / 270')
    outs: list[Path] = []
    for f in files:
        try:
            reader = PdfReader(str(f))
            writer = PdfWriter()
            for page in reader.pages:
                page.rotate(angle)
                writer.add_page(page)
            out = unique_path(Path(out_dir) / f'{f.stem}-旋转{angle}.pdf')
            with out.open('wb') as fh:
                writer.write(fh)
            writer.close()
            outs.append(out)
        except Exception as e:
            raise ConvertError(f'{f.name} 旋转失败: {e}') from e
    return outs


def compress_pdf(files, out_dir: Path) -> list[Path]:
    from pypdf import PdfReader, PdfWriter

    outs: list[Path] = []
    for f in files:
        try:
            reader = PdfReader(str(f))
            writer = PdfWriter()
            for page in reader.pages:
                writer.add_page(page)
            # 逐页压缩内容流，再合并重复对象；只处理一遍，避免重复遍历。
            for page in writer.pages:
                page.compress_content_streams()
            writer.compress_identical_objects()
            out = unique_path(Path(out_dir) / f'{f.stem}-压缩.pdf')
            with out.open('wb') as fh:
                writer.write(fh)
            writer.close()
            outs.append(out)
        except Exception as e:
            raise ConvertError(f'{f.name} 压缩失败: {e}') from e
    return outs

def convert_pdf_to_images(files, out_dir: Path, target: str = "png", dpi: int = 200) -> list[Path]:
    import pypdfium2 as pdfium

    fmt = target.upper()
    if fmt in ("JPG",):
        fmt = "JPEG"
    outs = []
    for f in files:
        try:
            pdf = pdfium.PdfDocument(str(f))
        except Exception as e:
            raise ConvertError(f"无法打开 PDF {f.name}: {e}") from e
        try:
            for i in range(len(pdf)):
                page = pdf[i]
                bmp = page.render(scale=dpi / 72.0)
                img = bmp.to_pil()
                img = _flatten_for_save(img, fmt)
                suffix = ".jpg" if fmt == "JPEG" else "." + fmt.lower()
                out = unique_path(Path(out_dir) / f"{f.stem}-第{i + 1}页{suffix}")
                img.save(str(out), fmt)
                outs.append(out)
        except Exception as e:
            raise ConvertError(f"{f.name} 转图片失败: {e}") from e
        finally:
            pdf.close()
    return outs


# ---------------------------------------------------------------------------
# Office → PDF（LibreOffice headless）
# ---------------------------------------------------------------------------

AUDIO_EXTS = ('.mp3', '.wav', '.flac', '.m4a', '.aac', '.ogg', '.opus', '.wma', '.aiff', '.amr')
VIDEO_EXTS = ('.mp4', '.m4v', '.mkv', '.mov', '.webm', '.avi', '.wmv', '.flv', '.ts', '.mts', '.m2ts', '.3gp')
MEDIA_EXTS = AUDIO_EXTS + VIDEO_EXTS
AUDIO_TARGETS = ('mp3', 'wav', 'flac', 'm4a', 'aac', 'ogg', 'opus', 'wma')
VIDEO_TARGETS = ('mp4', 'm4v', 'mkv', 'mov', 'webm', 'avi', 'wmv', 'gif')

_FFMPEG_CACHE: Path | None = None


def find_ffmpeg(refresh: bool = False) -> Path:
    global _FFMPEG_CACHE
    if _FFMPEG_CACHE is not None and not refresh:
        return _FFMPEG_CACHE
    cfg = load_settings().get('ffmpeg')
    candidates = []
    if cfg:
        p = Path(cfg)
        candidates.append(p if p.is_absolute() else base_dir() / p)
    candidates.extend([
        TOOLS_DIR / 'FFmpeg' / 'ffmpeg.exe',
        TOOLS_DIR / 'ffmpeg' / 'ffmpeg.exe',
        TOOLS_DIR / 'ffmpeg.exe',
    ])
    which = shutil.which('ffmpeg') or shutil.which('ffmpeg.exe')
    if which:
        candidates.append(Path(which))
    for p in candidates:
        if p.exists() and p.is_file():
            _FFMPEG_CACHE = p.resolve()
            return _FFMPEG_CACHE
    raise ConvertError('未找到 FFmpeg。请确认 tools/FFmpeg/ffmpeg.exe 存在，或在 settings.json 中配置 ffmpeg 路径。')


def _media_codec_args(target: str) -> list[str]:
    if target == 'mp3':
        return ['-vn', '-c:a', 'libmp3lame', '-q:a', '2']
    if target == 'wav':
        return ['-vn', '-c:a', 'pcm_s16le']
    if target == 'flac':
        return ['-vn', '-c:a', 'flac', '-compression_level', '8']
    if target == 'm4a':
        return ['-vn', '-c:a', 'aac', '-b:a', '192k']
    if target == 'aac':
        return ['-vn', '-c:a', 'aac', '-b:a', '192k']
    if target == 'ogg':
        return ['-vn', '-c:a', 'libvorbis', '-q:a', '5']
    if target == 'opus':
        return ['-vn', '-c:a', 'libopus', '-b:a', '128k']
    if target == 'wma':
        return ['-vn', '-c:a', 'wmav2', '-b:a', '192k']
    if target in ('mp4', 'm4v', 'mkv', 'mov'):
        args = ['-c:v', 'libopenh264', '-b:v', '3500k', '-c:a', 'aac', '-b:a', '192k']
        if target in ('mp4', 'm4v', 'mov'):
            args += ['-movflags', '+faststart']
        return args
    if target == 'webm':
        return ['-c:v', 'libvpx-vp9', '-crf', '32', '-b:v', '0', '-c:a', 'libopus', '-b:a', '128k']
    if target == 'avi':
        return ['-c:v', 'mpeg4', '-q:v', '4', '-c:a', 'libmp3lame', '-q:a', '4']
    if target == 'wmv':
        return ['-c:v', 'wmv2', '-b:v', '3000k', '-c:a', 'wmav2', '-b:a', '192k']
    raise ConvertError(f'不支持的音视频目标格式：{target}')


def _run_ffmpeg_file(ffmpeg: Path, source: Path, out: Path, target: str):
    temp = out.with_name(out.stem + '.part' + out.suffix)
    if target == 'gif':
        codec_args = ['-an', '-vf', 'fps=12,scale=960:-2:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=256[p];[s1][p]paletteuse=dither=sierra2_4a']
    else:
        codec_args = _media_codec_args(target)
    cmd = [str(ffmpeg), '-hide_banner', '-y', '-i', str(source)] + codec_args + [str(temp)]
    try:
        result = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=7200, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        if result.returncode != 0:
            detail = (result.stderr or result.stdout or b'').decode('utf-8', errors='replace')
            lines = [line for line in detail.strip().splitlines() if line.strip()]
            raise ConvertError(f'{source.name} 转换失败：{lines[-1] if lines else result.returncode}')
        if not temp.exists() or temp.stat().st_size == 0:
            raise ConvertError(f'{source.name} 转换失败：FFmpeg 未生成输出文件')
        os.replace(temp, out)
    except subprocess.TimeoutExpired as e:
        raise ConvertError(f'{source.name} 转换超时（超过 2 小时）') from e
    finally:
        temp.unlink(missing_ok=True)


def convert_media(files, out_dir: Path, target: str = 'mp3') -> list[Path]:
    target = target.lower()
    if target not in AUDIO_TARGETS + VIDEO_TARGETS:
        raise ConvertError(f'不支持的音视频目标格式：{target}')
    ffmpeg = find_ffmpeg()
    outs: list[Path] = []
    for f in files:
        out = unique_path(Path(out_dir) / (f.stem + '.' + target))
        try:
            _run_ffmpeg_file(ffmpeg, f, out, target)
            outs.append(out)
        except ConvertError:
            raise
        except Exception as e:
            raise ConvertError(f'{f.name} 调用 FFmpeg 失败：{e}') from e
    return outs

OFFICE_INPUT_EXTS = {
    "word": (".doc", ".docx", ".rtf", ".odt", ".txt"),
    "excel": (".xls", ".xlsx", ".csv", ".ods"),
    "ppt": (".ppt", ".pptx", ".odp"),
}


def _run_soffice_batch(soffice: Path, profile_dir: Path, files: list[Path],
                       out_tmp: Path, timeout: int, target: str = "pdf"):
    """一次 soffice 进程转换多个文件；soffice --convert-to 原生支持多输入。
    target 为 LibreOffice 目标过滤串，如 "pdf" 或 "docx:MS Word 2007 XML"。"""
    cmd = [
        str(soffice),
        f"-env:UserInstallation={profile_dir.as_uri()}",
        "--headless", "--norestore", "--invisible",
        "--convert-to", target,
        "--outdir", str(out_tmp),
    ] + [str(f) for f in files]
    try:
        return subprocess.run(
            cmd, capture_output=True, timeout=timeout,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except subprocess.TimeoutExpired:
        raise ConvertError(f"转换超时（>{timeout}s）")
    except OSError as e:
        raise ConvertError(f"无法启动 LibreOffice: {e}")


def _plan_office_batches(files: list[Path], max_cmd_chars: int = 6000) -> list[list[Path]]:
    """
    切分批次：
    1. LibreOffice 输出名 = 源文件 stem + .pdf，同一批（同一 --outdir）内 stem 不能重复，
       重名的文件自动分到下一批；
    2. Windows 命令行长度上限约 8191 字符，按累计长度切批。
    """
    batches: list[list[Path]] = []
    cur: list[Path] = []
    seen_stems: set[str] = set()
    cur_len = 0
    for f in files:
        key = f.stem.lower()
        est = len(str(f)) + 3
        if key in seen_stems or cur_len + est > max_cmd_chars:
            if cur:
                batches.append(cur)
            cur, seen_stems, cur_len = [], set(), 0
        cur.append(f)
        seen_stems.add(key)
        cur_len += est
    if cur:
        batches.append(cur)
    return batches


def office_convert(files, out_dir: Path, out_ext: str = ".pdf",
                   conv_target: str = "pdf", report=None) -> list[Path]:
    """
    Office → 指定格式（PDF / Word / ...）。整批文件合并给尽量少的 soffice 进程
    （每次启动约需数秒，批量转换合并启动后可大幅提速）。
    out_ext 为输出扩展名（含点，如 ".pdf" / ".docx"）；conv_target 为 LibreOffice
    的 --convert-to 过滤串。report(kind, text) 回调逐文件上报 "ok"/"err"；未提供回调
    时，任何失败汇总抛出 ConvertError（兼容单文件调用方）。
    """
    soffice = find_soffice()
    out_dir = Path(out_dir)
    emit = report if report is not None else (lambda kind, text: None)
    errors: list[str] = []
    outs: list[Path] = []
    pending: list[Path] = []

    def _move_out(produced: Path, src: Path) -> Path:
        out = unique_path(out_dir / (src.stem + out_ext))
        shutil.move(str(produced), str(out))
        return out

    def _collect(batch_tmp: Path, batch: list[Path]) -> None:
        for f in batch:
            produced = batch_tmp / (f.stem + out_ext)
            if not produced.exists():
                pending.append(f)
                continue
            try:
                out = _move_out(produced, f)
            except OSError as e:
                msg = f"{f.name} 写入输出目录失败: {e}"
                errors.append(msg)
                emit("err", msg)
                continue
            outs.append(out)
            emit("ok", f"{f.name} → {out.name}")

    for batch in _plan_office_batches(list(files)):
        # 每次启动使用独立 profile：不写用户目录/注册表，且超时被杀后
        # 残留的 profile 锁不会传染给后续批次。ignore_cleanup_errors 防止
        # 孤儿 soffice.bin 短暂占用目录时抛清理异常。
        try:
            with tempfile.TemporaryDirectory(prefix="lo_profile_", ignore_cleanup_errors=True) as prof, \
                 tempfile.TemporaryDirectory(prefix="lo_out_", ignore_cleanup_errors=True) as tmp:
                timeout = min(1800, 180 + 90 * len(batch))
                try:
                    _run_soffice_batch(soffice, Path(prof), batch, Path(tmp), timeout, conv_target)
                except ConvertError:
                    # 整批失败（超时/无法启动）：逐文件重试，错误隔离
                    pending.extend(batch)
                    continue
                _collect(Path(tmp), batch)
        except OSError:
            pending.extend(batch)

    # 批处理里缺失输出的文件逐个重试（换独立临时目录，排除同 stem 竞争后仍失败才是真失败）
    for f in pending:
        try:
            with tempfile.TemporaryDirectory(prefix="lo_profile_", ignore_cleanup_errors=True) as prof, \
                 tempfile.TemporaryDirectory(prefix="lo_out_", ignore_cleanup_errors=True) as tmp:
                proc = _run_soffice_batch(soffice, Path(prof), [f], Path(tmp), 300, conv_target)
                produced = Path(tmp) / (f.stem + out_ext)
                if not produced.exists():
                    detail = (proc.stderr or proc.stdout or b"").decode("utf-8", "ignore")[:300]
                    raise ConvertError(
                        f"soffice 退出码 {proc.returncode}：{detail}" if detail
                        else f"soffice 退出码 {proc.returncode}")
                out = _move_out(produced, f)
                outs.append(out)
                emit("ok", f"{f.name} → {out.name}")
        except ConvertError as e:
            msg = f"{f.name} 转换失败: {e}"
            errors.append(msg)
            emit("err", msg)
        except OSError as e:
            msg = f"{f.name} 转换失败: {e}"
            errors.append(msg)
            emit("err", msg)

    if report is None and errors:
        raise ConvertError("；".join(errors))
    return outs


def office_to_pdf(files, out_dir: Path, target: str = "pdf",
                  report=None) -> list[Path]:
    """Office → PDF（兼容旧接口）。"""
    return office_convert(files, out_dir, ".pdf", "pdf", report=report)


def convert_doc_to_word(files, out_dir: Path, target: str = "docx",
                        report=None) -> list[Path]:
    """
    Word / RTF / ODT / TXT → Word（.docx）。
    由内置 LibreOffice 完成；源文件已是 .docx 时也重新导出为 .docx（会生成 _1 去重）。
    """
    return office_convert(files, out_dir, ".docx", "docx:MS Word 2007 XML", report=report)


# ---------------------------------------------------------------------------
# PDF → Word（pdf2docx）
# ---------------------------------------------------------------------------

def convert_pdf_to_word(files, out_dir: Path) -> list[Path]:
    outs = []
    for f in files:
        try:
            from pdf2docx import Converter
        except Exception as e:
            raise ConvertError(f"pdf2docx 未正确安装: {e}") from e
        out = unique_path(Path(out_dir) / (f.stem + ".docx"))
        cv = None
        try:
            cv = Converter(str(f))
            cv.convert(str(out))
            cv.close()
            cv = None
        except Exception as e:
            if cv is not None:
                try:
                    cv.close()
                except Exception:
                    pass
            if out.exists():
                out.unlink(missing_ok=True)
            raise ConvertError(f"{f.name} 转 Word 失败: {e}") from e
        outs.append(out)
    return outs


# ---------------------------------------------------------------------------
# PDF → Excel（pdfplumber 提取表格，openpyxl 写入）
# ---------------------------------------------------------------------------

def convert_pdf_to_excel(files, out_dir: Path) -> list[Path]:
    import pdfplumber
    from openpyxl import Workbook

    outs = []
    for f in files:
        out = unique_path(Path(out_dir) / (f.stem + ".xlsx"))
        try:
            wb = Workbook()
            wb.remove(wb.active)
            any_data = False
            with pdfplumber.open(str(f)) as pdf:
                for idx, page in enumerate(pdf.pages, start=1):
                    ws = wb.create_sheet(title=f"第{idx}页"[:31])
                    tables = page.extract_tables() or []
                    if tables:
                        t = tables[0]
                        for row in t:
                            ws.append(["" if c is None else str(c) for c in row])
                        if len(tables) > 1:
                            ws.append(["---- 本页还有 %d 个其他表格，未合并 ----" % (len(tables) - 1)])
                        any_data = True
                    else:
                        text = page.extract_text() or ""
                        for line in text.splitlines():
                            ws.append([line])
                        any_data = any_data or bool(text.strip())
            if not any_data:
                raise ConvertError("未提取到任何表格或文本")
            wb.save(str(out))
        except ConvertError:
            if out.exists():
                out.unlink(missing_ok=True)
            raise
        except Exception as e:
            if out.exists():
                out.unlink(missing_ok=True)
            raise ConvertError(f"{f.name} 转 Excel 失败: {e}") from e
        outs.append(out)
    return outs


# ---------------------------------------------------------------------------
# PDF → PPT（每页渲染为整页幻灯片图片，python-pptx）
# ---------------------------------------------------------------------------

def convert_pdf_to_ppt(files, out_dir: Path, dpi: int = 150) -> list[Path]:
    import pypdfium2 as pdfium
    from pptx import Presentation
    from pptx.util import Inches

    outs = []
    for f in files:
        out = unique_path(Path(out_dir) / (f.stem + ".pptx"))
        try:
            pdf = pdfium.PdfDocument(str(f))
            try:
                prs = Presentation()
                prs.slide_width = Inches(13.333)
                prs.slide_height = Inches(7.5)
                blank = prs.slide_layouts[6]
                for i in range(len(pdf)):
                    # 图片走内存流，不落盘（无需 .pages 临时目录，少一倍磁盘 IO）
                    page = pdf[i]
                    img = page.render(scale=dpi / 72.0).to_pil()
                    buf = BytesIO()
                    img.save(buf, "PNG")
                    buf.seek(0)
                    slide = prs.slides.add_slide(blank)
                    # 按原页面比例等比缩放并居中，避免 A4 / 4:3 页面被拉伸变形。
                    box_w, box_h = prs.slide_width, prs.slide_height
                    scale = min(box_w / img.width, box_h / img.height)
                    pic_w, pic_h = int(img.width * scale), int(img.height * scale)
                    slide.shapes.add_picture(
                        buf, (box_w - pic_w) // 2, (box_h - pic_h) // 2,
                        width=pic_w, height=pic_h,
                    )
                prs.save(str(out))
            finally:
                pdf.close()
        except Exception as e:
            if out.exists():
                out.unlink(missing_ok=True)
            raise ConvertError(f"{f.name} 转 PPT 失败: {e}") from e
        outs.append(out)
    return outs


# ---------------------------------------------------------------------------
# Office 双向互转：通用导出器（LibreOffice 滤镜映射，均已实测）
# ---------------------------------------------------------------------------

WORD_EXPORTS = {
    "pdf": ("pdf", ".pdf"),
    "docx": ("docx:MS Word 2007 XML", ".docx"),
    "doc": ("doc:MS Word 97", ".doc"),
    "odt": ("odt:writer8", ".odt"),
    "rtf": ("rtf", ".rtf"),
    "txt": ("txt:Text (encoded):UTF8", ".txt"),
}
EXCEL_EXPORTS = {
    "pdf": ("pdf", ".pdf"),
    "xlsx": ("xlsx:Calc MS Excel 2007 XML", ".xlsx"),
    "xls": ("xls:MS Excel 97", ".xls"),
    "ods": ("ods:calc8", ".ods"),
    "csv": ("csv:Text - txt - csv (StarCalc)", ".csv"),
}
PPT_EXPORTS = {
    "pdf": ("pdf", ".pdf"),
    "pptx": ("pptx:Impress MS PowerPoint 2007 XML", ".pptx"),
    "ppt": ("ppt:MS PowerPoint 97", ".ppt"),
    "odp": ("odp:impress8", ".odp"),
}
TEXT_EXPORTS = {
    "pdf": ("pdf", ".pdf"),
    "docx": ("docx:MS Word 2007 XML", ".docx"),
    "doc": ("doc:MS Word 97", ".doc"),
    "odt": ("odt:writer8", ".odt"),
    "rtf": ("rtf", ".rtf"),
}


def office_export(filters: dict, default: str):
    """生成 Office/文本 → 指定格式的处理器；target 为目标扩展名（不带点）。"""
    def _h(files, out_dir: Path, target: str = "", report=None) -> list[Path]:
        ext = (target or default).lower().lstrip(".")
        if ext not in filters:
            ext = default
        filt, out_ext = filters[ext]
        return office_convert(files, Path(out_dir), out_ext, filt, report=report)
    return _h


def convert_office_to_images(files, out_dir: Path, target: str = "png") -> list[Path]:
    """Word/Excel/PPT → 图片：先转 PDF 再逐页渲染（每页一张，全页覆盖）。"""
    outs = []
    for f in files:
        with tempfile.TemporaryDirectory(prefix="o2i_") as td:
            pdfs = office_convert([f], Path(td), ".pdf", "pdf")
            if not pdfs:
                raise ConvertError(f"{f.name} 无法生成中间 PDF")
            outs += convert_pdf_to_images(pdfs, Path(out_dir), target)
    return outs


# ---------------------------------------------------------------------------
# PDF → 纯文本（pdfplumber 提取）
# ---------------------------------------------------------------------------

def convert_pdf_to_text(files, out_dir: Path) -> list[Path]:
    import pdfplumber

    outs = []
    for f in files:
        out = unique_path(Path(out_dir) / (f.stem + ".txt"))
        try:
            with pdfplumber.open(str(f)) as pdf:
                parts = [(p.extract_text() or "") for p in pdf.pages]
            text = "\n".join(parts)
            if not text.strip():
                raise ConvertError(f"{f.name} 未提取到文本（可能是扫描件）")
            out.write_text(text, encoding="utf-8")
        except ConvertError:
            if out.exists():
                out.unlink(missing_ok=True)
            raise
        except Exception as e:
            if out.exists():
                out.unlink(missing_ok=True)
            raise ConvertError(f"{f.name} 转文本失败: {e}") from e
        outs.append(out)
    return outs


# ---------------------------------------------------------------------------
# 注册表：GUI 直接读取
# ---------------------------------------------------------------------------

def _read_text_auto(path: Path) -> str:
    data = path.read_bytes()
    for enc in ('utf-8-sig', 'gb18030'):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            continue
    return data.decode('utf-8', errors='replace')


def _xml_value(elem):
    result = {('@' + k): v for k, v in elem.attrib.items()}
    children = list(elem)
    text = (elem.text or '').strip()
    if not children:
        if result:
            if text:
                result['#text'] = text
            return result
        return text
    grouped = {}
    for child in children:
        grouped.setdefault(child.tag, []).append(_xml_value(child))
    for key, values in grouped.items():
        result[key] = values[0] if len(values) == 1 else values
    if text:
        result['#text'] = text
    return result


def _load_structured(path: Path):
    ext = path.suffix.lower()
    if ext == '.json':
        return json.loads(_read_text_auto(path))
    if ext in ('.yaml', '.yml'):
        import yaml
        return yaml.safe_load(_read_text_auto(path))
    if ext == '.xml':
        root = ET.fromstring(_read_text_auto(path))
        return {root.tag: _xml_value(root)}
    raise ConvertError(f'不支持的结构化数据格式：{ext}')


def _jsonable(value):
    if isinstance(value, dict):
        return {str(k): _jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_jsonable(v) for v in value]
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    return str(value)


def _flatten_record(record, prefix: str = '') -> dict:
    row = {}
    if not isinstance(record, dict):
        return {'value': record}
    for key, value in record.items():
        name = f'{prefix}.{key}' if prefix else str(key)
        if isinstance(value, dict):
            row.update(_flatten_record(value, name))
        elif isinstance(value, list):
            row[name] = json.dumps(_jsonable(value), ensure_ascii=False)
        else:
            row[name] = value
    return row


def _tabular_rows(data) -> list[dict]:
    if isinstance(data, list):
        return [_flatten_record(item) for item in data]
    if isinstance(data, dict):
        list_values = [v for v in data.values() if isinstance(v, list)]
        if len(data) == 1 and list_values:
            return [_flatten_record(item) for item in list_values[0]]
        if data and all(isinstance(v, list) for v in data.values()):
            length = max(len(v) for v in data.values())
            return [{k: (v[i] if i < len(v) else None) for k, v in data.items()} for i in range(length)]
        return [_flatten_record(data)]
    return [{'value': data}]


def _append_xml_value(parent, key: str, value):
    if isinstance(value, list):
        for item in value:
            _append_xml_value(parent, key, item)
        return
    child = ET.SubElement(parent, str(key))
    if isinstance(value, dict):
        for k, v in value.items():
            k = str(k)
            if k.startswith('@'):
                child.set(k[1:], str(v))
            elif k == '#text':
                child.text = str(v)
            else:
                _append_xml_value(child, k, v)
    elif value is not None:
        child.text = str(value)


def _data_to_xml(data):
    if isinstance(data, dict) and len(data) == 1:
        root_name, root_value = next(iter(data.items()))
    else:
        root_name, root_value = 'root', data
    root = ET.Element(str(root_name))
    if isinstance(root_value, dict):
        for key, value in root_value.items():
            _append_xml_value(root, str(key), value)
    elif isinstance(root_value, list):
        for item in root_value:
            _append_xml_value(root, 'item', item)
    elif root_value is not None:
        root.text = str(root_value)
    ET.indent(root, space='  ')
    return ET.tostring(root, encoding='utf-8', xml_declaration=True)


def convert_structured(files, out_dir: Path, target: str = 'json') -> list[Path]:
    from openpyxl import Workbook

    target = target.lower()
    suffix = '.xlsx' if target == 'xlsx' else '.' + target
    outs: list[Path] = []
    for f in files:
        try:
            data = _jsonable(_load_structured(f))
            out = unique_path(Path(out_dir) / (f.stem + suffix))
            if target == 'json':
                out.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8')
            elif target == 'yaml':
                import yaml
                out.write_text(yaml.safe_dump(data, allow_unicode=True, sort_keys=False), encoding='utf-8')
            elif target == 'xml':
                out.write_bytes(_data_to_xml(data))
            elif target == 'xlsx':
                rows = _tabular_rows(data)
                wb = Workbook()
                ws = wb.active
                ws.title = '数据'
                headers = []
                for row in rows:
                    for key in row:
                        if key not in headers:
                            headers.append(key)
                headers = headers or ['value']
                ws.append(headers)
                for row in rows:
                    ws.append([row.get(k) for k in headers])
                ws.freeze_panes = 'A2'
                ws.auto_filter.ref = ws.dimensions
                for col in ws.columns:
                    width = min(48, max(10, max(len(str(c.value or '')) for c in col) + 2))
                    ws.column_dimensions[col[0].column_letter].width = width
                wb.save(out)
            else:
                raise ConvertError(f'不支持的数据目标格式：{target}')
            outs.append(out)
        except ConvertError:
            raise
        except Exception as e:
            raise ConvertError(f'{f.name} 转 {target.upper()} 失败: {e}') from e
    return outs


def convert_table_to_json(files, out_dir: Path) -> list[Path]:
    from openpyxl import load_workbook

    outs: list[Path] = []
    for f in files:
        try:
            if f.suffix.lower() == '.csv':
                reader = csv.DictReader(StringIO(_read_text_auto(f)))
                rows = [dict(row) for row in reader]
            else:
                wb = load_workbook(str(f), read_only=True, data_only=True)
                ws = wb.active
                values = list(ws.iter_rows(values_only=True))
                wb.close()
                if not values:
                    rows = []
                else:
                    headers = [str(v) if v is not None else f'列{i + 1}' for i, v in enumerate(values[0])]
                    rows = [dict(zip(headers, row)) for row in values[1:]]
            out = unique_path(Path(out_dir) / (f.stem + '.json'))
            out.write_text(json.dumps(rows, ensure_ascii=False, indent=2, default=str), encoding='utf-8')
            outs.append(out)
        except Exception as e:
            raise ConvertError(f'{f.name} 转 JSON 失败: {e}') from e
    return outs

@dataclass
class Conversion:
    label: str                      # 同一 源→目标 组合下的方式名
    src: str                        # 源格式类别
    dst: str                        # 目标格式类别
    extensions: tuple               # 允许的输入扩展名
    handler: object                 # run(files, out_dir, target) -> list[Path]
    accepts: tuple = ()             # 额外参数白名单
    accepts_label: str = ""         # 子选项下拉的标题
    per_file: bool = True           # True=逐文件调用（单个失败不影响其他）
    batch_report: bool = False      # True=一次传入全部文件，handler 通过 report 回调逐文件上报
    workers: int = 1                # 逐文件模式下的并行线程数；1=顺序执行
    description: str = ""

    def run(self, files, out_dir, target="", report=None):
        kwargs = {"target": target} if self.accepts else {}
        if self.batch_report and report is not None:
            kwargs["report"] = report
        return self.handler(files, Path(out_dir), **kwargs)


def _only_exts(files, exts):
    return [f for f in files if f.suffix.lower() in exts]


# 格式类别（UI 下拉用）与每类输入扩展名
CAT_MEDIA = '音视频'
CAT_DATA = '数据'
CAT_IMG = "图片"
CAT_PDF = "PDF"
CAT_WORD = "Word"
CAT_EXCEL = "Excel"
CAT_PPT = "PPT"
CAT_TEXT = "文本"

PDF_EXTS = (".pdf",)
WORD_EXTS = (".doc", ".docx", ".rtf", ".odt")
EXCEL_EXTS = (".xls", ".xlsx", ".ods", ".csv")
PPT_EXTS = (".ppt", ".pptx", ".odp")
TEXT_EXTS = (".txt", ".md")

CATEGORY_EXTS = {
    CAT_IMG: IMAGE_EXTS,
    CAT_PDF: PDF_EXTS,
    CAT_WORD: WORD_EXTS,
    CAT_EXCEL: EXCEL_EXTS,
    CAT_PPT: PPT_EXTS,
    CAT_TEXT: TEXT_EXTS,
}

_IMG_FMTS = ('png', 'jpg', 'webp', 'avif', 'heic', 'ico', 'tiff', 'bmp', 'gif', 'tga', 'ppm')
DATA_EXTS = ('.json', '.yaml', '.yml', '.xml')
TABLE_DATA_EXTS = ('.xlsx', '.csv')
CATEGORY_EXTS.update({CAT_MEDIA: MEDIA_EXTS, CAT_DATA: DATA_EXTS})

CONVERSIONS: list[Conversion] = [
    # ── 图片 → ──
    Conversion(
        label="图片 转 PDF（每张单独一个）", src=CAT_IMG, dst=CAT_PDF,
        extensions=IMAGE_EXTS,
        handler=lambda files, out: convert_images_to_pdf(files, out),
        workers=4,
        description="Pillow 实现，每张图生成一个同名 PDF，纯 Python。",
    ),
    Conversion(
        label="多张图片 合并为一个 PDF", src=CAT_IMG, dst=CAT_PDF,
        extensions=IMAGE_EXTS,
        handler=lambda files, out: merge_images_to_pdf(files, out),
        per_file=False,
        description="按文件列表顺序合并为一个多页 PDF。",
    ),
    # ── PDF → ──
    Conversion(
        label="PDF 转图片（每页一张）", src=CAT_PDF, dst=CAT_IMG,
        extensions=PDF_EXTS, handler=convert_pdf_to_images,
        accepts=_IMG_FMTS, accepts_label="目标格式", workers=2,
        description="pypdfium2 渲染，200 DPI，纯 pip 依赖。",
    ),
    Conversion(
        label="PDF 转 Word（可编辑 docx）", src=CAT_PDF, dst=CAT_WORD,
        extensions=PDF_EXTS,
        handler=lambda files, out: convert_pdf_to_word(files, out),
        description="pdf2docx 实现，保留段落/表格；扫描件 PDF 无法还原。",
    ),
    Conversion(
        label="PDF 转 Excel", src=CAT_PDF, dst=CAT_EXCEL,
        extensions=PDF_EXTS,
        handler=lambda files, out: convert_pdf_to_excel(files, out),
        description="pdfplumber 提取表格逐页写入工作表；无表格时导出文本行。",
    ),
    Conversion(
        label="PDF 转 PPT（整页图片版）", src=CAT_PDF, dst=CAT_PPT,
        extensions=PDF_EXTS,
        handler=lambda files, out: convert_pdf_to_ppt(files, out),
        description="每页渲染为全页图片插入幻灯片，文本不可编辑。",
    ),
    Conversion(
        label="PDF 转 纯文本", src=CAT_PDF, dst=CAT_TEXT,
        extensions=PDF_EXTS,
        handler=lambda files, out: convert_pdf_to_text(files, out),
        description="pdfplumber 提取文字层，输出 UTF-8 .txt；扫描件无文字层时为空。",
    ),
    # ── Word → ──
    Conversion(
        label="Word 转 PDF", src=CAT_WORD, dst=CAT_PDF,
        extensions=WORD_EXTS, handler=office_export(WORD_EXPORTS, "pdf"),
        per_file=False, batch_report=True,
        description="内置 LibreOffice；整批共用进程启动，批量转换显著提速。",
    ),
    Conversion(
        label="Word 转图片（每页一张）", src=CAT_WORD, dst=CAT_IMG,
        extensions=WORD_EXTS, handler=convert_office_to_images,
        accepts=_IMG_FMTS, accepts_label="目标格式",
        description="先转 PDF 再逐页渲染，保留分页。",
    ),
    Conversion(
        label="Word 转 旧版/其他文档格式", src=CAT_WORD, dst=CAT_WORD,
        extensions=WORD_EXTS, handler=office_export(WORD_EXPORTS, "docx"),
        accepts=("docx", "doc", "odt", "rtf"), accepts_label="目标格式",
        description="doc ↔ docx / odt / rtf 任意互转，内置 LibreOffice。",
    ),
    Conversion(
        label="Word 转 纯文本", src=CAT_WORD, dst=CAT_TEXT,
        extensions=WORD_EXTS, handler=office_export(WORD_EXPORTS, "txt"),
        per_file=False, batch_report=True,
        description="导出 UTF-8 文本，格式信息丢失。",
    ),
    # ── Excel → ──
    Conversion(
        label="Excel 转 PDF", src=CAT_EXCEL, dst=CAT_PDF,
        extensions=EXCEL_EXTS, handler=office_export(EXCEL_EXPORTS, "pdf"),
        per_file=False, batch_report=True,
        description="内置 LibreOffice；整批共用进程启动，批量转换显著提速。",
    ),
    Conversion(
        label="Excel 转图片（每页一张）", src=CAT_EXCEL, dst=CAT_IMG,
        extensions=EXCEL_EXTS, handler=convert_office_to_images,
        accepts=_IMG_FMTS, accepts_label="目标格式",
        description="先转 PDF 再逐页渲染。打印区域外的内容可能不被包含。",
    ),
    Conversion(
        label="Excel 转 其他表格格式", src=CAT_EXCEL, dst=CAT_EXCEL,
        extensions=EXCEL_EXTS, handler=office_export(EXCEL_EXPORTS, "xlsx"),
        accepts=("xlsx", "xls", "ods", "csv"), accepts_label="目标格式",
        description="xls ↔ xlsx / ods / csv 任意互转，内置 LibreOffice。",
    ),
    # ── PPT → ──
    Conversion(
        label="PPT 转 PDF", src=CAT_PPT, dst=CAT_PDF,
        extensions=PPT_EXTS, handler=office_export(PPT_EXPORTS, "pdf"),
        per_file=False, batch_report=True,
        description="内置 LibreOffice；整批共用进程启动，批量转换显著提速。",
    ),
    Conversion(
        label="PPT 转图片（每页一张）", src=CAT_PPT, dst=CAT_IMG,
        extensions=PPT_EXTS, handler=convert_office_to_images,
        accepts=_IMG_FMTS, accepts_label="目标格式",
        description="先转 PDF 再逐页渲染，每张幻灯片一页图。",
    ),
    Conversion(
        label="PPT 转 旧版/其他演示格式", src=CAT_PPT, dst=CAT_PPT,
        extensions=PPT_EXTS, handler=office_export(PPT_EXPORTS, "pptx"),
        accepts=("pptx", "ppt", "odp"), accepts_label="目标格式",
        description="ppt ↔ pptx / odp 任意互转，内置 LibreOffice。",
    ),
    # ── 文本 → ──
    Conversion(
        label="文本 转 PDF", src=CAT_TEXT, dst=CAT_PDF,
        extensions=TEXT_EXTS, handler=office_export(TEXT_EXPORTS, "pdf"),
        per_file=False, batch_report=True,
        description="txt / md 直接排版输出 PDF，内置 LibreOffice。",
    ),
    Conversion(
        label="文本 转 Word 文档", src=CAT_TEXT, dst=CAT_WORD,
        extensions=TEXT_EXTS, handler=office_export(TEXT_EXPORTS, "docx"),
        accepts=("docx", "doc", "odt", "rtf"), accepts_label="目标格式",
        description="txt / md 转为 Word 系列文档，内置 LibreOffice。",
    ),
    # 文本→Excel：LibreOffice 把 .txt 当作 Writer 文档打开，无法可靠导入表格；
    # csv 输入统一走「Excel 转 其他表格格式」（csv 属于 Excel 类别扩展名）。
]


CONVERSIONS.insert(0, Conversion(
    label='图片格式互转', src=CAT_IMG, dst=CAT_IMG,
    extensions=IMAGE_EXTS, handler=convert_images_format_plus,
    accepts=_IMG_FMTS, accepts_label='目标格式', workers=4,
    description='支持 JPG / PNG / WebP / AVIF / HEIC / ICO / TIFF / GIF 等；JPEG 采用高质量 4:4:4 编码。',
))

CONVERSIONS.extend([
    Conversion(
        label='多张图片 合成动图 GIF', src=CAT_IMG, dst=CAT_IMG,
        extensions=IMAGE_EXTS, handler=images_to_animated_gif,
        per_file=False,
        description='按文件队列顺序合成循环 GIF，最长边限制为 1280 像素。',
    ),
    Conversion(
        label='PDF 合并（多个文件 → 一个）', src=CAT_PDF, dst=CAT_PDF,
        extensions=PDF_EXTS, handler=merge_pdfs, per_file=False,
        description='按文件队列顺序合并全部 PDF，加密或有损坏的页面可能失败。',
    ),
    Conversion(
        label='PDF 拆分（一页一个文件）', src=CAT_PDF, dst=CAT_PDF,
        extensions=PDF_EXTS, handler=split_pdf,
        description='每个 PDF 的每一页导出为独立文件。',
    ),
    Conversion(
        label='PDF 旋转页面', src=CAT_PDF, dst=CAT_PDF,
        extensions=PDF_EXTS, handler=rotate_pdf,
        accepts=('90', '180', '270'), accepts_label='旋转角度',
        description='顺时针旋转全部页面，不改变原始内容。',
    ),
    Conversion(
        label='PDF 压缩（内容流）', src=CAT_PDF, dst=CAT_PDF,
        extensions=PDF_EXTS, handler=compress_pdf,
        description='压缩 PDF 内容流并合并重复对象；不会重新压缩已嵌入的高清图片。',
    ),
])

CONVERSIONS.extend([
    Conversion(
        label='音频转音频', src=CAT_MEDIA, dst=CAT_MEDIA,
        extensions=AUDIO_EXTS, handler=convert_media,
        accepts=AUDIO_TARGETS, accepts_label='目标格式', workers=2,
        description='使用内置 FFmpeg 转 MP3 / WAV / FLAC / M4A / AAC / OGG / OPUS / WMA。',
    ),
    Conversion(
        label='视频转视频 / 提取音频', src=CAT_MEDIA, dst=CAT_MEDIA,
        extensions=VIDEO_EXTS, handler=convert_media,
        accepts=VIDEO_TARGETS + AUDIO_TARGETS, accepts_label='目标格式', workers=2,
        description='支持 MP4 / MKV / MOV / WebM / AVI / WMV / GIF，也可选音频格式提取音轨。',
    ),
    Conversion(
        label='结构化数据互转', src=CAT_DATA, dst=CAT_DATA,
        extensions=DATA_EXTS, handler=convert_structured,
        accepts=('json', 'yaml', 'xml', 'xlsx'), accepts_label='目标格式', workers=4,
        description='JSON / YAML / XML 互转，也可把结构化数据展开为 Excel 表格。',
    ),
    Conversion(
        label='结构化数据 转 Excel', src=CAT_DATA, dst=CAT_EXCEL,
        extensions=DATA_EXTS, handler=convert_structured,
        accepts=('xlsx',), accepts_label='目标格式', workers=4,
        description='自动展开对象字段，嵌套字段使用点号命名；数组转为多行。',
    ),
    Conversion(
        label='Excel / CSV 转 JSON', src=CAT_EXCEL, dst=CAT_DATA,
        extensions=TABLE_DATA_EXTS, handler=convert_table_to_json, workers=2,
        description='读取当前工作表首行作为字段名，输出 UTF-8 JSON 数组。',
    ),
])


def categories_src() -> list[str]:
    """按注册表出现顺序返回所有源类别。"""
    seen: list[str] = []
    for c in CONVERSIONS:
        if c.src not in seen:
            seen.append(c.src)
    return seen


def dsts_for(src: str) -> list[str]:
    seen: list[str] = []
    for c in CONVERSIONS:
        if c.src == src and c.dst not in seen:
            seen.append(c.dst)
    return seen


def convs_for(src: str, dst: str) -> list[Conversion]:
    return [c for c in CONVERSIONS if c.src == src and c.dst == dst]


def pair_exists(src: str, dst: str) -> bool:
    return bool(convs_for(src, dst))


