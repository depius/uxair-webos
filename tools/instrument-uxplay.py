#!/usr/bin/env python3
"""Create a development-only, exact-source-checked UxPlay request probe copy."""
import hashlib
import importlib.util
import pathlib
import sys

PINNED_BLOBS = {
    'raop.c': 'b6fc8f3ca31190900c64660a6e6c970381f5b48c',
    'http_request.c': 'ec030c4a1dd5f10597f44b003c2adf079288c1ea',
    'httpd.c': '56a3742f5f9e1bb1ccbcc2879b4663d2e747c726',
    'raop_rtp_mirror.c': '4ddedfdb16e7a35295bc5fd150a35284b15361c0',
}
PAIRING_BLOB = 'c38f191cc31b1e59d75922d096b5d67d99c06890'
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


def replace_region(source, start, end, replacement):
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError('pinned UxPlay HTTP logging site changed; refuse to instrument')
    first = source.index(start)
    last = source.index(end, first + len(start))
    return source[:first] + replacement + source[last:]


def replace_once(source, anchor, replacement):
    if source.count(anchor) != 1:
        raise ValueError(f'pinned UxPlay anchor changed: {anchor!r}')
    return source.replace(anchor, replacement, 1)


def direct_request_parser(source):
    # llhttp can deliver the URL and protocol in different recv fragments.
    # Capture only the declared protocol span rather than reading past the URL.
    source = replace_once(source, '    strncpy(request->protocol, at + length + 1, 8);\n', '')
    source = replace_once(source, 'static int\non_header_field(', r"""static int
on_protocol(llhttp_t *parser, const char *at, size_t length)
{
    http_request_t *request = parser->data;
    size_t previous = strlen(request->protocol);
    if (previous > 4 || length > 4 - previous) return -1;
    memcpy(request->protocol + previous, at, length);
    request->protocol[previous + length] = '\0';
    return 0;
}

static int
on_header_field(""")
    source = replace_once(source, '    request->complete = 1;\n',
        '    if (strlen(request->protocol) != 4 || llhttp_get_http_major(parser) > 9 ||\n'
        '        llhttp_get_http_minor(parser) > 9) return -1;\n'
        '    request->protocol[4] = \'/\';\n'
        '    request->protocol[5] = \'0\' + llhttp_get_http_major(parser);\n'
        '    request->protocol[6] = \'.\';\n'
        '    request->protocol[7] = \'0\' + llhttp_get_http_minor(parser);\n'
        '    request->complete = 1;\n')
    source = replace_once(source, '    request->parser_settings.on_url = &on_url;\n',
        '    request->parser_settings.on_url = &on_url;\n'
        '    request->parser_settings.on_protocol = &on_protocol;\n')
    source = replace_once(source, '        if (!strcmp(request->headers[i], name)) {',
                          '        if (!strcasecmp(request->headers[i], name)) {')
    source = replace_once(source, '    int complete;\n',
        '    int complete, consumed;\n    bool encrypted, encrypted_reply;\n    size_t header_bytes;\n'
        '    const char *reply_rejection;\n')
    for field in ('on_url', 'on_header_field', 'on_header_value'):
        start = f'{field}(llhttp_t *parser, const char *at, size_t length)\n{{\n    http_request_t *request = parser->data;\n'
        source = replace_once(source, start, start +
            '    if (request->encrypted && (request->header_bytes += length + 1) > 8192) return -1;\n')
    source = replace_once(source, 'static int\non_message_complete', r"""static int
on_headers_complete(llhttp_t *parser)
{
    http_request_t *request = parser->data;
    bool fp2 = request->url && !strcmp(request->url, "/fp-setup2");
    if (!request->encrypted && !fp2) return 0;
    if (request->encrypted_reply) {
        if (strcmp(request->protocol, "HTTP") || llhttp_get_http_major(parser)!=1 ||
            llhttp_get_http_minor(parser)!=1) request->reply_rejection = "protocol";
        else if (llhttp_get_status_code(parser)!=200) request->reply_rejection = "status";
        else if (parser->upgrade) request->reply_rejection = "upgrade";
        else if (parser->flags & (F_CHUNKED | F_TRANSFER_ENCODING)) request->reply_rejection = "transfer-encoding";
        else if (parser->content_length) request->reply_rejection = "body-not-empty";
        return request->reply_rejection ? -1 : 1;
    }
    if (parser->flags & (F_CHUNKED | F_TRANSFER_ENCODING) ||
        parser->content_length > (fp2 ? 120U : 16384U) ||
        (request->encrypted && (strcmp(request->protocol, "HTTP") ||
            llhttp_get_http_major(parser) != 1 || llhttp_get_http_minor(parser) != 1 ||
            parser->method == HTTP_CONNECT || !request->url || request->url[0] != '/'))) return -1;
    /* Treat an encrypted /reverse Upgrade header as metadata for dispatch. */
    if (request->encrypted) parser->upgrade = 0;
    else if (parser->upgrade) return -1;
    return 0;
}

static int
on_message_complete""")
    source = replace_once(source, '    request->complete = 1;\n    return 0;\n',
        '    request->complete = 1;\n'
        '    if (request->encrypted || (request->url && !strcmp(request->url, "/fp-setup2"))) return HPE_PAUSED;\n'
        '    return 0;\n')
    source = replace_once(source, '    request->parser_settings.on_message_complete = &on_message_complete;\n',
        '    request->parser_settings.on_headers_complete = &on_headers_complete;\n'
        '    request->parser_settings.on_message_complete = &on_message_complete;\n')
    source = replace_once(source, '    int ret = llhttp_execute(&request->parser, data, datalen);\n',
        '    int ret = llhttp_execute(&request->parser, data, datalen);\n'
        '    if (ret == HPE_PAUSED && request->complete) {\n'
        '        request->consumed = (int)(llhttp_get_error_pos(&request->parser) - data);\n'
        '        llhttp_resume(&request->parser);\n'
        '        return HPE_OK;\n'
        '    }\n')
    source = replace_once(source, 'int\nhttp_request_is_complete', r"""int uxair_request_consumed(const http_request_t *request) { return request->consumed; }
const char *uxair_reply_rejection(const http_request_t *request) {
    return request->reply_rejection ? request->reply_rejection : "http-syntax";
}
void uxair_request_encrypted(http_request_t *request) { request->encrypted = true; }
void uxair_request_encrypted_reply(http_request_t *request) {
    request->encrypted = request->encrypted_reply = true;
    llhttp_init(&request->parser, HTTP_RESPONSE, &request->parser_settings);
    request->parser.data = request;
}

int
http_request_is_complete""")
    source = replace_once(source, '        free(request->url);\n',
        '        if (request->encrypted && request->url) OPENSSL_cleanse(request->url, strlen(request->url));\n'
        '        free(request->url);\n')
    source = replace_once(source, '            free(request->headers[i]);\n',
        '            if (request->encrypted && request->headers[i]) OPENSSL_cleanse(request->headers[i], strlen(request->headers[i]));\n'
        '            free(request->headers[i]);\n')
    source = replace_once(source, '        free(request->data);\n',
        '        if (request->encrypted && request->data) OPENSSL_cleanse(request->data, request->datalen);\n'
        '        free(request->data);\n')
    return '#include <strings.h>\n#include <openssl/crypto.h>\n' + source


