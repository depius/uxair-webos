/* Included by the pinned, generated raop.c after its native handlers. */
#include "direct_config.h"
#include "direct_bridge.h"
#include "direct_wire.h"
#include <strings.h>

static http_response_t *uxair_response(const char *protocol, const char *cseq, int status) {
    http_response_t *response = http_response_create();
    if (!response) return NULL;
    const char *reason = "Direct Video Request Failed";
    switch (status) {
    case 101: reason = "Switching Protocols"; break;
    case 200: reason = "OK"; break;
    case 400: reason = "Bad Request"; break;
    case 405: reason = "Method Not Allowed"; break;
    case 409: reason = "Conflict"; break;
    case 413: reason = "Payload Too Large"; break;
    case 501: reason = "Not Implemented"; break;
    case 503: reason = "Service Unavailable"; break;
    }
    http_response_init(response, protocol, status, reason);
    http_response_add_header(response, "Server", "AirTunes/" GLOBAL_VERSION);
    if (cseq) http_response_add_header(response, "CSeq", cseq);
    return response;
}

static int uxair_info_qualifiers(http_request_t *request) {
    int flags = uxair_info_query(http_request_get_url(request));
    int length = 0;
    const char *data = http_request_get_data(request, &length);
    if (!length) return flags;
    if (!data || length < 8 || length > 16384 || memcmp(data, "bplist00", 8)) return -1;
    plist_t root = NULL;
    plist_from_bin(data, (uint32_t)length, &root);
    plist_t list = plist_dict_get_item(root, "qualifier");
    if (!PLIST_IS_ARRAY(list) || plist_array_get_size(list) > 16) {
        plist_free(root);
        return -1;
    }
    for (uint32_t i = 0; i < plist_array_get_size(list); ++i) {
        char *key = NULL;
        plist_get_string_val(plist_array_get_item(list, i), &key);
        if (key && !strcmp(key, "txtAirPlay")) flags |= UX_TXT_AIRPLAY;
        if (key && !strcmp(key, "txtRAOP")) flags |= UX_TXT_RAOP;
        free(key);
    }
    plist_free(root);
    return flags ? flags : -1;
}

static void uxair_info_response(raop_conn_t *conn, http_request_t *request,
                                http_response_t **result, const char *cseq, int server_info) {
    const char *protocol = http_request_get_protocol(request);
    const int txt = server_info ? 0 : uxair_info_qualifiers(request);
    *result = uxair_response(protocol, cseq, txt < 0 ? 400 : 200);
    if (!*result) return;
    if (txt < 0) {
        http_response_finish(*result, NULL, 0);
        return;
    }
    char *data = NULL;
    int length = 0;
    if (server_info) {
        http_handler_server_info(conn, request, *result, &data, &length);
        // The pinned /server-info handler starts from a canned Apple TV mask.
        // Match the actual DNS-SD and /info capabilities instead.
        plist_t root = NULL;
        if (data && length > 0) plist_from_xml(data, (uint32_t)length, &root);
        free(data);
        data = NULL;
        uint32_t size = 0;
        if (root) {
            plist_dict_set_item(root, "features", plist_new_uint(dnssd_get_airplay_features(conn->raop->dnssd)));
            plist_to_xml(root, &data, &size);
            plist_free(root);
        }
        length = (int)size;
    } else if (txt) {
        plist_t root = plist_new_dict();
        if (txt & UX_TXT_AIRPLAY) {
            const char *record = dnssd_get_airplay_txt(conn->raop->dnssd, &length);
            if (record && length > 0) plist_dict_set_item(root, "txtAirPlay", plist_new_data(record, (uint64_t)length));
        }
        if (txt & UX_TXT_RAOP) {
            const char *record = dnssd_get_raop_txt(conn->raop->dnssd, &length);
            if (record && length > 0) plist_dict_set_item(root, "txtRAOP", plist_new_data(record, (uint64_t)length));
        }
        uint32_t size = 0;
        plist_to_bin(root, &data, &size);
        plist_free(root);
        length = (int)size;
        http_response_add_header(*result, "Content-Type", "application/x-apple-binary-plist");
    } else {
        raop_handler_info(conn, request, *result, &data, &length);
    }
    if (!data || length <= 0) {
        http_response_destroy(*result);
        *result = uxair_response(protocol, cseq, 503);
        length = 0;
    }
    if (*result) http_response_finish(*result, data, length);
    free(data);
}

static int uxair_dispatch_direct(raop_conn_t *conn, http_request_t *request,
                                http_response_t **result, const char *cseq, uxair_route_t *route) {
    if (!uxair_direct_enabled()) return 0;
    const uxair_video_route_t selected = uxair_video_route(request);
    if (selected == UX_VIDEO_NONE) return 0;
    if (selected == UX_VIDEO_INFO || selected == UX_VIDEO_SERVER_INFO) {
        *route = UX_ROUTE_DISCOVERY;
        uxair_info_response(conn, request, result, cseq, selected == UX_VIDEO_SERVER_INFO);
        return 1;
    }

    *route = UX_ROUTE_DIRECT;
    raop_t *raop = conn->raop;
    const char *method = http_request_get_method(request);
    const char *protocol = http_request_get_protocol(request);
    const char *owner = http_request_get_header(request, "X-Apple-Session-ID");
    int status = 400;
    if (selected == UX_VIDEO_REVERSE) {
        const char *upgrade = http_request_get_header(request, "Upgrade");
        if (strcmp(method, "POST")) status = 405;
        else if (upgrade && !strcasecmp(upgrade, "PTTH/1.0") && owner && *owner)
            status = uxair_direct_reverse(raop->callbacks.cls, owner,
                httpd_get_connection_socket(raop->httpd, conn), (unsigned long)conn) ? 101 : 409;
        *result = uxair_response(protocol, cseq, status);
        if (status == 101) {
            if (*result) {
                httpd_set_connection_type(raop->httpd, conn, CONNECTION_TYPE_PTTH);
                conn->connection_type = CONNECTION_TYPE_PTTH;
                http_response_add_header(*result, "Connection", "Upgrade");
                http_response_add_header(*result, "Upgrade", "PTTH/1.0");
            } else uxair_direct_lost(raop->callbacks.cls, (unsigned long)conn);
        }
        if (*result) http_response_finish(*result, NULL, 0);
        return 1;
    }

    int length = 0, output_length = 0;
    char *output = NULL;
    const char *type = "text/parameters";
    const char *data = http_request_get_data(request, &length);
    status = uxair_direct_request(raop->callbacks.cls, owner, method, http_request_get_url(request),
        http_request_get_header(request, "Content-Type"), data, length, &output, &output_length, &type);
    *result = uxair_response(protocol, cseq, status);
    if (*result) {
        http_response_add_header(*result, "Content-Type", type);
        http_response_finish(*result, output, output_length);
    }
    free(output);
    return 1;
}
