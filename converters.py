# -*- coding: utf-8 -*-
"""converters.py — 桌面端的转换注册表。

转换实现已经迁移到 C++ 核心（cpp/，通过 fccli.exe 调用）。本模块保留原有
公开 API 与注册表结构，方便 ui.py / self_test.py / tests 继续使用：

* 优先：cpp_bridge.run_job() 调用 C++ 核心；
* 回退：找不到 fccli，或 settings.json 设置 "engine": "python" 时，
  调用 converters_legacy.py 中的旧实现。

因此 Python 只负责界面、队列和进度，不再承担主要转换逻辑。
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import cpp_bridge as _bridge
import converters_legacy as _legacy

# ---------------------------------------------------------------------------
# 兼容旧 API 的常量与异常
# ---------------------------------------------------------------------------
ConvertError = _legacy.ConvertError

CAT_MEDIA = _legacy.CAT_MEDIA
CAT_DATA = _legacy.CAT_DATA
CAT_IMG = _legacy.CAT_IMG
CAT_PDF = _legacy.CAT_PDF
CAT_WORD = _legacy.CAT_WORD
CAT_EXCEL = _legacy.CAT_EXCEL
CAT_PPT = _legacy.CAT_PPT
CAT_TEXT = _legacy.CAT_TEXT

IMAGE_EXTS = _legacy.IMAGE_EXTS
PDF_EXTS = _legacy.PDF_EXTS
WORD_EXTS = _legacy.WORD_EXTS
EXCEL_EXTS = _legacy.EXCEL_EXTS
PPT_EXTS = _legacy.PPT_EXTS
TEXT_EXTS = _legacy.TEXT_EXTS
MEDIA_EXTS = _legacy.MEDIA_EXTS
AUDIO_EXTS = _legacy.AUDIO_EXTS
VIDEO_EXTS = _legacy.VIDEO_EXTS
AUDIO_TARGETS = _legacy.AUDIO_TARGETS
VIDEO_TARGETS = _legacy.VIDEO_TARGETS
DATA_EXTS = _legacy.DATA_EXTS
TABLE_DATA_EXTS = _legacy.TABLE_DATA_EXTS
CATEGORY_EXTS = _legacy.CATEGORY_EXTS

# 旧版 Label → C++ 注册表 op id
_OP_BY_LABEL = {
    "图片格式互转": "image_convert",
    "图片 转 PDF（每张单独一个）": "image_to_pdf",
    "多张图片 合并为一个 PDF": "images_merge_pdf",
    "PDF 转图片（每页一张）": "pdf_to_image",
    "PDF 转 Word（可编辑 docx）": "pdf_to_word",
    "PDF 转 Excel": "pdf_to_excel",
    "PDF 转 PPT（整页图片版）": "pdf_to_ppt",
    "PDF 转 纯文本": "pdf_to_text",
    "Word 转 PDF": "word_to_pdf",
    "Word 转图片（每页一张）": "word_to_image",
    "Word 转 旧版/其他文档格式": "word_convert",
    "Word 转 纯文本": "word_to_text",
    "Excel 转 PDF": "excel_to_pdf",
    "Excel 转图片（每页一张）": "excel_to_image",
    "Excel 转 其他表格格式": "excel_convert",
    "PPT 转 PDF": "ppt_to_pdf",
    "PPT 转图片（每页一张）": "ppt_to_image",
    "PPT 转 旧版/其他演示格式": "ppt_convert",
    "文本 转 PDF": "text_to_pdf",
    "文本 转 Word 文档": "text_to_word",
    "多张图片 合成动图 GIF": "images_to_gif",
    "PDF 合并（多个文件 → 一个）": "pdf_merge",
    "PDF 拆分（一页一个文件）": "pdf_split",
    "PDF 旋转页面": "pdf_rotate",
    "PDF 压缩（内容流）": "pdf_compress",
    "音频转音频": "audio_convert",
    "视频转视频 / 提取音频": "video_convert",
    "结构化数据互转": "data_convert",
    "结构化数据 转 Excel": "data_to_excel",
    "Excel / CSV 转 JSON": "table_to_json",
}


@dataclass
class Conversion:
    label: str
    src: str
    dst: str
    extensions: tuple
    handler: object
    accepts: tuple = ()
    accepts_label: str = ""
    per_file: bool = True
    batch_report: bool = False
    workers: int = 1
    description: str = ""
    op: str = ""
    legacy_handler: object = None

    def run(self, files, out_dir, target="", report=None):
        mode = _bridge.engine_mode()
        if self.op and mode != "python":
            if _bridge.available():
                return _bridge.run_job(
                    self.op,
                    files,
                    Path(out_dir),
                    target,
                    report=report if self.batch_report else None,
                )
            if mode == "cpp":
                raise ConvertError("settings.json 指定 engine=cpp，但未找到 bin/fccli.exe")
        kwargs = {"target": target} if self.accepts else {}
        if self.batch_report and report is not None:
            kwargs["report"] = report
        handler = self.legacy_handler or self.handler
        return handler(files, Path(out_dir), **kwargs)

    def run_batch(
        self,
        files,
        out_dir,
        target="",
        options=None,
        on_file=None,
        cancel=None,
    ):
        """批量执行，逐文件回调 on_file(kind, message, input_path)。

        C++ 可用时整批交给一个 fccli 进程（进程内并发）；否则退回 Python 实现
        （串行逐文件，保证回退模式也能用）。
        """
        from cpp_bridge import BatchResult

        mode = _bridge.engine_mode()
        if self.op and mode != "python":
            if _bridge.available():
                return _bridge.run_batch(
                    self.op,
                    files,
                    Path(out_dir),
                    target,
                    options=options,
                    on_file=on_file,
                    cancel=cancel,
                )
            if mode == "cpp":
                raise ConvertError("settings.json 指定 engine=cpp，但未找到 bin/fccli.exe")

        result = BatchResult()
        out = Path(out_dir)
        handler = self.legacy_handler or self.handler
        kwargs = {"target": target} if self.accepts else {}

        if self.batch_report:
            def report(kind, text):
                if kind == "ok":
                    result.ok += 1
                else:
                    result.failed += 1
                    result.errors.append(text)
                if on_file is not None:
                    on_file(kind, text, None)

            handler(files, out, report=report, **kwargs)
            return result

        if not self.per_file:
            if cancel is not None and cancel.is_set():
                result.cancelled = True
                return result
            try:
                outs = handler(files, out, **kwargs)
                result.ok = 1
                result.outputs.extend(outs)
                if on_file is not None:
                    names = ", ".join(o.name for o in outs)
                    on_file("ok", f"生成 {names}", None)
            except ConvertError as exc:
                result.failed = 1
                result.errors.append(str(exc))
                if on_file is not None:
                    on_file("err", str(exc), None)
            return result

        for item in files:
            if cancel is not None and cancel.is_set():
                result.cancelled = True
                break
            try:
                outs = handler([item], out, **kwargs)
                result.ok += 1
                result.outputs.extend(outs)
                if on_file is not None:
                    names = ", ".join(o.name for o in outs)
                    on_file("ok", f"{item.name} → {names}", item)
            except ConvertError as exc:
                result.failed += 1
                result.errors.append(str(exc))
                if on_file is not None:
                    on_file("err", f"{item.name}：{exc}", item)
            except Exception as exc:  # noqa: BLE001 - 单文件失败不应中断整批
                result.failed += 1
                result.errors.append(f"{item.name}: {exc}")
                if on_file is not None:
                    on_file("err", f"{item.name}：{exc}", item)
        return result


def _build_registry() -> list[Conversion]:
    result: list[Conversion] = []
    for spec in _legacy.CONVERSIONS:
        result.append(
            Conversion(
                label=spec.label,
                src=spec.src,
                dst=spec.dst,
                extensions=spec.extensions,
                handler=spec.handler,
                accepts=spec.accepts,
                accepts_label=spec.accepts_label,
                per_file=spec.per_file,
                batch_report=spec.batch_report,
                workers=spec.workers,
                description=spec.description,
                op=_OP_BY_LABEL.get(spec.label, ""),
                legacy_handler=spec.handler,
            )
        )
    return result


CONVERSIONS: list[Conversion] = _build_registry()


def categories_src() -> list[str]:
    seen: list[str] = []
    for conv in CONVERSIONS:
        if conv.src not in seen:
            seen.append(conv.src)
    return seen


def dsts_for(src: str) -> list[str]:
    seen: list[str] = []
    for conv in CONVERSIONS:
        if conv.src == src and conv.dst not in seen:
            seen.append(conv.dst)
    return seen


def convs_for(src: str, dst: str) -> list[Conversion]:
    return [conv for conv in CONVERSIONS if conv.src == src and conv.dst == dst]


def pair_exists(src: str, dst: str) -> bool:
    return bool(convs_for(src, dst))


def engine_info() -> dict:
    """给界面/自检显示当前使用的引擎。"""
    path = _bridge.cli_path()
    from version import __version__

    return {
        "mode": _bridge.engine_mode(),
        "available": _bridge.available(),
        "cli": str(path) if path else "",
        "version": __version__,
        "core_version": _bridge.core_version() if path else "",
    }


def __getattr__(name):
    """未显式导出的旧函数/常量继续转发到 legacy 实现。"""
    return getattr(_legacy, name)