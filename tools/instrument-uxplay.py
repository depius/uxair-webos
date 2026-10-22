#!/usr/bin/env python3
"""Create a development-only, exact-source-checked UxPlay request probe copy."""
import hashlib
import pathlib
import sys

PINNED_BLOBS = {
    'raop.c': 'b6fc8f3ca31190900c64660a6e6c970381f5b48c',
    'http_request.c': 'ec030c4a1dd5f10597f44b003c2adf079288c1ea',
    'httpd.c': '56a3742f5f9e1bb1ccbcc2879b4663d2e747c726',
    'raop_rtp_mirror.c': '4ddedfdb16e7a35295bc5fd150a35284b15361c0',
}
INCLUDE = '#include "raop.h"\n'
GATE = '    if (!cseq && !raop->hls_support && !ble) {\n'
PROBE = '''        int probe_body_length = -1;
        (void) http_request_get_data(request, &probe_body_length);
        const char *probe_type = http_request_get_header(request, "Content-Type");
        int probe_session = http_request_get_header(request, "X-Apple-Session-ID") != NULL;
        if (direct_info_eligible(method, url, protocol, probe_type,
                                 probe_body_length, probe_session)) {
            /* Development-only handshake experiment. Reuse the pinned RTSP
             * binary-plist info writer and its current DNS-SD feature mask.
             * Do not enter the HLS dispatcher or change connection ownership. */
            *response = http_response_create();
            if (*response) {
                char *info_data = NULL;
                int info_length = 0;
                http_response_init(*response, "HTTP/1.1", 200, "OK");
                raop_handler_info(conn, request, *response, &info_data, &info_length);
                if (info_data && info_length > 0) {
                    http_response_add_header(*response, "Server", "AirTunes/"GLOBAL_VERSION);
                    http_response_finish(*response, info_data, info_length);
                    free(info_data);
                    direct_request_diag_log_result(method, url, probe_type, protocol,
                                                   probe_body_length, probe_session, 200);
                    return;
                }
                free(info_data);
                http_response_init(*response, "HTTP/1.1", 500, "Internal Server Error");
                http_response_finish(*response, NULL, 0);
                direct_request_diag_log_result(method, url, probe_type, protocol,
                                               probe_body_length, probe_session, 500);
                return;
            }
        }
        direct_request_diag_log(method, url, probe_type, protocol,
                                probe_body_length, probe_session);
'''

