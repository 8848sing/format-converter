"""C++ 核心桥接层。

桌面界面仍然使用 Python/Tkinter，但所有转换都优先交给 `fccli.exe`
（C++ 核心）执行；找不到可执行文件或 settings.json 指定 engine=python 时，
退回 converters_legacy.py 的纯 Python 实现。

任务格式与 C++ 核心的 C API 一致：
    {"op": "...", "inputs": [...], "out_dir": "...", "target": "...", "options": {...}}

两种执行方式：

* `run_job()`   一次调用、执行完才返回（自检和旧调用方使用）；
* `run_batch()` 流式执行：边跑边把每个文件的结果回调给界面，支持中途取消。

`run_batch` 用一个常驻 `fccli` 进程跑完整批文件，由 C++ Engine 在进程内做有界并发：
以前「每个文件起一个进程」在几十张小图上会白白花掉几秒的进程启动时间。
"""
from __future__ import annotations

import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Callable

CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0

# 连续多久没有任何事件就认为核心卡死（秒）。大批量任务只要求「有进展」，
# 不限制整批总时长，避免几千张图因为总和超时被误杀。
DEFAULT_IDLE_TIMEOUT = 300.0
# Office 依赖 LibreOffice：正常情况下单个文件几十秒内一定会有事件，
# 出现长时间静默基本是 soffice 启动失败/被杀毒拦截，早点报错比干等有用。
OFFICE_IDLE_TIMEOUT = 180.0
# 音视频转码可能长时间没有回调（ffmpeg 在跑一个长视频），给足时间。
MEDIA_IDLE_TIMEOUT = 1800.0

_version_cache: dict[str, str] = {}


def base_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def _load_settings() -> dict:
    try:
        import settings_store

        return settings_store.load()
    except Exception:
        path = base_dir() / "settings.json"
        if path.exists():
            try:
                value = json.loads(path.read_text(encoding="utf-8"))
                return value if isinstance(value, dict) else {}
            except Exception:
                return {}
        return {}


def engine_mode() -> str:
    """auto / cpp / python"""
    value = os.environ.get("FC_ENGINE", "").strip().lower()
    if value in ("auto", "cpp", "python"):
        return value
    value = str(_load_settings().get("engine", "auto")).strip().lower()
    return value if value in ("auto", "cpp", "python") else "auto"


def _cli_candidates() -> list[Path]:
    override = os.environ.get("FC_CLI", "").strip()
    root = base_dir()
    candidates = []
    if override:
        candidates.append(Path(override))
    if getattr(sys, "frozen", False) and getattr(sys, "_MEIPASS", None):
        candidates.append(Path(sys._MEIPASS) / "bin" / "fccli.exe")
        candidates.append(Path(sys._MEIPASS) / "bin" / "fccli")
    candidates.extend(
        [
            root / "bin" / "fccli.exe",
            root / "bin" / "fccli",
            root / "fccli.exe",
            root / "cpp" / "build" / "fccli.exe",
            root / "cpp" / "build" / "fccli",
        ]
    )
    return candidates


def cli_path() -> Path | None:
    for candidate in _cli_candidates():
        try:
            if candidate.is_file():
                return candidate.resolve()
        except OSError:
            continue
    found = shutil.which("fccli")
    return Path(found).resolve() if found else None


def available() -> bool:
    return engine_mode() != "python" and cli_path() is not None


