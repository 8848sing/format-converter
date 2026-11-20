# C++ 核心（format-converter-core）

这是格式转换工具的 C++17 核心，桌面端 Python 界面和后续 Android 界面共用同一套转换实现。

## 目录

```text
cpp/
├─ CMakeLists.txt          # Windows / Android 共用的核心构建
├─ include/fc/core.h       # 稳定 C API（Python ctypes / JNI 均可调用）
├─ include/fc/version.h
├─ src/                    # 核心实现
│  ├─ image.cpp            # 图片解码/编码、EXIF 方向、GIF、ICO
│  ├─ image_codec_win.cpp  # Windows WIC（TIFF / ICO / HEIC 等系统编解码）
│  ├─ pdf_writer.cpp       # 原生 PDF 写入（图片转 PDF）
│  ├─ pdfium_api.cpp       # PDFium 动态加载（Windows）
│  ├─ pdf_ops.cpp          # PDF 渲染/合并/拆分/旋转/文字提取
│  ├─ docx.cpp / xlsx.cpp / pptx.cpp
│  ├─ office.cpp           # LibreOffice 外部进程
│  ├─ media.cpp            # FFmpeg 外部进程
│  ├─ data.cpp             # JSON/YAML/XML/XLSX/CSV
│  ├─ registry.cpp         # 30 个转换注册项
│  ├─ engine.cpp           # 批量调度、有限并发、逐文件事件
│  └─ cli_main.cpp         # fccli 命令行
├─ third_party/            # stb / nlohmann / miniz / tinyxml2 / gif.h / yaml-cpp / libwebp
├─ android/                # JNI 桥 + Gradle/Java 外壳
└─ resources/pptx/         # 精简 PPTX 模板
```

## 设计要点

- **统一错误**：内部抛 `fc::Error`，CLI/JSON 事件把错误传回界面；单个文件失败不会中断整批。
- **事件流**：每个任务输出一行 JSON：
  - `{"type":"file_ok","input":"...","outputs":[...]}`
  - `{"type":"file_error","input":"...","error":"..."}`
  - `{"type":"done","ok":N,"failed":N,"cancelled":false}`
- **有限并发**：图片任务按 CPU 逻辑线程数自动铺满（最多 16 路），其他类型 1～2 路；PDFium 调用加全局锁，避免 PDF 内部非线程安全。
- **UTF-8 路径**：Windows 下所有路径从 UTF-8 转宽字符，输出名用 `path_join_utf8` 拼接，避免中文/空格路径被按 ANSI 解码。
- **不覆盖**：`OutputAllocator` 在内存中登记已分配路径，同名输出自动加 `_1`、`_2`。
- **原子落盘**：输出先写 `.part` 再改名；音视频临时文件保留原扩展名，确保 FFmpeg 能推断封装格式。

## 依赖

- **PDFium**：预编译包（BSD）。Windows 使用 `pdfium.dll` 动态加载；Android 使用 `libpdfium.so`。
  - 下载：<https://github.com/bblanchon/pdfium-binaries/releases>
  - 目录：`cpp/third_party/pdfium-win/` 或 `cpp/third_party/pdfium-android-<abi>/`
- **libwebp / yaml-cpp**：源码已放在 `third_party/`，CMake 直接 `add_subdirectory`。
- **LibreOffice / FFmpeg**：桌面端外部程序，仍放在项目根目录 `tools/`，可用 `settings.json` 覆盖路径。

## Windows 构建

要求：CMake 3.20+、C++17 编译器（MSVC 2022 / MinGW-w64 / Zig 均可）。

```powershell
cmake -S cpp -B cpp/build -G "Visual Studio 17 2022" -A x64 `
  -DFC_PDFIUM_DIR="$PWD/cpp/third_party/pdfium-win"
