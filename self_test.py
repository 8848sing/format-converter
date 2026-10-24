from pathlib import Path
import json
import subprocess
import tempfile

from PIL import Image, ImageDraw

import converters as c


def run_self_test() -> int:
    checks = []
    with tempfile.TemporaryDirectory(prefix='format-converter-test-') as tmp:
        root = Path(tmp)
        inputs = root / 'inputs'
        outputs = root / 'outputs'
        inputs.mkdir()
        outputs.mkdir()

        image_path = inputs / 'sample.png'
        image = Image.new('RGB', (320, 180), '#3975ff')
        ImageDraw.Draw(image).rectangle((20, 20, 300, 160), outline='white', width=4)
        image.save(image_path)

        def check(name, func):
            try:
                ok = bool(func())
                checks.append({'name': name, 'ok': ok, 'error': '' if ok else 'empty output'})
            except Exception as exc:
                checks.append({'name': name, 'ok': False, 'error': str(exc)})

        check('image-jpg', lambda: c.convert_images_format_plus([image_path], outputs, 'jpg')[0].stat().st_size > 0)
        check('image-heic-roundtrip', lambda: c.convert_images_format_plus([c.convert_images_format_plus([image_path], outputs, 'heic')[0]], outputs, 'png')[0].stat().st_size > 0)
        check('image-avif', lambda: c.convert_images_format_plus([image_path], outputs, 'avif')[0].stat().st_size > 0)
        check('image-ico', lambda: c.convert_images_format_plus([image_path], outputs, 'ico')[0].stat().st_size > 0)

        pdf_a = outputs / 'a.pdf'
        pdf_b = outputs / 'b.pdf'
        image.save(pdf_a, 'PDF')
        image.save(pdf_b, 'PDF')

        import pymupdf
        text_doc = pymupdf.open()
        text_page = text_doc.new_page()
        text_page.insert_text((72, 72), 'Hello 12345')
        text_page.insert_text((72, 100), 'Second line for extraction')
        text_pdf = outputs / 'text.pdf'
        text_doc.save(text_pdf)
        text_doc.close()
        check('pdf-merge', lambda: len(__import__('pypdf').PdfReader(str(c.merge_pdfs([pdf_a, pdf_b], outputs)[0])).pages) == 2)
        check('pdf-split-rotate-compress', lambda: len(c.split_pdf([c.rotate_pdf([c.compress_pdf([pdf_a], outputs)[0]], outputs, '90')[0]], outputs)) == 1)
        check('pdf-to-image', lambda: c.convert_pdf_to_images([pdf_a], outputs, 'png')[0].stat().st_size > 0)
        check('pdf-to-word', lambda: c.convert_pdf_to_word([pdf_a], outputs)[0].stat().st_size > 0)
        check('pdf-to-ppt', lambda: c.convert_pdf_to_ppt([pdf_a], outputs)[0].stat().st_size > 0)
        check('pdf-to-text', lambda: c.convert_pdf_to_text([text_pdf], outputs)[0].stat().st_size > 0)
        check('pdf-to-excel', lambda: c.convert_pdf_to_excel([text_pdf], outputs)[0].exists())

        try:
            soffice = c.find_soffice()
        except Exception:
            soffice = None
        checks.append({'name': 'libreoffice-found', 'ok': bool(soffice),
                       'error': '' if soffice else 'soffice.exe not found'})
        if soffice:
            from openpyxl import Workbook
            wb = Workbook()
            ws = wb.active
            ws['A1'] = 'self test'
            ws['B2'] = 42
            xlsx_in = inputs / 'sheet.xlsx'
            wb.save(xlsx_in)
            check('office-to-pdf', lambda: c.office_to_pdf([xlsx_in], outputs, 'pdf')[0].stat().st_size > 0)

        ffmpeg = c.find_ffmpeg()
        wav = inputs / 'tone.wav'
        subprocess.run([str(ffmpeg), '-hide_banner', '-y', '-f', 'lavfi', '-i', 'sine=frequency=880:duration=1', str(wav)], check=True, capture_output=True)
        check('ffmpeg-mp3', lambda: c.convert_media([wav], outputs, 'mp3')[0].stat().st_size > 0)

        source = inputs / 'data.json'
        source.write_text(json.dumps({'items': [{'name': 'A', 'value': 1}, {'name': 'B', 'value': 2}]}), encoding='utf-8')
        yaml_file = c.convert_structured([source], outputs, 'yaml')[0]
        xml_file = c.convert_structured([source], outputs, 'xml')[0]
        xlsx_file = c.convert_structured([source], outputs, 'xlsx')[0]
        check('data-convert', lambda: all(p.stat().st_size > 0 for p in (yaml_file, xml_file, xlsx_file)))
        check('excel-json', lambda: c.convert_table_to_json([xlsx_file], outputs)[0].stat().st_size > 0)

    result = {'ok': all(item['ok'] for item in checks), 'checks': checks}
    report = c.base_dir() / 'self-test-result.json'
    report.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return 0 if result['ok'] else 1
