# Contributing

Thanks for wanting to improve this project. Please go through the points below before opening a pull request.

[English](CONTRIBUTING.md) | [简体中文](CONTRIBUTING.zh-CN.md)

## Development setup

1. Windows 10/11 with Python 3.11/3.12 (UI and fallback) plus CMake 3.20+ and a C++17 compiler (core).
2. `python -m pip install -r requirements.txt`
3. Build the C++ core (see `cpp/README.zh-CN.md`); the build copies `fccli.exe` into `bin/`.
4. `python main.py` starts the UI; `python main.py --self-test` runs the self-test.
5. Native smoke test: `cmake -S cpp -B cpp/build -DFC_BUILD_TESTS=ON`, then run `fc_tests`.

Office and audio/video features need `tools/LibreOffice` and `tools/FFmpeg`. Both directories are large and not tracked by git; see `tools/README.md` for how to obtain them.

## Checks before submitting

```powershell
python -m ruff check .
python -m compileall -q main.py ui.py converters.py converters_legacy.py cpp_bridge.py self_test.py version.py
python -m unittest discover -s tests -v
cmake --build cpp/build --config Release
```

## Code conventions

1. Conversion logic prefers `cpp/src/`; `converters.py` keeps only the registry and the C++ bridge, and `ui.py` owns the UI.
2. A new conversion mode must be registered in both the Python `CONVERSIONS` table and C++ `registry.cpp`, with a structural test.
3. C++ failures raise `fc::Error` and become `file_error` events; Python still raises `ConvertError` uniformly, and the UI catches it per file so one bad file never aborts a batch.
4. Never hard-code absolute paths from your machine; external tools are resolved through `settings.json` or `tools/` discovery.
5. Do not add runtime dependencies that require a network download, and do not execute installers or scripts of unknown origin.

## Commit messages

Use short imperative sentences in English, for example `fix: prevent PDF split from overwriting same-named files`, `feat: support AVIF output`.