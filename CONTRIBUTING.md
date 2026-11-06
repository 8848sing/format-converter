# Contributing

Thanks for wanting to improve this project. Please go through the points below before opening a pull request.

[English](CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md)

## Development setup

1. Windows 10/11 with Python 3.11 or 3.12 (`pdf2docx` lags behind new Python releases).
2. `python -m pip install -r requirements.txt`
3. `python main.py` starts the UI.
4. `python main.py --self-test` runs the full-feature self-test and writes `self-test-result.json`.

Office and audio/video features need `tools/LibreOffice` and `tools/FFmpeg`. Both directories are large and not tracked by git; see `tools/README.md` for how to obtain them.

## Checks before submitting

```powershell
python -m ruff check .
python -m compileall -q main.py ui.py converters.py self_test.py version.py
python -m unittest discover -s tests -v
```

## Code conventions

1. Conversion logic belongs in `converters.py`, UI logic in `ui.py`; neither should reach into the other's responsibilities.
2. A new conversion mode must be registered in the `CONVERSIONS` table and covered by a structural constraint that `tests/test_registry.py` can check.
3. Failures raise `ConvertError` uniformly; the UI catches it per file so one bad file never aborts a batch.
4. Never hard-code absolute paths from your machine; external tools are resolved through `settings.json` or `tools/` discovery.
5. Do not add runtime dependencies that require a network download, and do not execute installers or scripts of unknown origin.

## Commit messages

Use short imperative sentences in English, for example `fix: prevent PDF split from overwriting same-named files`, `feat: support AVIF output`.