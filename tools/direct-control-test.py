#!/usr/bin/env python3
"""Controlled, PIN/URL-free native acceptance test; never modifies app state files."""
import argparse
import http.client
import plistlib
import socket
import threading
import time
import urllib.parse
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--receiver', required=True, help='http://TV-IP:receiver-port')
    parser.add_argument('--source', required=True, help='fixture master URL (not printed)')
    args = parser.parse_args()
    receiver = urllib.parse.urlsplit(args.receiver)
    if receiver.scheme != 'http' or not receiver.hostname or not receiver.port:
        parser.error('receiver must be http://TV-IP:port')
    owner = str(uuid.uuid4())
    reverse = socket.create_connection((receiver.hostname, receiver.port), timeout=5)
    reverse.sendall((f'POST /reverse HTTP/1.1\r\nHost: {receiver.hostname}\r\nX-Apple-Session-ID: {owner}\r\nConnection: Upgrade\r\nUpgrade: PTTH/1.0\r\nContent-Length: 0\r\n\r\n').encode())
    data = b''
    while b'\r\n\r\n' not in data:
        block=reverse.recv(4096)
        if not block: raise RuntimeError('reverse connection closed')
        data += block
        if len(data)>8192: raise RuntimeError('oversized reverse response')
    if not data.startswith(b'HTTP/1.1 101'): raise RuntimeError('reverse upgrade failed')
    print('Reverse upgrade: 101')
    stop = threading.Event()
    def events():
        pending = data.split(b'\r\n\r\n',1)[1]
        reverse.settimeout(1)
        while not stop.is_set():
            try:
                block = reverse.recv(4096)
                if not block: break
                pending += block
                while b'\r\n\r\n' in pending:
                    header, body = pending.split(b'\r\n\r\n',1)
                    length = next((int(line.split(b':',1)[1]) for line in header.split(b'\r\n') if line.lower().startswith(b'content-length:')),0)
                    if length>524288: raise RuntimeError('oversized event')
                    if len(body)<length: break
                    event = plistlib.loads(body[:length]);pending = body[length:]
                    if event.get('state') in ('loading','playing','paused','stopped'): print('Event:',event['state'])
                    reverse.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n')
                if len(pending)>532480: raise RuntimeError('oversized event buffer')
            except socket.timeout: continue
            except OSError: break
    thread=threading.Thread(target=events,daemon=True);thread.start()
    def command(method,path,body=b'',content_type='text/parameters'):
        conn=http.client.HTTPConnection(receiver.hostname,receiver.port,timeout=5)
        try:
            conn.request(method,path,body,{'X-Apple-Session-ID':owner,'Content-Type':content_type})
            result=conn.getresponse();payload=result.read(65536)
            print(method,path.split('?')[0],result.status)
            if result.status!=200: raise RuntimeError('control request failed')
            if path=='/playback-info':
                info=plistlib.loads(payload)
                print({key:info.get(key) for key in ('readyToPlay','position','duration','rate')})
        finally: conn.close()
    try:
        command('POST','/play',f'Content-Location: {args.source}\r\nStart-Position: 0\r\n'.encode())
        for _ in range(3): time.sleep(1);command('GET','/playback-info')
        command('POST','/rate?value=0');time.sleep(1);command('GET','/playback-info')
        command('POST','/rate?value=1');time.sleep(1)
        command('POST','/scrub?position=5');time.sleep(1);command('GET','/playback-info')
    finally:
        try: command('POST','/stop')
        finally: stop.set();reverse.close();thread.join(timeout=2)
    print('Report picture/audio and whether progress uses seconds; host controls alone cannot verify either.')


if __name__=='__main__': main()