def instrument_raop(source, direct_enabled=True):
    if source.count(INCLUDE) != 1 or source.count(GATE) != 1:
        raise ValueError('pinned UxPlay request gate changed; refuse to instrument')
    if not direct_enabled:
        return source.replace(INCLUDE, INCLUDE + '#include "direct_request_diag.h"\n', 1).replace(GATE, GATE + PROBE, 1)
    source = source.replace(INCLUDE, INCLUDE + '#include "direct_request_diag.h"\n', 1)
    source = replace_once(source, 'struct raop_s {',
        '#include "direct_wire.h"\n#include "direct_crypto.h"\n\nstruct raop_s {\n    uxair_wire_stats_t wire;')
    source = replace_once(source, 'struct raop_conn_s {',
        'struct raop_conn_s {\n'
        '    unsigned wire_id, wire_request;\n'
        '    char video_pair_owner[129];\n'
        '    pairing_session_t *video_verify;\n'
        '    fairplay_t *video_fairplay;\n'
        '    bool video_verified, video_pair_bound;\n'
        '    unsigned char video_secret[32], video_controller[ED25519_KEY_SIZE];\n'
        '    uxair_crypto_t video_crypto;')
    source = replace_once(source, '#include "http_handlers.h"\n',
                          '#include "http_handlers.h"\n#include "direct_dispatch.h"\n#include "direct_encrypted_dispatch.h"\n')
    source = replace_once(source,
        'conn_request(void *ptr, http_request_t *request, http_response_t **response) {',
        'conn_request_routed(void *ptr, http_request_t *request, http_response_t **response, uxair_route_t *route, const char **reason) {')
    for name in ('logger_debug', 'logger_debug_data'):
        suffix = '_DATA' if name.endswith('_data') else ''
        source = replace_once(source, f'    bool {name} = (logger_get_level(raop->logger) >= LOGGER_DEBUG{suffix});\n', '')
    source = replace_once(source, '        http_response_init(*response, protocol, 431, "Request Header Fields Too Large");\n',
        '        *route = UX_ROUTE_REJECTED;\n'
        '        *reason = "header-limit";\n'
        '        if (*response) {\n'
        '            http_response_init(*response, protocol, 431, "Request Header Fields Too Large");\n'
        '            http_response_finish(*response, NULL, 0);\n'
        '        }\n')
    source = replace_once(source, '        int cseq_val = parse_int(cseq_req);\n',
        '        int cseq_val = uxair_direct_enabled() ? uxair_parse_cseq(cseq_req) : parse_int(cseq_req);\n')
    source = replace_once(source,
        '            logger_log(raop->logger, LOGGER_ERR, "rejecting request with invalid CSeq value %s", cseq_req);\n',
        '            *route = UX_ROUTE_REJECTED;\n'
        '            *reason = "invalid-cseq";\n'
        '            if (uxair_direct_enabled()) {\n'
        '                *response = uxair_response(protocol, NULL, 400);\n'
        '                if (*response) http_response_finish(*response, NULL, 0);\n'
        '            }\n')
    source = replace_once(source, GATE, r"""    if (uxair_direct_enabled() && strcmp(protocol, "HTTP/1.1") && strcmp(protocol, "RTSP/1.0")) {
        *route = UX_ROUTE_REJECTED;
        *reason = "unsupported-protocol";
        *response = uxair_response(protocol, cseq, 505);
        if (*response) http_response_finish(*response, NULL, 0);
        return;
    }
    if (uxair_dispatch_direct(conn, request, response, cseq, route, reason)) return;
    if (conn->video_pair_bound) {
        *route = UX_ROUTE_REJECTED;
        *reason = "encrypted-upgrade-required";
        *response = uxair_response(protocol, cseq, 409);
        if (*response) {
            http_response_add_header(*response, "Content-Length", "0");
            http_response_finish(*response, NULL, 0);
        }
        return;
    }

""" + GATE + r"""        *route = UX_ROUTE_REJECTED;
        *reason = "no-cseq-route";
        if (uxair_direct_enabled()) {
            *response = uxair_response(protocol, cseq, 501);
            if (*response) http_response_finish(*response, NULL, 0);
            return;
        }
""" + PROBE)
    source = replace_region(source, r'    logger_log(raop->logger, LOGGER_DEBUG, "\n%s %s %s", method, url, protocol);' + '\n',
        '    raop_handler_t handler = NULL;\n',
        '    if (client_session_id && conn->client_session_id && strcmp(client_session_id, conn->client_session_id)) {\n'
        '        *route = UX_ROUTE_REJECTED;\n'
        '        *reason = "session-conflict";\n'
        '        http_response_init(*response, protocol, 409, "Conflicting Session");\n'
        '        goto finish;\n'
        '    }\n')
    source = replace_once(source, '    if (handler != NULL) {\n',
        '    if (handler != NULL) {\n'
        '        *route = strstr(url, "/info") && handler == &raop_handler_info ? UX_ROUTE_DISCOVERY : UX_ROUTE_RAOP;\n')
    source = replace_once(source,
        '        logger_log(raop->logger, LOGGER_INFO,\n                   "Unhandled Client Request: %s %s %s", method, url, protocol);\n',
        '        *route = UX_ROUTE_UNHANDLED;\n')
    source = replace_region(source, '    int len = 0;\n    const char *data = http_response_get_data(*response, &len);\n',
        'static void\nconn_destroy(void *ptr) {',
        '    free(response_data);\n}\n\n'
        'static void\nconn_request(void *ptr, http_request_t *request, http_response_t **response) {\n'
        '    raop_conn_t *conn = ptr;\n'
        '    uxair_wire_stats_t *wire = &conn->raop->wire;\n'
        '    if (!conn->wire_id) conn->wire_id = ++wire->connections;\n'
        '    uxair_route_t route = UX_ROUTE_UNHANDLED;\n'
        '    const char *reason = NULL;\n'
        '    conn_request_routed(ptr, request, response, &route, &reason);\n'
        '    const char *auth = conn->video_verified ? "video-verified" : conn->video_verify ? "proof-pending" :\n'
        '        conn->authenticated ? "digest-verified" : "unobserved";\n'
        '    uxair_trace_exchange(wire, conn->wire_id, conn->raop->port, ++conn->wire_request, request, *response, route, reason, auth);\n'
        '}\n\n')
    source = replace_once(source, '        raop_stop_httpd(raop);\n',
        '        raop_stop_httpd(raop);\n'
        '        uxair_trace_summary(&raop->wire, raop->port, "listener-stop");\n')
    return replace_once(source, '    logger_log(raop->logger, LOGGER_DEBUG, "Destroying connection");',
        '    uxair_video_cipher_log_close(conn);\n'
        '    uxair_direct_lost(raop->callbacks.cls,(unsigned long)conn,conn->video_crypto.reverse &&\n'
        '        conn->video_crypto.close_reason && strcmp(conn->video_crypto.close_reason,"reverse-session-ended"));\n'
        '    uxair_video_pairing_clear(conn);\n'
        '    logger_log(raop->logger, LOGGER_DEBUG, "Destroying connection");')


