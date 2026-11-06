# Third-party tools and security notes

[English](THIRD_PARTY_SECURITY.md) | [简体中文](THIRD_PARTY_SECURITY.zh-CN.md)

This version adds exactly one external binary: FFmpeg. Every other new capability uses Python packages distributed through PyPI.

## FFmpeg

- Source repository: https://github.com/BtbN/FFmpeg-Builds
- Release page: https://github.com/BtbN/FFmpeg-Builds/releases/tag/latest
- Downloaded file: `ffmpeg-n8.1-latest-win64-lgpl-8.1.zip`
- Release date: 2026-10-18
- Size: 170,541,588 bytes
- SHA-256: `abff292c5cee9f44e3c4d2830ba424708f7135601c998830faa3b22d79542454`
- Build: `ffmpeg version n8.1.3-18-g46d899d7a0-20261018`
- License: LGPL build; the license text ships in `tools/FFmpeg/LICENSE.txt`

Verify it (PowerShell):

```powershell
Get-FileHash .\ffmpeg-n8.1-latest-win64-lgpl-8.1.zip -Algorithm SHA256
```

The computed hash was compared character by character with the digest published by the GitHub Release API and matched.

## Security measures

- No installer, download script or batch file of unknown origin was executed.
- FFmpeg was fetched only from the official GitHub Release and verified with SHA-256.
- The program invokes FFmpeg with `subprocess.run` argument arrays and never `shell=True`, so filenames are never interpreted as commands.
- Conversion timeouts are capped at 2 hours; on failure the partial `.part` file is deleted.
- The PyInstaller build disables UPX so a compression shell cannot raise antivirus false-positive rates.
- `pillow-heif`, `pypdf` and `PyYAML` are installed from PyPI and pinned in `requirements.txt`.

## Boundaries you should still know

- No software can be proven 100% risk-free merely because its source is trusted; this document records the actual source and verification result.
- The application currently has no commercial code-signing certificate, so Windows SmartScreen may still show "unknown publisher". That is not the same as a virus detection, but buying a certificate is recommended before wide public distribution.
- LibreOffice is the component already present in the original v1.1 portable bundle; it was not re-downloaded or replaced in this version. Its license and source remain documented in the license files of that distribution.