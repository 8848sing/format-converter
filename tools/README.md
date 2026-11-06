tools/ directory
================

[English](README.md) | [简体中文](README.zh-CN.md)

This program never downloads external programs on its own and never executes remote install scripts.

FFmpeg
------
Version: n8.1.3-18-g46d899d7a0-20261018 (LGPL build)
Source: https://github.com/BtbN/FFmpeg-Builds
Path: tools/FFmpeg/ffmpeg.exe
License: tools/FFmpeg/LICENSE.txt
SHA-256 and security notes are in THIRD_PARTY_SECURITY.md at the project root.

LibreOffice
-----------
Used for Word / Excel / PPT and PDF conversion.
Path: tools/LibreOffice/program/soffice.exe
This component comes from the original v1.1 portable bundle; it was not re-downloaded or replaced in this version.

Custom paths
------------
Create settings.json at the project root:

  {
    "ffmpeg": "tools/FFmpeg/ffmpeg.exe",
    "soffice": "tools/LibreOffice/program/soffice.exe"
  }

Absolute paths are supported too. Restart the program after changing the file.