def core_version() -> str:
    """C++ 核心的版本号（用于界面显示），失败返回空串。"""
    exe = cli_path()
    if exe is None:
        return ""
    key = str(exe)
    if key in _version_cache:
        return _version_cache[key]
    text = ""
    try:
        proc = subprocess.run(
            [str(exe), "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10,
            creationflags=CREATE_NO_WINDOW,
        )
        text = proc.stdout.decode("utf-8", errors="replace").strip()
    except (OSError, subprocess.SubprocessError):
        text = ""
    _version_cache[key] = text
    return text


def idle_timeout_for(op: str) -> float:
    """按转换类型给出「多久没有事件就算卡死」的阈值。"""
    if op in ("audio_convert", "video_convert"):
        return MEDIA_IDLE_TIMEOUT
    if op.startswith(("word_", "excel_", "ppt_", "text_")):
        return OFFICE_IDLE_TIMEOUT
    return DEFAULT_IDLE_TIMEOUT


def _build_job(op, files, out_dir, target, options) -> bytes:
    job = {
        "op": op,
        "inputs": [str(Path(item).resolve()) for item in files],
        "out_dir": str(Path(out_dir).resolve()),
        "target": target or "",
    }
    if options:
        job["options"] = options
    return json.dumps(job, ensure_ascii=False).encode("utf-8")


class BatchResult:
    """一次批量执行的结果。"""

    def __init__(self):
        self.outputs: list[Path] = []
        self.errors: list[str] = []
        self.events = 0
        self.ok = 0
        self.failed = 0
        self.cancelled = False
        self.done: dict | None = None
        self.stderr = ""

    def __repr__(self) -> str:  # pragma: no cover - 调试用
        return (
            f"BatchResult(ok={self.ok}, failed={self.failed}, "
            f"cancelled={self.cancelled}, outputs={len(self.outputs)})"
        )


def run_batch(
    op: str,
    files,
    out_dir,
    target: str = "",
    options: dict | None = None,
    on_file: Callable[[str, str, Path | None], None] | None = None,
    cancel: threading.Event | None = None,
    idle_timeout: float | None = None,
) -> BatchResult:
    """流式执行一个批量任务。

    on_file(kind, message, input_path)：
        kind = "ok" 或 "err"，message 已经是可显示的一行中文描述。
    cancel：threading.Event，置位后向核心发送取消请求；核心会在当前文件结束后停止。
    """
    from converters_legacy import ConvertError

    exe = cli_path()
    if exe is None:
        raise ConvertError("未找到 C++ 核心 fccli.exe")

    out_path = Path(out_dir)
    out_path.mkdir(parents=True, exist_ok=True)
    payload = _build_job(op, files, out_path, target, options)
    command = [
        str(exe),
        "convert",
        "--job-line",
        "-",
        "--base-dir",
        str(base_dir()),
    ]
    try:
        proc = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=CREATE_NO_WINDOW,
        )
    except OSError as exc:
        raise ConvertError(f"无法启动 C++ 核心: {exc}") from exc

    result = BatchResult()
    line_q: queue.Queue = queue.Queue()
    stderr_chunks: list[str] = []

    def _read_stdout():
        try:
            for raw in proc.stdout:
                line_q.put(raw)
        finally:
            line_q.put(None)

    def _read_stderr():
        try:
            for raw in proc.stderr:
                stderr_chunks.append(raw.decode("utf-8", errors="replace"))
        except (OSError, ValueError):
            pass

    threading.Thread(target=_read_stdout, daemon=True).start()
    threading.Thread(target=_read_stderr, daemon=True).start()

    try:
        proc.stdin.write(payload + b"\n")
        proc.stdin.flush()
    except (OSError, ValueError) as exc:
        _terminate(proc)
        raise ConvertError(f"无法把任务交给 C++ 核心: {exc}") from exc

    cancel_sent = False
    idle_limit = idle_timeout_for(op) if idle_timeout is None else float(idle_timeout)
    last_event = time.monotonic()

    def send_cancel():
        nonlocal cancel_sent
        if cancel_sent:
            return
        cancel_sent = True
        try:
            proc.stdin.write(b"#cancel\n")
            proc.stdin.flush()
        except (OSError, ValueError):
            pass

    try:
        while True:
            if cancel is not None and cancel.is_set():
                send_cancel()
            try:
                raw = line_q.get(timeout=0.15)
            except queue.Empty:
                if time.monotonic() - last_event > idle_limit:
                    _terminate(proc)
                    hint = ""
                    if op.startswith(("word_", "excel_", "ppt_", "text_")):
                        hint = "。请检查 LibreOffice 是否被杀毒软件拦截，或在设置里指定 soffice.exe 路径"
                    elif op in ("audio_convert", "video_convert"):
                        hint = "。请检查 FFmpeg 是否可用，或在设置里指定 ffmpeg.exe 路径"
                    raise ConvertError(
                        f"C++ 核心超过 {int(idle_limit)} 秒没有任何进展，已停止{hint}"
                    )
                if proc.poll() is not None:
                    # 进程已退出，把队列里剩余事件读完再收尾。
                    continue
                continue
            if raw is None:
                break
            last_event = time.monotonic()
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            result.events += 1
            kind = event.get("type")
            if kind == "file_ok":
                result.ok += 1
                outputs = [Path(item) for item in event.get("outputs") or []]
                result.outputs.extend(outputs)
                if on_file is not None:
                    message = event.get("message") or ""
                    if not message:
                        names = ", ".join(item.name for item in outputs)
                        message = f"{Path(event.get('input', '')).name} → {names}"
                    on_file("ok", message, Path(event.get("input", "")))
            elif kind == "file_error":
                result.failed += 1
                message = str(event.get("error") or "转换失败")
                result.errors.append(message)
                if on_file is not None:
                    on_file("err", message, Path(event.get("input", "")))
            elif kind == "done":
                result.done = event
                result.cancelled = bool(event.get("cancelled"))
    finally:
        try:
            proc.stdin.close()
        except (OSError, ValueError):
            pass
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            _terminate(proc)

    result.stderr = "".join(stderr_chunks).strip()
    code = proc.returncode
    if result.events == 0:
        detail = result.stderr or "C++ 核心执行失败"
        try:
            parsed = json.loads(detail)
            detail = str(parsed.get("error", detail))
        except json.JSONDecodeError:
            pass
        raise ConvertError(detail)
    if code == 2:
        detail = result.stderr or "C++ 核心执行失败"
        try:
            parsed = json.loads(detail)
            detail = str(parsed.get("error", detail))
        except json.JSONDecodeError:
            pass
        raise ConvertError(detail)
    if result.done and result.done.get("cancelled"):
        result.cancelled = True
    return result


def _terminate(proc: subprocess.Popen) -> None:
    try:
        if proc.poll() is None:
            proc.kill()
    except OSError:
        pass


def run_job(
    op: str,
    files,
    out_dir,
    target: str = "",
    report: Callable[[str, str], None] | None = None,
    options: dict | None = None,
):
    """执行一个 C++ 核心任务，返回输出 Path 列表。

    report 存在时用于 batch_report 处理器：逐文件回调 ("ok"/"err", 文本)；
    不存在时，任何文件错误都会抛 ConvertError。
    """
    from converters_legacy import ConvertError

    on_file = None
    if report is not None:
        def on_file(kind: str, message: str, _path=None):
            report(kind, message)

    result = run_batch(op, files, out_dir, target, options=options, on_file=on_file)

    if report is not None:
        if result.failed and not result.errors and result.events == 0:
            raise ConvertError("有文件未能转换")
        return result.outputs

    if result.errors:
        raise ConvertError("\n".join(result.errors))
    if not result.outputs:
        if result.done and result.done.get("failed"):
            raise ConvertError("转换失败")
        raise ConvertError("C++ 核心未产出结果")
    return result.outputs