#!/usr/bin/env python3
"""Bind pinned UxPlay RTP and TEARDOWN callbacks to the admitted transport epoch."""
import hashlib
from pathlib import Path
import sys

PINS = {
    'raop.c': 'b6fc8f3ca31190900c64660a6e6c970381f5b48c',
    'raop_handlers.h': '62d8699be898985aaa53af8e68a0a016125597ed',
    'raop_rtp.c': '8c3307b3be736d984313a43d92fa3dd33fd18aab',
    'raop_rtp_mirror.c': '4ddedfdb16e7a35295bc5fd150a35284b15361c0',
}
CALL = 'raop->callbacks.video_reset(raop->callbacks.cls, RESET_TYPE_RTP_SHUTDOWN);'


def replace(source, before, after, count=1):
    if source.count(before) != count:
        raise ValueError('Pinned lifecycle site changed: ' + before)
    return source.replace(before, after)


def instrument_handler(source):
    # The fixture contains just the TEARDOWN handler.
    start = source.index('raop_handler_teardown(')
    if source.count(CALL) != 2 or CALL in source[:start]:
        raise ValueError('pinned RTP TEARDOWN sites changed')
    return replace(source, CALL,
        'if (conn->uxair_admitted) uxair_sender_video_reset(raop->callbacks.cls, RESET_TYPE_RTP_SHUTDOWN, conn->uxair_epoch);', 2)


def bind_setup(source):
    source = replace(source, """        if (raop->callbacks.report_client_request) {
            raop->callbacks.report_client_request(raop->callbacks.cls, deviceID, model, name, &admit_client);
        }""", """        const unsigned uxair_epoch = uxair_admit_transport(raop->callbacks.cls, deviceID, model, name, &admit_client);""")
    anchor = 'conn->raop_ntp, remote, conn->remotelen, aeskey);'
    return replace(source, anchor, anchor + """
        conn->uxair_epoch = uxair_epoch;
        conn->uxair_admitted = true;
        uxair_audio_set_epoch(conn->raop_rtp, uxair_epoch);
        uxair_video_set_epoch(conn->raop_rtp_mirror, uxair_epoch);
""")


def instrument_rtp(source, video, development=False):
    name = 'raop_rtp_mirror' if video else 'raop_rtp'
    kind = 'video' if video else 'audio'
    source = replace(source, '#include "raop.h"', '#include "raop.h"\n#include "receiver_lifecycle.h"')
    anchor = f'struct {name}_s {{'
    source = replace(source, anchor, anchor + '\n    unsigned uxair_epoch;')
    cls = f'{name}->callbacks.cls'
    epoch = f'{name}->uxair_epoch'
    if video:
        source = replace(source,
            f'{name}->callbacks.video_process({cls}, {name}->ntp, &video_data);',
            f'uxair_rtp_packet({cls}, 1, video_data.is_h265 ? 2 : 0, video_data.ntp_time_remote, video_data.data, video_data.data_len, {epoch});')
        source = replace(source, f'if (conn_reset&& {name}->callbacks.conn_reset) {{',
            'if (conn_reset) {')
        source = replace(source, f'{name}->callbacks.conn_reset({cls}, 1);',
            f'uxair_rtp_reset({cls}, 0, 1, {epoch});')
        source = replace(source, f'{name}->callbacks.video_reset({cls}, RESET_TYPE_RTP_SHUTDOWN);',
            f'uxair_rtp_reset({cls}, 1, RESET_TYPE_RTP_SHUTDOWN, {epoch});')
        source = replace(source, f'{name}->callbacks.video_pause({cls});',
            f'uxair_rtp_video_state({cls}, {epoch}, 0, 1);', 1 if development else 4)
        source = replace(source, f'{name}->callbacks.video_resume({cls});',
            f'uxair_rtp_video_state({cls}, {epoch}, 0, 0);')
        for value, boolean in enumerate(('false', 'true')):
            source = replace(source, f"""    if ({name}->callbacks.mirror_video_running) {{
        {name}->callbacks.mirror_video_running({cls}, {boolean});
    }}""", f'    uxair_rtp_video_state({cls}, {epoch}, 1, {value});')
    else:
        source = replace(source,
            f'{name}->callbacks.audio_process({cls}, {name}->ntp, &audio_data);',
            f'uxair_rtp_packet({cls}, 0, audio_data.ct, audio_data.ntp_time_remote, audio_data.data, audio_data.data_len, {epoch});')
        source = replace(source, f"""        if ({name}->callbacks.audio_flush) {{
            {name}->callbacks.audio_flush({cls});
        }}""", f'        uxair_rtp_audio_flush({cls}, {epoch});')
    return source + f"""
void uxair_{kind}_set_epoch(struct {name}_s *rtp, unsigned epoch) {{
    if (rtp) rtp->uxair_epoch = epoch;
}}
"""


def main():
    lib, input_c, input_mirror, out = map(Path, sys.argv[1:])
    originals = {}
    for name, expected in PINS.items():
        data = (lib / name).read_bytes()
        digest = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if digest != expected:
            raise SystemExit('Unpinned lifecycle input: ' + name)
        originals[name] = data.decode('utf-8')
    development = input_mirror.name == 'raop_rtp_mirror.c'
    source = input_c.read_text(encoding='utf-8')
    source = replace(source, '#include "raop_handlers.h"',
                     '#include "receiver_lifecycle.h"\n#include "uxair_raop_handlers.h"')
    source = replace(source, 'struct raop_conn_s {', 'struct raop_conn_s {\n    unsigned uxair_epoch;\n    bool uxair_admitted;')
    source = replace(source, """    /* Validate the callbacks structure */
    if (!callbacks->audio_process ||
        !callbacks->video_process) {
        return NULL;
    }
""", '    /* Media ingress uses the epoch-aware UxAir hooks. */\n')
    generated = {
        'lifecycle-raop.c': source,
        'uxair_raop_handlers.h': bind_setup(instrument_handler(originals['raop_handlers.h'])),
        'lifecycle-raop_rtp.c': instrument_rtp(originals['raop_rtp.c'], False),
        'lifecycle-raop_rtp_mirror.c': instrument_rtp(input_mirror.read_text(encoding='utf-8'), True, development),
    }
    out.mkdir(parents=True, exist_ok=True)
    for name, content in generated.items():
        (out / name).write_text(content, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
