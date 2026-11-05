"""The public header and build metadata identify the same library release."""
import re
import unittest
from pathlib import Path

class VersionTests(unittest.TestCase):
    def test_header_matches_cmake(self):
        root=Path(__file__).resolve().parents[1]
        header=re.search(r'#define REWRITE_LINK_VERSION\s+"([^"]+)"',(root/'rewrite_link.h').read_text()).group(1)
        cmake=re.search(r'project\([^)]*VERSION\s+([0-9.]+)',(root/'CMakeLists.txt').read_text(),re.I).group(1)
        self.assertEqual(header,cmake)
if __name__=='__main__':unittest.main()
