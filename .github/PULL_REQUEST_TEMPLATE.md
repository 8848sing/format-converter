## 这个 PR 做了什么

<!-- 一句话说清目的；如果是修 bug，请关联 Issue，例如：Fixes #12 -->

## 改动类型

- [ ] 修 bug
- [ ] 新功能 / 新格式支持
- [ ] 性能优化
- [ ] 界面 / 交互
- [ ] 文档 / 构建 / CI
- [ ] 重构（不改变行为）

## 自测

- [ ] `python -m ruff check .` 通过
- [ ] `python -m unittest discover -s tests -v` 通过
- [ ] `python tests/desktop_matrix.py` 通过（改了转换核心时必跑）
- [ ] 改了 C++：`fccli.exe self-test` 9 项全过
- [ ] 改了界面：附上改动前后的截图

## 需要 reviewer 注意的地方

<!-- 例如：改了 ffmpeg 参数、动了并发上限、引入了新依赖 -->
