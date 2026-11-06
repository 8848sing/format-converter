# 第三方工具与安全说明

[English](THIRD_PARTY_SECURITY.md) | [简体中文](THIRD_PARTY_SECURITY.zh-CN.md)

本版只新增了一个外部二进制工具：FFmpeg。其余新增功能均使用经过 PyPI 分发的 Python 库。

## FFmpeg

- 来源仓库：https://github.com/BtbN/FFmpeg-Builds
- Release 页面：https://github.com/BtbN/FFmpeg-Builds/releases/tag/latest
- 下载文件：`ffmpeg-n8.1-latest-win64-lgpl-8.1.zip`
- 发布日期：2026-10-18
- 文件大小：170,541,588 字节
- SHA-256：`abff292c5cee9f44e3c4d2830ba424708f7135601c998830faa3b22d79542454`
- 构建版本：`ffmpeg version n8.1.3-18-g46d899d7a0-20261018`
- 许可证：LGPL 构建，许可证文本位于 `tools/FFmpeg/LICENSE.txt`

校验方法（PowerShell）：

```powershell
Get-FileHash .\ffmpeg-n8.1-latest-win64-lgpl-8.1.zip -Algorithm SHA256
```

下载完成后，已与 GitHub Release API 发布的 digest 逐字核对，结果一致。

## 安全措施

- 没有执行来源不明的安装器、PowerShell 下载脚本或批处理脚本。
- FFmpeg 仅从 GitHub 官方 Release 获取，并校验 SHA-256。
- 程序调用 FFmpeg 时使用 `subprocess.run` 参数数组，不使用 `shell=True`，文件名不会被解释为命令。
- 转换超时上限为 2 小时；失败时会删除 `.part` 半成品。
- PyInstaller 打包关闭 UPX，避免压缩壳增加杀毒软件误报概率。
- `pillow-heif`、`pypdf`、`PyYAML` 均从 PyPI 安装，并写入 `requirements.txt`。

## 仍需了解的边界

- 没有任何软件能仅凭“来源可信”保证 100% 无风险；本说明记录的是实际来源和校验结果。
- 本程序当前没有商业代码签名证书，因此 Windows SmartScreen 可能仍会显示“未知发布者”。这不等于检测到病毒，但正式对外分发时建议购买代码签名证书。
- LibreOffice 是原 v1.1 便携包中已有的组件，本版没有重新下载或替换它。其许可证和来源请保留原分发目录中的许可证文件。