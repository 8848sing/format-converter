# -*- coding: utf-8 -*-
"""桌面端转换矩阵回归（不启动界面，直接走 converters.run_batch）。"""
import json
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import converters  # noqa: E402

PASS, FAIL = [], []


def conv(src, dst, label=None):
    items = converters.convs_for(src, dst)
    if not items:
        raise SystemExit(f"注册表缺少 {src} -> {dst}")
    if label:
        for item in items:
            if item.label == label:
                return item
        raise SystemExit(f"{src}->{dst} 没有 {label}")
    return items[0]


def check(name, fn):
    try:
        detail = fn()
        PASS.append(f"{name}: {detail}")
        print(f"PASS  {name}  {detail}")
    except Exception as exc:  # noqa: BLE001
        FAIL.append(f"{name}: {exc}")
        print(f"FAIL  {name}  {exc}")


def main():
    tmp = Path(tempfile.mkdtemp(prefix="fc_e2e_"))
    print("工作目录:", tmp)
    from PIL import Image, ImageDraw

    # 样本图片：一批 6 张不同尺寸，含中文名与空格
    src_dir = tmp / "in"
    src_dir.mkdir()
    images = []
    for i in range(6):
        p = src_dir / f"照片 {i}.png"
        img = Image.new("RGB", (400 + i * 20, 300 + i * 10), (30 + i * 25, 90, 200 - i * 20))
        draw = ImageDraw.Draw(img)
        draw.rectangle([20, 20, 120, 90], fill=(255, 200 - i * 15, 60))
        draw.text((30, 120), f"sample {i}", fill=(255, 255, 255))
        img.save(p)
        images.append(p)

    out1 = tmp / "out_webp"

    def batch_webp():
        c = conv("图片", "图片", "图片格式互转")
        got = []
        t0 = time.perf_counter()
        result = c.run_batch(images, out1, "webp", options={"quality": 80},
                             on_file=lambda k, m, p: got.append((k, m)))
        elapsed = time.perf_counter() - t0
        assert result.ok == 6, f"成功 {result.ok}"
        assert result.failed == 0, result.errors
        assert len(got) == 6
        assert all(p.exists() and p.stat().st_size > 0 for p in result.outputs)
        return f"6 张 → webp，{elapsed:.2f}s，首条 {got[0][1]}"

    check("图片转 WebP（批量 + 质量 80）", batch_webp)

    def quality_diff():
        c = conv("图片", "图片", "图片格式互转")
        low = tmp / "q10"
        high = tmp / "q95"
        c.run_batch([images[0]], low, "jpg", options={"quality": 10})
        c.run_batch([images[0]], high, "jpg", options={"quality": 95})
        a = next(low.glob("*.jpg")).stat().st_size
        b = next(high.glob("*.jpg")).stat().st_size
        assert b > a * 1.5, f"质量参数未生效 {a} vs {b}"
        return f"质量 10={a}B，95={b}B"

    check("质量参数影响输出体积", quality_diff)

    def avif_output():
        c = conv("图片", "图片", "图片格式互转")
        out = tmp / "out_avif"
        r = c.run_batch([images[0]], out, "avif", options={"quality": 70})
        assert r.ok == 1, r.errors
        produced = r.outputs[0]
        assert produced.stat().st_size > 512, produced.stat().st_size
        with Image.open(produced) as back:
            assert back.size == (400, 300), back.size
        return f"{produced.name} {produced.stat().st_size // 1024} KB（FFmpeg 兜底）"

    check("图片转 AVIF（FFmpeg 兜底）", avif_output)

    def img_to_pdf():
        c = conv("图片", "PDF", "图片 转 PDF（每张单独一个）")
        out = tmp / "out_pdf"
        r = c.run_batch(images, out, "")
        assert r.ok == 6, f"成功 {r.ok} 失败 {r.failed}: {r.errors}"
        return f"{len(r.outputs)} 个 PDF"

    check("图片转 PDF", img_to_pdf)

    def merge_pdf():
        c = conv("图片", "PDF", "多张图片 合并为一个 PDF")
        out = tmp / "out_merge"
        r = c.run_batch(images, out, "")
        first = r.outputs[0]
        assert first.stat().st_size > 1000
        return first.name

    check("多图合并 PDF", merge_pdf)

    pdfs = sorted((tmp / "out_pdf").glob("*.pdf"))

    def pdf_to_png():
        c = conv("PDF", "图片", "PDF 转图片（每页一张）")
        out = tmp / "out_png"
        r = c.run_batch(pdfs[:2], out, "png", options={"dpi": 150})
        assert r.ok == 2, r.errors
        sizes = [Image.open(p).size for p in r.outputs]
        return f"{len(r.outputs)} 张，尺寸 {sizes[0]}"

    check("PDF 转图片（150 DPI）", pdf_to_png)

    def pdf_text():
        c = conv("PDF", "文本", "PDF 转 纯文本")
        out = tmp / "out_txt"
        r = c.run_batch(pdfs[:1], out, "")
        if r.failed and any("未提取到文本" in e for e in r.errors):
            return "图片 PDF 无文字层，按预期报错"
        assert r.outputs, r.errors
        return r.outputs[0].name

    check("PDF 转文本（无文字层按预期）", pdf_text)

    def pdf_merge_split():
        c = conv("PDF", "PDF", "PDF 合并（多个文件 → 一个）")
        out = tmp / "out_pdfmerge"
        r = c.run_batch(pdfs[:2], out, "")
        merged = r.outputs[0]
        c2 = conv("PDF", "PDF", "PDF 拆分（一页一个文件）")
        out2 = tmp / "out_pdfsplit"
        r2 = c2.run_batch([merged], out2, "")
        assert r2.ok == 1
        return f"合并 {merged.name}，拆分 {len(r2.outputs)} 个"

    check("PDF 合并 + 拆分", pdf_merge_split)

    def pdf_rotate_compress():
        c = conv("PDF", "PDF", "PDF 旋转页面")
        out = tmp / "out_rot"
        c.run_batch(pdfs[:1], out, "")
        c2 = conv("PDF", "PDF", "PDF 压缩（内容流）")
        out2 = tmp / "out_zip"
        r = c2.run_batch(pdfs[:1], out2, "")
        assert r.outputs
        return "旋转 + 压缩完成"

    check("PDF 旋转 + 压缩", pdf_rotate_compress)

    # 数据
    data_json = tmp / "data.json"
    data_json.write_text(
        json.dumps([{"名称": "甲", "数量": 3}, {"名称": "乙", "数量": 7}], ensure_ascii=False),
        encoding="utf-8",
    )

    def data_xlsx():
        c = conv("数据", "Excel", "结构化数据 转 Excel")
        out = tmp / "out_xlsx"
        r = c.run_batch([data_json], out, "xlsx")
        assert r.outputs, r.errors
        return r.outputs[0].name

    check("JSON 转 XLSX", data_xlsx)

    def xlsx_json():
        c = conv("Excel", "数据", "Excel / CSV 转 JSON")
        out = tmp / "out_json"
        xlsx = next((tmp / "out_xlsx").glob("*.xlsx"))
        r = c.run_batch([xlsx], out, "json")
        assert r.outputs, r.errors
        text = r.outputs[0].read_text(encoding="utf-8")
        assert "甲" in text
        return f"{r.outputs[0].name} 含中文"

    check("XLSX 转 JSON", xlsx_json)

    def data_yaml():
        c = conv("数据", "数据", "结构化数据互转")
        out = tmp / "out_yaml"
        r = c.run_batch([data_json], out, "yaml")
        assert r.outputs, r.errors
        return r.outputs[0].name

    check("JSON 转 YAML", data_yaml)

    # TIFF：走内置解码器（不依赖 WIC 扩展）
    def tiff_input():
        tif = tmp / "in" / "scan.tif"
        Image.open(images[0]).save(tif, format="TIFF", compression="tiff_lzw")
        c = conv("图片", "图片", "图片格式互转")
        out = tmp / "out_from_tiff"
        r = c.run_batch([tif], out, "png", options={"quality": 90})
        assert r.ok == 1, r.errors
        return f"TIFF(LZW) → {r.outputs[0].name}"

    check("TIFF 输入（内置解码）", tiff_input)

    # Word → 文本（有 soffice 走 LibreOffice，没有走内置提取）
    def word_text():
        from docx import Document

        doc = Document()
        doc.add_heading("测试标题", level=1)
        doc.add_paragraph("这是正文，用来验证 Word 转文本。")
        table = doc.add_table(rows=1, cols=2)
        table.rows[0].cells[0].text = "列一"
        table.rows[0].cells[1].text = "列二"
        docx = tmp / "in" / "测试文档.docx"
        doc.save(str(docx))
        c = conv("Word", "文本", "Word 转 纯文本")
        out = tmp / "out_docx"
        r = c.run_batch([docx], out, "")
        assert r.outputs, r.errors
        text = r.outputs[0].read_text(encoding="utf-8", errors="replace")
        assert "测试标题" in text, text[:80]
        return f"{r.outputs[0].name}（内置提取）"

    check("Word 转文本", word_text)

    # 取消：一批大图，中途取消
    def cancel_batch():
        import threading

        big = []
        for i in range(24):
            p = tmp / "in" / f"big_{i}.png"
            Image.open(images[0]).resize((2200, 1600)).save(p)
            big.append(p)
        out = tmp / "out_cancel"
        event = threading.Event()
        seen = []

        def on_file(kind, message, path):
            seen.append(kind)
            if len(seen) >= 3:
                event.set()

        c = conv("图片", "图片", "图片格式互转")
        r = c.run_batch(big, out, "webp", on_file=on_file, cancel=event)
        assert r.cancelled, "未标记取消"
        assert r.ok < 24, f"取消后仍完成 {r.ok} 个"
        assert not list(out.glob("*.part")), "存在残留临时文件"
        return f"完成 {r.ok} / 24 后取消，无残留"

    check("中途取消", cancel_batch)

    print("\n通过", len(PASS), "失败", len(FAIL))
    for item in FAIL:
        print(" -", item)
    return 0 if not FAIL else 1


if __name__ == "__main__":
    raise SystemExit(main())