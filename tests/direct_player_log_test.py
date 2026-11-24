#!/usr/bin/env python3
"""Inspect actual native mock execution logs, including firmware secret injection."""
from pathlib import Path
import sys
log = Path(sys.argv[1]).read_text()
bufferstream_log = Path(sys.argv[2]).read_text()
for secret in ('PRIVATE', 'SECRET', 'http://', 'https://', 'token='):
    assert secret not in log + bufferstream_log, 'native diagnostic leaked a URL or injected credential'
for expected in ('foreground begin available=1', 'foreground end result=1', 'foreground end result=0',
                 'name=program-count', 'name=buffer-range-info', 'name=streaming-info-periodic',
                 'contract=uri-envelope-hls', 'contract=uri-envelope-auto',
                 'load-return=1 readiness=await-callback network=unproven', 'stage=relay end result=1', 'stage=acb-initialize end result=1',
                 'stage=load end result=1', 'stage=load end result=0',
                 'event=0xb raw_value=77', 'failure=load-timeout stage=await-load',
                 'failure=duration-timeout', 'failure=native-error', 'failure=sink',
                 'failure=play-rejected', 'unload end result=1', 'request-plugins=absent',
                 'backend=native-uri stage=load exception=missing-plugins',
                 'backend=native-uri stage=load exception=std-exception',
                 'backend=native-uri stage=unload exception=std-exception'):
    assert expected in log, expected
for expected in ('contract=BUFFERSTREAM acb-role=MSE source=demuxed-hls video=H264 audio=PCM uri=absent request-plugins=absent',
                 'externalStreamingInfo.contents.codec.video:string',
                 'externalStreamingInfo.pcmInfo.sampleRate:real',
                 'backend=bufferstream stage=load exception=missing-plugins',
                 'backend=bufferstream stage=unload exception=std-exception'):
    assert expected in bufferstream_log, expected
print('PASS: actual native phase/return/timeout/callback logs contain no URLs or injected credentials')
