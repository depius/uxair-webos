#!/usr/bin/env python3
"""Exercise the actual HLS/MP4 fixture and prove its progressive control has H.264/AAC."""
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
root=Path(__file__).resolve().parents[1]
with socket.socket() as available:
    available.bind(('127.0.0.1',0)); port=available.getsockname()[1]
child=subprocess.Popen([sys.executable,'-u',str(root/'tools/direct-hls-fixture.py'),'--bind','127.0.0.1','--port',str(port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
def request(path,method='GET'):
    conn=http.client.HTTPConnection('127.0.0.1',port,timeout=2)
    try:
        conn.request(method,path);r=conn.getresponse();data=r.read(8*1024*1024+1)
        return r.status,data,r.getheader('Content-Length')
    finally:conn.close()
try:
    deadline=time.monotonic()+65
    while True:
        try:
            status,master,_=request('/master.m3u8?token=SECRET');assert status==200;break
        except OSError:
            assert child.poll() is None,'fixture exited during generation'
            assert time.monotonic()<deadline,'fixture startup deadline'
            time.sleep(.1)
    assert master.startswith(b'#EXTM3U') and b'1280x720' in master
    status,media,_=request('/media.m3u8');assert status==200 and b'#EXT-X-ENDLIST' in media
    segment=next(line.decode() for line in media.splitlines() if line and not line.startswith(b'#'))
    status,ts,_=request('/'+segment);assert status==200 and ts[:1]==b'G'
    status,mp4,length=request('/probe.mp4');assert status==200 and 0<len(mp4)<=8*1024*1024 and int(length)==len(mp4)
    assert b'ftyp' in mp4[:32]
    status,head,size=request('/probe.mp4','HEAD');assert status==200 and not head and int(size)==len(mp4)
    with tempfile.TemporaryDirectory() as folder:
        sample=Path(folder)/'probe.mp4';sample.write_bytes(mp4)
        info=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_entries','stream=codec_type,codec_name,width,height','-of','json',str(sample)],timeout=10))
        assert any(s.get('codec_name')=='h264' and s.get('width')==1280 and s.get('height')==720 for s in info['streams'])
        assert any(s.get('codec_name')=='aac' for s in info['streams'])
finally:
    child.terminate()
    try:output,errors=child.communicate(timeout=5)
    except subprocess.TimeoutExpired:child.kill();output,errors=child.communicate(timeout=5)
assert 'SECRET' not in output+errors and 'token=' not in output+errors
assert 'role=master' in output and 'role=media' in output and 'role=segment' in output and 'role=mp4' in output
print('PASS: actual bounded HLS/TS fixture, remuxed H.264/AAC MP4 and HEAD control; credential-free role/status logs')