DIRECT_GATE = r'''    if (!cseq && getenv("UXAIR_DIRECT_VIDEO") && !strcmp(getenv("UXAIR_DIRECT_VIDEO"), "1") &&
        (!strcmp(url,"/info") || !strcmp(url,"/info?txtAirPlay") || !strcmp(url,"/info?txtRAOP") || !strcmp(url,"/fp-setup2") || !strcmp(url,"/play") || !strncmp(url,"/scrub",6) || !strncmp(url,"/rate",5) ||
         !strcmp(url,"/stop") || !strcmp(url,"/server-info") || !strcmp(url,"/playback-info") || !strcmp(url,"/reverse") || !strcmp(url,"/action") || !strncmp(url,"/getProperty?",13) || !strncmp(url,"/setProperty?",13))) {
        if ((!strcmp(url,"/info") || !strcmp(url,"/info?txtAirPlay") || !strcmp(url,"/info?txtRAOP")) && !strcmp(method,"GET")) {
            char *data=NULL;int length=0;*response=http_response_create();http_response_init(*response,"HTTP/1.1",200,"OK");
            raop_handler_info(conn,request,*response,&data,&length);http_response_finish(*response,data,length);free(data);return;
        }
        if (!strcmp(url,"/server-info") && !strcmp(method,"GET")) {
            char *data=NULL;int length=0;*response=http_response_create();http_response_init(*response,"HTTP/1.1",200,"OK");
            http_handler_server_info(conn,request,*response,&data,&length);
            plist_t root=NULL;plist_from_xml(data,(uint32_t)length,&root);free(data);data=NULL;
            if(root){plist_dict_set_item(root,"features",plist_new_uint(dnssd_get_airplay_features(raop->dnssd)));uint32_t size=0;plist_to_xml(root,&data,&size);length=(int)size;plist_free(root);}
            http_response_finish(*response,data,length);free(data);return;
        }
        if (!strcmp(url,"/reverse")) {
            const char *upgrade=http_request_get_header(request,"Upgrade");
            const char *owner=http_request_get_header(request,"X-Apple-Session-ID");
            int accepted=!strcmp(method,"POST") && upgrade && !strcmp(upgrade,"PTTH/1.0") &&
                uxair_direct_reverse(raop->callbacks.cls,owner,httpd_get_connection_socket(raop->httpd,conn),(unsigned long)conn);
            *response=http_response_create();http_response_init(*response,"HTTP/1.1",accepted?101:409,accepted?"Switching Protocols":"Conflict");
            if(accepted){httpd_set_connection_type(raop->httpd,conn,CONNECTION_TYPE_PTTH);http_response_add_header(*response,"Connection","Upgrade");http_response_add_header(*response,"Upgrade","PTTH/1.0");}
            http_response_finish(*response,NULL,0);return;
        }
        int direct_length=0,output_length=0;char *output=NULL;const char *mime="text/parameters";
        const char *data=http_request_get_data(request,&direct_length);
        int status=uxair_direct_request(raop->callbacks.cls,http_request_get_header(request,"X-Apple-Session-ID"),
            method,url,http_request_get_header(request,"Content-Type"),data,direct_length,&output,&output_length,&mime);
        *response=http_response_create();
        http_response_init(*response,"HTTP/1.1",status,status==200?"OK":"Direct Video Request Failed");
        http_response_add_header(*response,"Content-Type",mime);
        http_response_finish(*response,output,output_length);free(output);return;
    }
'''


def replace_region(source, start, end, replacement):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError('pinned UxPlay HTTP logging site changed; refuse to instrument')
    first = source.index(start)
    last = source.index(end, first + len(start))
    return source[:first] + replacement + source[last:]


def instrument_raop(source):
    if source.count(INCLUDE) != 1 or source.count(GATE) != 1:
        raise ValueError('pinned UxPlay request gate changed; refuse to instrument')
    source=source.replace('    logger_log(raop->logger, LOGGER_DEBUG, "Destroying connection");',
        '    uxair_direct_lost(raop->callbacks.cls,(unsigned long)conn);\n    logger_log(raop->logger, LOGGER_DEBUG, "Destroying connection");',1)
    return source.replace(INCLUDE, INCLUDE + '#include "direct_request_diag.h"\n#include "direct_bridge.h"\n', 1).replace(
        GATE, DIRECT_GATE + GATE + PROBE, 1)


def instrument_http_request(source):
    anchor = '    request->data = realloc(request->data, request->datalen + length);\n'
    if source.count(anchor) != 1:
        raise ValueError('pinned UxPlay HTTP body parser changed; refuse to instrument')
    # This also prevents int overflow in request->datalen before realloc.
    guard = ('    if (request->datalen < 0 || length > 33554432U ||\n'
             '        (size_t) request->datalen > 33554432U - length) {\n'
             '        direct_request_diag_body_limit();\n'
             '        return -1;\n'
             '    }\n')
    small_guard = ('    const char *direct_path = http_request_get_url(request);\n'
                   '    if (direct_path && (!strcmp(direct_path, "/play") || !strcmp(direct_path, "/action")) &&\n'
                   '        ((size_t)request->datalen > 524288U || length > 524288U - (size_t)request->datalen)) return -1;\n')
    return '#include "direct_request_diag.h"\n' + source.replace(anchor, small_guard + guard + anchor, 1)


