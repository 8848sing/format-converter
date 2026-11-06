tools/ 目录说明
================

[English](README.md) | [简体中文](README.zh-CN.md)

本程序不会自动联网下载外部程序，也不会执行远程安装脚本。

FFmpeg
------
版本：n8.1.3-18-g46d899d7a0-20261018（LGPL 构建）
来源：https://github.com/BtbN/FFmpeg-Builds
路径：tools/FFmpeg/ffmpeg.exe
许可证：tools/FFmpeg/LICENSE.txt
SHA-256 与安全说明见程序根目录 THIRD_PARTY_SECURITY.zh-CN.md。

LibreOffice
-----------
用于 Word / Excel / PPT 与 PDF 的转换。
路径：tools/LibreOffice/program/soffice.exe
该组件沿用原 v1.1 便携包，本版没有重新下载或替换。

自定义路径
----------
可在程序根目录 settings.json 中配置：

  {
    "ffmpeg": "tools/FFmpeg/ffmpeg.exe",
    "soffice": "tools/LibreOffice/program/soffice.exe"
  }

也支持绝对路径。修改后重启程序生效。