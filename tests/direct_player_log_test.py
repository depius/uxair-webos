#!/usr/bin/env python3
"""Inspect actual native mock execution logs, including firmware secret injection."""
from pathlib import Path
import sys
log = Path(sys.argv[1]).read_text()
for secret in ('PRIVATE', 'SECRET', 'http://', 'https://', 'token='):
    assert secret not in log, 'native diagnostic leaked a URL or injected credential'
for expected in ('stage=relay end result=1', 'stage=acb-initialize end result=1',
                 'stage=load end result=1', 'stage=load end result=0',
                 'event=0xb raw_value=77', 'failure=load-timeout stage=await-load',
                 'failure=duration-timeout', 'failure=native-error', 'failure=sink',
                 'failure=play-rejected', 'unload end result=1'):
    assert expected in log, expected
print('PASS: actual native phase/return/timeout/callback logs contain no URLs or injected credentials')