cmake --build cpp/build --config Release
```

或 MinGW：

```powershell
cmake -S cpp -B cpp/build -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DFC_PDFIUM_DIR="$PWD/cpp/third_party/pdfium-win"
cmake --build cpp/build -j 8
```

产物：

```text
cpp/build/fccli.exe        # 命令行（静态链接 C++ 运行库）
cpp/build/fccore.dll       # C API 动态库
bin/fccli.exe              # CMake 自动复制，Python 界面优先从这里加载
bin/pdfium.dll
```

首次构建需要编译 libwebp / yaml-cpp，耗时几分钟；之后增量编译很快。

### 可选开关

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `FC_BUILD_CLI` | ON | 构建 `fccli` |
| `FC_BUILD_SHARED` | ON | 构建 `fccore` C API DLL |
| `FC_WITH_PDFIUM` | ON | 关闭后 PDF 操作返回“未启用” |
| `FC_WITH_WEBP` | ON | 关闭后 WebP 操作报错 |
| `FC_WITH_YAML` | ON | 关闭后 YAML 操作报错 |

## 命令行

```powershell
fccli --version
fccli list

# 直接指定参数
fccli convert --op image_convert --out D:\out --target webp a.png b.png

# 通过 JSON 任务（Python 界面使用这种方式）
fccli convert --job job.json --base-dir C:\path\to\app

# 自检
fccli self-test --base-dir C:\path\to\app
```

任务 JSON：

```json
{
  "op": "image_convert",
  "inputs": ["C:/in/a.png", "C:/in/b.png"],
  "out_dir": "C:/out",
  "target": "webp",
  "options": {"dpi": 200}
}
```

## C API

```c
#include "fc/core.h"

fc_context* ctx = fc_context_create("{\"base_dir\":\"C:/app\"}");
char* error = NULL;
int code = fc_run_job(ctx, job_json, on_event, user_data, &error);
fc_context_destroy(ctx);
```

- `fc_registry_json()` 返回 30 个转换项，字段与 Python 注册表一致。
- `fc_context_cancel()` 只停止尚未开始的任务；正在运行的外部进程会先完成。
- `fc_free_string()` 释放错误字符串。

## 功能覆盖

| 类别 | C++ 实现方式 | 备注 |
| --- | --- | --- |
| 图片互转 | stb + libwebp + Windows WIC | AVIF 编码依赖系统 WIC 编码器，Windows 默认没有；HEIC/TIFF/ICO 走 WIC |
| 图片转 PDF | 原生 PDF 写入 | JPEG 嵌入，页面按 150 DPI 计算尺寸 |
| PDF 渲染/文字 | PDFium | 200 DPI 渲染；PDFium 调用串行化 |
| PDF 合并/拆分/旋转 | PDFium | 逐页导入 `FPDF_ImportPages` |
| PDF 压缩 | PDFium 重写 | 结构重写，不重新压缩嵌入图片 |
| PDF 转 Word/Excel/PPT | 原生 DOCX/XLSX/PPTX | Word/Excel 为文字版；PPT 为整页图片版 |
| Office 互转 | LibreOffice `--convert-to` | 批量合并进程；Android 暂不支持 |
| 音视频 | FFmpeg | 与原 Python 参数一致；Android 暂不支持 |
| 数据 | nlohmann + yaml-cpp + tinyxml2 + miniz | XLSX 读写为轻量实现，支持首工作表 |

## Android

见 [`android/README.zh-CN.md`](android/README.zh-CN.md)。当前 Android 首版支持图片、PDF、数据三类；Office/音视频需要后续集成额外库。

## 验证状态

- Windows：`fccli self-test` 通过；图片/PDF/Office/音视频/数据共 42 项端到端检查通过。
- Android：NDK 27.2 下 arm64-v8a、x86_64 的 `libfccore_jni.so` 与 `libpdfium.so`
  编译通过，`assembleDebug` 生成可安装 APK；尚未在真机/模拟器上跑完整交互测试。
- C++ 冒烟测试：`cmake -DFC_BUILD_TESTS=ON` 后运行 `fc_tests`。

## 许可

核心代码 MIT。PDFium BSD，libwebp BSD，yaml-cpp MIT，stb 公共领域/MIT，miniz MIT，tinyxml2 zlib，gif.h 公共领域。各依赖的完整许可见 `third_party/`。