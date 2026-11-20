# 参与贡献

[English](CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md)

感谢你愿意改进这个项目。提交前请先确认下面几点。

## 开发环境

1. Windows 10/11 + Python 3.11/3.12（界面与回退）+ CMake 3.20+、C++17 编译器（核心）。
2. `python -m pip install -r requirements.txt`
3. 构建 C++ 核心：见 `cpp/README.zh-CN.md`；产物会复制到 `bin/`。
4. `python main.py` 启动界面；`python main.py --self-test` 跑自检。
5. 原生冒烟测试：`cmake -S cpp -B cpp/build -DFC_BUILD_TESTS=ON` 后运行 `fc_tests`。

Office 与音视频相关功能需要 `tools/LibreOffice` 和 `tools/FFmpeg`。这两个目录体积很大，不纳入版本库，获取方式见 `tools/README.zh-CN.md`。

## 提交前检查

```powershell
python -m ruff check .
python -m compileall -q main.py ui.py converters.py converters_legacy.py cpp_bridge.py self_test.py version.py
python -m unittest discover -s tests -v
cmake --build cpp/build --config Release
```

## 代码约定

1. 转换逻辑优先放在 `cpp/src/`；`converters.py` 只保留注册表与 C++ 桥接，`ui.py` 只负责界面。
2. 新增转换方式时，在 Python `CONVERSIONS` 与 C++ `registry.cpp` 同时登记，并补结构测试。
3. C++ 失败抛 `fc::Error`，CLI 转成 `file_error` 事件；Python 侧仍统一抛 `ConvertError`，界面逐文件捕获。
4. 不要写死本机绝对路径；外部工具统一走 `settings.json` 或 `tools/` 探测。
5. 不引入需要联网下载的运行时依赖，也不执行来源不明的安装脚本。

## 提交信息

使用简短的祈使句，例如 `fix: 修复 PDF 拆分同名覆盖`、`feat: 支持 AVIF 输出`。