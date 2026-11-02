"""Source archives retain lock manifests, never installed host npm executables."""
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest

ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('source_package',ROOT/'tools/source-package.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class SourcePackageTest(unittest.TestCase):
    def test_nested_build_tools_excluded(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            for name in ['webos-cli/package.json','webos-cli/package-lock.json','webos-cli/node_modules/cli/bin/executable.js','__pycache__/cache.pyc']:
                p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('fixture')
            buf=io.BytesIO()
            with tarfile.open(fileobj=buf,mode='w') as t:t.add(root,arcname='uxair-webos/tools',filter=m.source_member)
            with tarfile.open(fileobj=io.BytesIO(buf.getvalue())) as t:names=t.getnames()
            self.assertIn('uxair-webos/tools/webos-cli/package-lock.json',names)
            self.assertIn('uxair-webos/tools/webos-cli/package.json',names)
            self.assertFalse(any('node_modules' in x or '__pycache__' in x for x in names))

if __name__=='__main__':unittest.main()
