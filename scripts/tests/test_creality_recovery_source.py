"""The recovery source check preserves runtime directives and literal contents."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
from check_creality_recovery import code, normalized


class SourceEquivalenceTests(unittest.TestCase):
    def parse(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.cpp"
            path.write_text(text, encoding="utf-8")
            return normalized(code(path))

    def test_direct_includes_and_comments_do_not_change_equivalence(self):
        self.assertEqual(self.parse('#include <string>\nint value = 1; // note\n'),
                         self.parse('int value=1;'))

    def test_conditional_compilation_is_a_runtime_difference(self):
        self.assertNotEqual(self.parse('#if 0\nint value=1;\n#endif\n'),
                            self.parse('int value=1;'))

    def test_literals_keep_spaces_and_comment_like_characters(self):
        self.assertNotEqual(self.parse('auto s="A // B";'), self.parse('auto s="A //B";'))


if __name__ == "__main__":
    unittest.main()
