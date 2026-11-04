"""Validate the actual sanitizer-backed ingress executable's fixed-format output."""
from pathlib import Path
import os, subprocess
root=Path(__file__).resolve().parents[1]
exe=os.environ.get('UXAIR_INGRESS_TEST',str(root/'build/tests/receiver-ingress-test'))
r=subprocess.run([exe],capture_output=True,text=True,check=True)
print(r.stdout,end='')
lines=[s for s in r.stderr.splitlines() if s.startswith('[ingress]')]
assert len(lines)==4, r.stderr
for i,line in enumerate(lines):
    fields=dict(token.split('=',1) for token in line.split()[1:])
    video=i>=2
    limit=2097152 if video else 1048576
    assert fields==dict(rejected='oversize',kind='video' if video else 'audio',
        incoming_bytes=str(limit+1),limit_bytes=str(limit),video_codec='HEVC',
        packet_codec='2' if video else '8',epoch='0',generation='7',generation_active='1',
        queue_packets='1',queue_bytes=str(limit),closing='0',accepting='1'), fields
    assert len(line)<512
assert len(r.stderr.splitlines())==7, r.stderr  # four oversize, two callbacks, one capacity
print('PASS: every oversize rejection logs bounded numeric fields and constant labels only')
