#!/usr/bin/env python3
"""Tag sender RTP shutdown at its pinned origin without logging request data."""
import hashlib
from pathlib import Path
import sys

PINS={'raop.c':'b6fc8f3ca31190900c64660a6e6c970381f5b48c',
      'raop_handlers.h':'62d8699be898985aaa53af8e68a0a016125597ed'}
CALL='raop->callbacks.video_reset(raop->callbacks.cls, RESET_TYPE_RTP_SHUTDOWN);'
def instrument_handler(source):
    # Both sites are within the TEARDOWN handler; never retag mirror errors.
    start=source.index('raop_handler_teardown(')
    if source.count(CALL)!=2 or source[:start].count(CALL):
        raise ValueError('pinned RTP TEARDOWN sites changed')
    return source.replace(CALL,'uxair_sender_video_reset(raop->callbacks.cls, RESET_TYPE_RTP_SHUTDOWN);')
def main():
    lib,input_c,out=map(Path,sys.argv[1:]);out.mkdir(parents=True,exist_ok=True)
    for name,digest in PINS.items():
        data=(lib/name).read_bytes()
        if hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()!=digest:
            raise SystemExit('Unpinned lifecycle input: '+name)
    source=input_c.read_text(encoding='utf-8')
    anchor='#include "raop_handlers.h"'
    if source.count(anchor)!=1:raise SystemExit('Lifecycle include changed')
    source=source.replace(anchor,'#include "receiver_lifecycle.h"\n#include "uxair_raop_handlers.h"')
    (out/'lifecycle-raop.c').write_text(source,encoding='utf-8',newline='\n')
    (out/'uxair_raop_handlers.h').write_text(instrument_handler((lib/'raop_handlers.h').read_text(encoding='utf-8')),encoding='utf-8',newline='\n')
if __name__=='__main__':main()
