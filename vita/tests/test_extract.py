import sys
from pathlib import Path
import struct
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from extract_disc import parse_fst, write_original

class ExtractionTests(unittest.TestCase):
    def test_valid_file(self):
        table = struct.pack('>6I', 0x01000000, 0, 2, 0, 64, 16) + b'a.dat\0'
        self.assertEqual(parse_fst(table, 80), [(Path('a.dat'), 64, 16)])
        with self.assertRaises(ValueError): parse_fst(table, 79)
        with self.assertRaises(ValueError): parse_fst(table[:-1], 80)

    def test_invalid_names_and_directory(self):
        table = struct.pack('>6I', 0x01000000, 0, 2, 0, 64, 16)
        for name in (b'..\0', b'../x\0', b'\\x\0', b'ux0:x\0'):
            with self.assertRaises(ValueError): parse_fst(table+name, 80)
        directory = struct.pack('>6I', 0x01000000, 0, 2, 0x01000000, 0, 3) + b'dir\0'
        with self.assertRaises(ValueError): parse_fst(directory, 80)

    def test_preserve_existing_and_no_symlink(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record = write_original(root, Path('sys/main.dol'), b'original')
            self.assertEqual(write_original(root, Path('sys/main.dol'), b'original'), record)
            with self.assertRaises(ValueError): write_original(root, Path('sys/main.dol'), b'changed')
            self.assertEqual((root/'sys/main.dol').read_bytes(), b'original')
            (root/'alias').symlink_to(root/'sys', target_is_directory=True)
            with self.assertRaises(ValueError): write_original(root, Path('alias/test'), b'x')

if __name__ == '__main__': unittest.main()