def instrument_http_request(source, direct_enabled=True):
    spec = importlib.util.spec_from_file_location('network_bounds', pathlib.Path(__file__).with_name('harden-network.py'))
    bounds = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bounds)
    guarded = bounds.bound_http_request(source, 'direct_request_diag_body_limit();')
    anchor = '    if (request->datalen < 0 || length > '
    guard = (
        '    const char *direct_path = http_request_get_url(request);\n'
        '    size_t direct_limit = direct_path && !strcmp(direct_path, "/play") ? 16384U :\n'
        '                          direct_path && !strcmp(direct_path, "/action") ? 524288U : 0;\n'
        '    if (direct_limit && (request->datalen < 0 || length > direct_limit ||\n'
        '        (size_t) request->datalen > direct_limit - length)) return -1;\n')
    if not direct_enabled:
        guard = ''
    guarded = guarded.replace(anchor, guard + anchor, 1)
    if direct_enabled:
        guarded = direct_request_parser(guarded)
    return '#include "direct_request_diag.h"\n' + guarded


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


def instrument_encrypted_httpd(source):
    source = replace_once(source, '#include "httpd.h"\n',
        '#include "httpd.h"\n#include "direct_crypto.h"\n')
    source = replace_once(source, '            if (httpd->connections[i].pending_remove) {\n',
        '            if (httpd->connections[i].pending_remove ||\n'
        '                uxair_video_cipher_expired(httpd->connections[i].user_data) ||\n'
        '                uxair_video_cipher_service(httpd->connections[i].user_data, httpd->connections[i].socket_fd)) {\n')
    source = replace_once(source, '            if (!connection->request) {\n',
        '            if (!connection->request && !uxair_video_cipher_active(connection->user_data)) {\n')
    source = replace_once(source, '            recv_datalen = read_result;\n',
        '            recv_datalen = read_result;\n'
        '            if (uxair_video_cipher_active(connection->user_data)) {\n'
        '                if (uxair_video_cipher_input(connection->user_data, connection->socket_fd, buffer, (size_t)recv_datalen))\n'
        '                    httpd_remove_connection(httpd, connection, 0);\n'
        '                continue;\n'
        '            }\n')
    source = replace_once(source, '                http_request_destroy(connection->request);\n',
        '                const int consumed = uxair_request_consumed(connection->request);\n'
        '                http_request_destroy(connection->request);\n')
    source = replace_once(source, '                        ret = send(connection->socket_fd, data+written, datalen-written, 0);\n',
        '                        ret = send(connection->socket_fd, data+written, datalen-written,\n'
        '                                   uxair_video_cipher_pending(connection->user_data) ? MSG_NOSIGNAL : 0);\n'
        '                        if (ret == 0) break;\n')
    source = replace_once(source, '                    if (http_response_get_disconnect(response)) {\n',
        '                    if (uxair_video_cipher_pending(connection->user_data)) {\n'
        '                        if (written != datalen || consumed <= 0 || consumed > recv_datalen) {\n'
        '                            uxair_video_cipher_abort(connection->user_data, "upgrade-ack-write-or-boundary");\n'
        '                            httpd_remove_connection(httpd, connection, 0);\n'
        '                        } else if (uxair_video_cipher_activate(connection->user_data) ||\n'
        '                            uxair_video_cipher_input(connection->user_data, connection->socket_fd,\n'
        '                                                    buffer + consumed, (size_t)(recv_datalen - consumed)))\n'
        '                            httpd_remove_connection(httpd, connection, 0);\n'
        '                    } else if (http_response_get_disconnect(response)) {\n')
    return source