def instrument_httpd(source):
    # The pinned reader loops on EAGAIN until the first eight bytes arrive.
    # Feed each available fragment once; negotiated PTTH sockets already have
    # a known role and never need to probe an HTTP response prefix.
    source = source.replace('                    http_request_is_reverse(connection->request);\n',
                            '                    http_request_set_reverse(connection->request);\n', 1)
    source = replace_region(
        source,
        '            if (new_request) {\n                int readstart = 0;\n',
        '            if (http_request_is_reverse(connection->request)) {\n',
        '            int read_result = recv(connection->socket_fd, buffer, sizeof(buffer), 0);\n'
        '            if (read_result == 0) {\n'
        '                httpd_remove_connection(httpd, connection, 0);\n'
        '                continue;\n'
        '            }\n'
        '            if (read_result < 0) {\n'
        '                if (errno != SOCKET_ERRORNAME(EAGAIN) && errno != SOCKET_ERRORNAME(EWOULDBLOCK) && errno != SOCKET_ERRORNAME(EINTR))\n'
        '                    httpd_remove_connection(httpd, connection, SOCKET_GET_ERROR());\n'
        '                continue;\n'
        '            }\n'
        '            recv_datalen = read_result;\n')
    source = replace_region(
        source,
        '                if (recv_datalen && logger_debug) {\n',
        '                if (recv_datalen == 0) {\n',
        '                if (recv_datalen && logger_debug)\n'
        '                    logger_log(httpd->logger, LOGGER_DEBUG, "reverse HTTP response received (content omitted)");\n')
    source = replace_region(
        source,
        '            if (http_request_has_error(connection->request)) {\n',
        '            /* If request is finished, process and deallocate */\n',
        '            if (http_request_has_error(connection->request)) {\n'
        '                logger_log(httpd->logger, LOGGER_ERR, "httpd parse error (request data omitted)");\n'
        '                httpd_remove_connection(httpd, connection, 0);\n'
        '                continue;\n'
        '            }\n\n')
    source = replace_region(
        source,
        '                // Callback the received data to raop\n',
        '                httpd->callbacks.conn_request(connection->user_data, connection->request, &response);\n',
        '                // Request metadata is emitted only by the sanitized probe at the AirPlay gate.\n')
    return source


