#!/usr/bin/env python3
"""Guard the isolated hardware probe's deadline, ownership, state and package scope."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
probe=(root/'src/direct_uri_probe.cpp').read_text()
launcher=(root/'tools/run-native-uri-probe-b7.sh').read_text()
for forbidden in ('Receiver ', 'start_log(', 'load_settings(', 'save_settings(', 'save_usage_stats(', 'UXAIR_DIRECT_DISCOVERY=after'):
    assert forbidden not in probe+launcher
assert 'timeout -s KILL -t 120' in launcher and 'timeout -s KILL 120' in launcher
assert 'run_bounded_probe ./direct-uri-probe' in launcher
assert 'pidof uxair' in launcher and 'no process was stopped' in launcher
assert 'com.github.depius.uxair.dev' in launcher and 'init=different' in launcher and 'ums=different' in launcher
assert 'direct_fixture_uri_valid(' in probe and '16000000000LL' in probe and '6000000000LL' in probe
assert 'status.relay_requests>0&&status.upstream_fetches>0&&status.downloaded_bytes>0' in probe
assert probe.count('{"A-')==1 and probe.count('{"F-')==1
assert 'fixture_direct&&!direct_fixture_uri_valid' in (root/'src/direct_player.cpp').read_text()
assert 'DirectLoadContract::ArgsHls,bool fixture_direct=false' in (root/'src/direct_player.h').read_text()
package=(root/'tools/package.py').read_text()
assert "if args.development:\n        shutil.copy2(ROOT / 'build/direct-uri-probe'" in package
assert "'direct-uri-probe'" in (root/'tools/verify-elf.py').read_text()
assert 'native URI probe must be ARM ELF32' in (root/'tools/validate-ipk.py').read_text()
print('PASS: probe is fixture-only, finite, exclusive, development-only and never starts RAOP or reads/writes saved state')
