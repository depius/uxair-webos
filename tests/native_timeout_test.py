#!/usr/bin/env python3
"""Execute the real launcher's deadline function against both timeout contracts."""
import os
from pathlib import Path
import subprocess
import tempfile
source=(Path(__file__).resolve().parents[1]/'tools/run-native-uri-probe-b7.sh').read_text()
function='run_bounded_probe() {'+source.split('run_bounded_probe() {',1)[1].split('\n}',1)[0]+'\n}'
with tempfile.TemporaryDirectory() as folder:
    root=Path(folder); fake=root/'timeout'; capture=root/'args'
    fake.write_text("""#!/bin/sh
if [ "$1" = --help ]; then
    if [ "$TIMEOUT_MODE" = legacy ]; then echo 'Usage: timeout [-t SECS] [-s SIG] PROG ARGS'
    else echo 'Usage: timeout [-s SIG] SECS PROG ARGS'; fi
    exit 0
fi
printf '%s\\n' "$@" > "$TIMEOUT_CAPTURE"
""")
    fake.chmod(0o755)
    for mode in ('legacy','modern'):
        env=dict(os.environ,PATH=str(root)+os.pathsep+os.environ['PATH'],TIMEOUT_MODE=mode,TIMEOUT_CAPTURE=str(capture))
        subprocess.run(['sh','-c',function+'\nrun_bounded_probe ./direct-uri-probe "fixture value"'],env=env,check=True,timeout=5)
        expected=['-s','KILL']+(['-t','120'] if mode=='legacy' else ['120'])+['./direct-uri-probe','fixture value']
        assert capture.read_text().splitlines()==expected,mode
print('PASS: actual probe deadline selects legacy BusyBox -t and modern timeout syntax; argument boundaries preserved')