def harden_mirror(source):
    """Bound the pinned mirror packet parser before HEVC is advertised."""
    source = source.replace('#include "raop_rtp_mirror.h"\n',
                            '#include "raop_rtp_mirror.h"\n#include "hevc_config_guard.h"\n', 1)
    source = source.replace(
        '            int payload_size = byteutils_get_int(packet, 0);\n',
        '            int payload_size = byteutils_get_int(packet, 0);\n'
        '            if (payload_size < 0 || payload_size > 2097152 ||\n'
        '                (packet[4] == 0x00 && payload_size < 4)) {\n'
        '                logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "mirror payload bound failed");\n'
        '                conn_reset = true; break;\n'
        '            }\n', 1)
    source = source.replace(
        '                payload = malloc(payload_size);\n',
        '                payload = malloc(payload_size ? payload_size : 1);\n'
        '                if (!payload) { conn_reset = true; break; }\n', 1)
    source = source.replace(
        '                    payload_out = (unsigned char*) malloc(payload_size + sps_pps_len);\n'
        '                    if (!payload_out) {\n'
        '                        printf("Memory allocation failed (payload_out)\\n");\n'
        '                        exit(1);\n'
        '                    }\n',
        '                    if (sps_pps_len < 0 || payload_size > 2097152 - sps_pps_len) {\n'
        '                        conn_reset = true; break;\n'
        '                    }\n'
        '                    payload_out = (unsigned char*) malloc(payload_size + sps_pps_len);\n'
        '                    if (!payload_out) { conn_reset = true; break; }\n', 1)
    source = source.replace(
        '                    payload_out = (unsigned char*)  malloc(payload_size);\n'
        '                    payload_decrypted = payload_out;\n',
        '                    payload_out = (unsigned char*) malloc(payload_size);\n'
        '                    if (!payload_out) { conn_reset = true; break; }\n'
        '                    payload_decrypted = payload_out;\n', 1)
    source = source.replace(
        '                    int nc_len = byteutils_get_int_be(payload_decrypted, nalu_size);\n',
        '                    if (nalu_size > payload_size - 4) { valid_data = false; break; }\n'
        '                    int nc_len = byteutils_get_int_be(payload_decrypted, nalu_size);\n', 1)
    source = source.replace(
        '                if (!memcmp(payload + 4, hvc1, 4)) {\n',
        '                if (payload_size < 8) { conn_reset = true; break; }\n'
        '                if (!memcmp(payload + 4, hvc1, 4)) {\n', 1)
    source = source.replace(
        '                    short sps_size = byteutils_get_short_be(payload,6);\n',
        '                    if (payload_size < 11) { conn_reset = true; break; }\n'
        '                    short sps_size = byteutils_get_short_be(payload,6);\n'
        '                    if (sps_size < 2 || sps_size > payload_size - 11) { conn_reset = true; break; }\n', 1)
    source = source.replace(
        '                    short pps_size = byteutils_get_short_be(payload, sps_size + 9);\n',
        '                    short pps_size = byteutils_get_short_be(payload, sps_size + 9);\n'
        '                    if (pps_size < 2 || pps_size > payload_size - sps_size - 11) {\n'
        '                        conn_reset = true; break;\n'
        '                    }\n', 1)
    start = '                    unsigned char vps_start_code[] = { 0xa0, 0x00, 0x01, 0x00 };\n'
    end = '                } else {\n                    if (codec == VIDEO_CODEC_UNKNOWN)'
    replacement = '''                    /* Pinned hvc1 configuration: three tagged length-prefixed
                     * parameter sets at offset 0x75. Never trust lengths from
                     * the sender or inspect beyond the received payload. */
                    ux_hevc_config_sets sets;
                    if (!ux_hevc_config_parse(payload, (size_t)payload_size, &sets)) {
                        logger_log(raop_rtp_mirror->logger, LOGGER_ERR,
                                   "invalid HEVC parameter-set bounds or NAL type");
                        conn_reset = true; break;
                    }
                    sps_pps_len = (int) sets.annexb_size;
                    sps_pps = (unsigned char*) malloc(sets.annexb_size);
                    if (!sps_pps) { conn_reset = true; break; }
                    unsigned char *out = sps_pps;
                    for (int k = 0; k < 3; ++k) {
                        memcpy(out, nal_start_code, 4); out += 4;
                        memcpy(out, payload + sets.offset[k], sets.length[k]);
                        out += sets.length[k];
                    }
'''
    source = replace_region(source, start, end, replacement)
    source = source.replace(
        '            if (unsupported_codec) {\n                break;\n            }\n',
        '            if (unsupported_codec || conn_reset) {\n                break;\n            }\n', 1)
    source = source.replace(
        '    /* Close the stream file descriptor */\n',
        '    free(payload);\n    free(sps_pps);\n'
        '    /* Close the stream file descriptor */\n', 1)
    return source


def main():
    if len(sys.argv) != 3:
        raise SystemExit('Usage: instrument-uxplay.py PINNED_LIB_DIR OUTPUT_DIR')
    source_dir, output_dir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    # Verify every input before writing any generated source.
    originals = {}
    for name, expected in PINNED_BLOBS.items():
        source = (source_dir / name).read_bytes()
        blob = hashlib.sha1(b'blob ' + str(len(source)).encode() + b'\0' + source).hexdigest()
        if blob != expected:
            raise SystemExit(f'UxPlay {name} is not the pinned source: {blob}')
        originals[name] = source.decode('utf-8')
    generated = {
        'raop.c': instrument_raop(originals['raop.c']),
        'http_request.c': instrument_http_request(originals['http_request.c']),
        'httpd.c': instrument_httpd(originals['httpd.c']),
        'raop_rtp_mirror.c': harden_mirror(originals['raop_rtp_mirror.c']),
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, source in generated.items():
        (output_dir / name).write_text(source, encoding='utf-8', newline='\n')
    print('Development-only direct request probe and HTTP safety guards generated from pinned UxPlay sources')


if __name__ == '__main__':
    main()