def instrument_pairing(source):
    return '#include <openssl/crypto.h>\n' + replace_once(source,
        '        free(session);\n    }\n}',
        '        OPENSSL_cleanse(session->ecdh_secret, sizeof(session->ecdh_secret));\n'
        '        free(session);\n    }\n}')


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
    if len(sys.argv) not in (3, 4) or len(sys.argv) == 4 and sys.argv[3] != '--direct':
        raise SystemExit('Usage: instrument-uxplay.py PINNED_LIB_DIR OUTPUT_DIR [--direct]')
    direct_enabled = len(sys.argv) == 4
    source_dir, output_dir = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    # Verify every input before writing any generated source.
    originals = {}
    pins = dict(PINNED_BLOBS)
    if direct_enabled:
        pins['pairing.c'] = PAIRING_BLOB
    for name, expected in pins.items():
        source = (source_dir / name).read_bytes()
        blob = hashlib.sha1(b'blob ' + str(len(source)).encode() + b'\0' + source).hexdigest()
        if blob != expected:
            raise SystemExit(f'UxPlay {name} is not the pinned source: {blob}')
        originals[name] = source.decode('utf-8')
    httpd = instrument_httpd(originals['httpd.c'])
    generated = {
        'raop.c': instrument_raop(originals['raop.c'], direct_enabled),
        'http_request.c': instrument_http_request(originals['http_request.c'], direct_enabled),
        'httpd.c': instrument_encrypted_httpd(httpd) if direct_enabled else httpd,
        'raop_rtp_mirror.c': harden_mirror(originals['raop_rtp_mirror.c']),
    }
    if direct_enabled:
        generated['pairing.c'] = instrument_pairing(originals['pairing.c'])
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, source in generated.items():
        (output_dir / name).write_text(source, encoding='utf-8', newline='\n')
    print('Development-only direct request probe and HTTP safety guards generated from pinned UxPlay sources')


if __name__ == '__main__':
    main()
