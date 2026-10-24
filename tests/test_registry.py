# -*- coding: utf-8 -*-
"""转换注册表的结构性测试：不依赖任何第三方库，可在 CI 中直接运行。

    python -m unittest discover -s tests -v
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import converters as c


class RegistryTest(unittest.TestCase):
    def test_conversion_count(self):
        self.assertEqual(len(c.CONVERSIONS), 30)

    def test_entries_are_complete(self):
        for conv in c.CONVERSIONS:
            with self.subTest(label=conv.label):
                self.assertTrue(conv.label)
                self.assertTrue(conv.src)
                self.assertTrue(conv.dst)
                self.assertTrue(conv.extensions)
                self.assertTrue(callable(conv.handler))
                self.assertGreaterEqual(conv.workers, 1)
                self.assertTrue(conv.description)

    def test_extensions_are_normalised(self):
        for conv in c.CONVERSIONS:
            for ext in conv.extensions:
                self.assertTrue(ext.startswith('.'), ext)
                self.assertEqual(ext, ext.lower(), ext)

    def test_labels_are_unique_per_pair(self):
        seen = set()
        for conv in c.CONVERSIONS:
            key = (conv.src, conv.dst, conv.label)
            self.assertNotIn(key, seen)
            seen.add(key)

    def test_categories_round_trip(self):
        for src in c.categories_src():
            for dst in c.dsts_for(src):
                self.assertTrue(c.pair_exists(src, dst))
                self.assertTrue(c.convs_for(src, dst))

    def test_categories_have_extensions(self):
        for cat in c.categories_src():
            self.assertIn(cat, c.CATEGORY_EXTS)
            self.assertTrue(c.CATEGORY_EXTS[cat])


if __name__ == '__main__':
    unittest.main()