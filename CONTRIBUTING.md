# 参与贡献

感谢你愿意改进这个项目。提交前请先确认下面几点。

## 开发环境

1. Windows 10/11 + Python 3.11 或 3.12（`pdf2docx` 对新版 Python 支持滞后）。
2. `python -m pip install -r requirements.txt`
3. `python main.py` 启动界面。
4. `python main.py --self-test` 跑一遍全功能自检，结果写入 `self-test-result.json`。

Office 与音视频相关功能需要 `tools/LibreOffice` 和 `tools/FFmpeg`。这两个目录体积很大，不纳入版本库，获取方式见 `tools/README.md`。

## 提交前检查

```powershell
python -m ruff check .
python -m compileall -q main.py ui.py converters.py self_test.py version.py
python -m unittest discover -s tests -v
```

## 代码约定

1. 转换逻辑放在 `converters.py`，界面逻辑放在 `ui.py`，两者不要互相侵入。
2. 新增转换方式时，在 `CONVERSIONS` 注册表登记，并补一条 `tests/test_registry.py` 能覆盖的结构约束。
3. 失败统一抛 `ConvertError`，由界面逐文件捕获，保证单个文件失败不中断整批任务。
4. 不要写死本机绝对路径；外部工具统一走 `settings.json` 或 `tools/` 探测。
5. 不引入需要联网下载的运行时依赖，也不执行来源不明的安装脚本。

## 提交信息

使用简短的祈使句，例如 `fix: 修复 PDF 拆分同名覆盖`、`feat: 支持 AVIF 输出`。