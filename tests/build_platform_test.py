"""Host selection and installer filename routing; no TV or package build used."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('bootstrap', ROOT / 'tools/bootstrap.py')
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)

class PlatformTest(unittest.TestCase):
    def test_packaging_requires_backend_before_staging(self):
        package_spec = importlib.util.spec_from_file_location('package', ROOT / 'tools/package.py')
        package = importlib.util.module_from_spec(package_spec)
        package_spec.loader.exec_module(package)
        scratch = ROOT / 'build/tests'
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as tmp, patch.object(package, 'ROOT', Path(tmp)), patch.object(sys, 'argv', ['package.py']):
            with self.assertRaises(SystemExit) as result:
                package.main()
            self.assertEqual(result.exception.code, 2)
            self.assertFalse((Path(tmp) / 'build/app').exists())

    def test_supported_hosts_only(self):
        for system, machine, expected in [('Linux', 'x86_64', 'toolchain-linux-x86_64'),
                                          ('Darwin', 'arm64', 'toolchain'),
                                          ('Windows', 'AMD64', None), ('Linux', 'aarch64', None)]:
            with patch.object(bootstrap.platform, 'system', return_value=system), patch.object(bootstrap.platform, 'machine', return_value=machine):
                if expected:
                    self.assertEqual(bootstrap.host_toolchain(), expected)
                else:
                    with self.assertRaises(SystemExit):
                        bootstrap.host_toolchain()

    def test_installer_routes_both_variants(self):
        bash = os.environ.get('TEST_BASH') or shutil.which('bash')
        if not bash:
            self.skipTest('Bash unavailable')
        # Exported functions intercept every packaging/install operation. No
        # device is contacted and no production marker/staging files are changed.
        script = r'''
python3() { if [ "$1" = tools/package.py ]; then return 0; fi; "$TEST_PYTHON" "$@"; }
cat() { if [ "$1" = build/media-backend.txt ]; then printf '%s\n' "$TEST_BACKEND"; else command cat "$@"; fi; }
ares-install() { printf 'INSTALL:%s\n' "$@"; }
export -f python3 cat ares-install
bash tools/tv.sh install
'''
        app = json.loads((ROOT / 'packaging/appinfo.json').read_text())
        for backend, suffix in [('starfish', 'webos3-4'), ('ndl', 'webos5plus'), ('invalid', None)]:
            env = dict(os.environ, TV_DEVICE='test-only', TEST_BACKEND=backend,
                       TEST_PYTHON=Path(sys.executable).as_posix())
            result = subprocess.run([bash, '-c', script], cwd=ROOT, env=env, text=True, capture_output=True, timeout=15)
            if suffix:
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"INSTALL:dist/{app['id']}_{app['version']}_arm-{suffix}.ipk", result.stdout)
            else:
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('INSTALL:', result.stdout)

if __name__ == '__main__':
    unittest.main()
