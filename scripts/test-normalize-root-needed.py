#!/usr/bin/env python3
import importlib.util
import pathlib
import subprocess
import tempfile
import unittest

SOURCE = pathlib.Path(__file__).with_name('normalize-root-needed.py')
spec = importlib.util.spec_from_file_location('root_normalizer', SOURCE)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def make_payload(root):
    (root / 'core.c').write_text('int root_core(void) {return 42;}\n')
    (root / 'payload.c').write_text('extern int root_core(void); int payload(void) {return root_core();}\n')
    subprocess.run(['cc', '-shared', '-fPIC', str(root / 'core.c'), '-Wl,-soname,libCore.so.6', '-o', str(root / 'libCore.so.6')], check=True)
    (root / 'libCore.so').symlink_to('libCore.so.6')
    subprocess.run(['cc', '-shared', '-fPIC', str(root / 'payload.c'), '-L', str(root), '-lCore', '-Wl,--no-as-needed', '-lgcc_s', '-o', str(root / 'payload.so')], check=True)
    return root / 'payload.so'


def names(path):
    return [name for name, _, _ in module.dependencies(path.read_bytes())]


class NormalizeDependenciesTest(unittest.TestCase):
    def test_root_only_rewrite_and_system_soname_preservation(self):
        with tempfile.TemporaryDirectory(prefix='root4duckdb-elf-', dir='/tmp') as tmp:
            root = pathlib.Path(tmp)
            payload = make_payload(root)
            self.assertIn('libCore.so.6', names(payload))
            self.assertIn('libgcc_s.so.1', names(payload))
            (root / 'libgcc_s.so').write_text('INPUT(libgcc_s.so.1)\n')
            module.normalize(payload, root)
            self.assertIn('libCore.so', names(payload))
            self.assertIn('libgcc_s.so.1', names(payload))
            self.assertNotIn('libgcc_s.so', names(payload))
            self.assertNotIn('libCore.so.6', names(payload))
            original = payload.read_bytes()
            module.normalize(payload, root)
            self.assertEqual(original, payload.read_bytes())

    def test_non_elf_root_linker_script_is_rejected_atomically(self):
        with tempfile.TemporaryDirectory(prefix='root4duckdb-elf-', dir='/tmp') as tmp:
            root = pathlib.Path(tmp)
            payload = make_payload(root)
            (root / 'libCore.so').unlink()
            (root / 'libCore.so').write_text('INPUT(libCore.so.6)\n')
            original = payload.read_bytes()
            with self.assertRaisesRegex(ValueError, 'not ELF'):
                module.normalize(payload, root)
            self.assertEqual(original, payload.read_bytes())

    def test_unversioned_gcc_dependency_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='root4duckdb-elf-', dir='/tmp') as tmp:
            root = pathlib.Path(tmp)
            (root / 'gcc_stub.c').write_text('int fake_gcc_runtime(void) {return 1;}\n')
            subprocess.run(['cc', '-shared', '-fPIC', str(root / 'gcc_stub.c'), '-Wl,-soname,libgcc_s.so', '-o', str(root / 'libfakegcc.so')], check=True)
            subprocess.run(['cc', '-shared', '-fPIC', '-x', 'c', '/dev/null', '-L', str(root), '-Wl,--no-as-needed', '-lfakegcc', '-o', str(root / 'payload.so')], check=True)
            payload = root / 'payload.so'
            self.assertIn('libgcc_s.so', names(payload))
            original = payload.read_bytes()
            with self.assertRaisesRegex(ValueError, 'Unsafe unversioned system dependency'):
                module.normalize(payload, root)
            self.assertEqual(original, payload.read_bytes())


if __name__ == '__main__':
    unittest.main()
