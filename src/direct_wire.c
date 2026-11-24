#include "direct_wire.h"
#include <plist/plist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int path_is(const char *path, const char *name) {
    size_t n = strlen(name);
    return path && !strncmp(path, name, n) && (path[n] == '\0' || path[n] == '?');
}

int uxair_info_query(const char *path) {
    if (!path_is(path, "/info")) return -1;
    if (!strcmp(path, "/info")) return 0;
    const char *query = path + 6;
    if (!strcmp(query, "txtAirPlay")) return UX_TXT_AIRPLAY;
    if (!strcmp(query, "txtRAOP")) return UX_TXT_RAOP;
    if (!strcmp(query, "txtAirPlay?txtRAOP") || !strcmp(query, "txtAirPlay&txtRAOP") ||
        !strcmp(query, "txtRAOP?txtAirPlay") || !strcmp(query, "txtRAOP&txtAirPlay"))
        return UX_TXT_AIRPLAY | UX_TXT_RAOP;
    return -1;
}

uxair_video_route_t uxair_video_route(http_request_t *request) {
    const char *path = http_request_get_url(request);
    const char *method = http_request_get_method(request);
    const char *protocol = http_request_get_protocol(request);
    if (!path || !method || !protocol ||
        (strcmp(protocol, "HTTP/1.1") && strcmp(protocol, "RTSP/1.0"))) return UX_VIDEO_NONE;
    if (!strcmp(method, "GET") && uxair_info_query(path) >= 0) return UX_VIDEO_INFO;
    if (!strcmp(method, "GET") && !strcmp(path, "/server-info")) return UX_VIDEO_SERVER_INFO;
    if (!strcmp(path, "/reverse")) return UX_VIDEO_REVERSE;
    const char *controls[] = {"/play", "/stop", "/playback-info", "/scrub", "/rate",
        "/action", "/getProperty", "/setProperty", "/fp-setup2"};
    for (size_t i = 0; i < sizeof(controls)/sizeof(*controls); ++i)
        if (path_is(path, controls[i])) return UX_VIDEO_CONTROL;
    return UX_VIDEO_NONE;
}

static const char *known_path(const char *path) {
    const char *names[] = {"/info", "/server-info", "/reverse", "/play", "/playback-info", "/stop",
        "/scrub", "/rate", "/getProperty", "/setProperty", "/action", "/event", "/audioMode",
        "/command", "/feedback", "/pair-pin-start", "/pair-setup-pin", "/pair-setup", "/pair-verify",
        "/pair-add", "/pair-remove", "/pair-list", "/fp-setup", "/fp-setup2", "/photo",
        "/slideshows", "/configure", "/auth-setup"};
    for (size_t i = 0; i < sizeof(names)/sizeof(*names); ++i)
        if (path_is(path, names[i])) return names[i];
    if (path && !strcmp(path, "*")) return "*";
    return "redacted";
}

static const char *known_method(const char *value) {
    const char *names[] = {"GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS", "ANNOUNCE",
        "SETUP", "RECORD", "FLUSH", "FLUSHBUFFERED", "TEARDOWN", "SET_PARAMETER", "GET_PARAMETER",
        "SETPEERS", "SETPEERSX", "SETRATEANCHORTIME", "SETRATEANCHORTIM", "PAUSE"};
    for (size_t i = 0; value && i < sizeof(names)/sizeof(*names); ++i)
        if (!strcmp(value, names[i])) return names[i];
    return "other";
}

static int response_status(http_response_t *response) {
    if (!response) return 0;
    int length = 0;
    const char *data = http_response_get_data(response, &length);
    if (!data || length < 13 || data[8] != ' ' || data[12] != ' ') return 0;
    int status = 0;
    for (unsigned i = 9; i < 12; ++i) {
        if (data[i] < '0' || data[i] > '9') return 0;
        status = status * 10 + data[i] - '0';
    }
    return status;
}

static const char *plist_label(plist_t item, const char *const *names, size_t count) {
    if (!PLIST_IS_STRING(item)) return "unknown";
    char *value = NULL;
    plist_get_string_val(item, &value);
    const char *result = "unknown";
    for (size_t i = 0; value && i < count; ++i)
        if (!strcmp(value, names[i])) { result = names[i]; break; }
    free(value);
    return result;
}

static unsigned stream_types(plist_t plist) {
    unsigned types = 0;
    plist_t streams = plist_dict_get_item(plist, "streams");
    if (!PLIST_IS_ARRAY(streams)) return 0;
    const uint32_t count = plist_array_get_size(streams);
    for (uint32_t i = 0; i < count && i < 8; ++i) {
        plist_t type = plist_dict_get_item(plist_array_get_item(streams, i), "type");
        if (!PLIST_IS_UINT(type)) continue;
        uint64_t value = 0;
        plist_get_uint_val(type, &value);
        switch (value) {
        case 96: types |= 1; break;    /* realtime audio */
        case 103: types |= 2; break;   /* buffered audio */
        case 110: types |= 4; break;   /* mirroring */
        case 130: types |= 8; break;   /* remote control */
        default: types |= 16; break;
        }
    }
    return types;
}

static const char *body_type(const char *body, int bytes, const char *type) {
    if (body && bytes >= 8 && !memcmp(body, "bplist00", 8)) return "binary-plist";
    if (!type) return "none";
    if (!strncasecmp(type, "text/parameters", strlen("text/parameters"))) return "parameters";
    if (!strncasecmp(type, "application/octet-stream", strlen("application/octet-stream"))) return "binary";
    if (!strncasecmp(type, "text/x-apple-plist+xml", strlen("text/x-apple-plist+xml")) ||
        !strncasecmp(type, "application/x-apple-plist+xml", strlen("application/x-apple-plist+xml"))) return "xml-plist";
    return "other";
}

static int trace_budget(unsigned group) {
    /* Independent budgets keep routine polling from hiding new negotiations. */
    static unsigned records[3];
    const unsigned limits[] = {128, 32, 64};
    unsigned index = __atomic_load_n(&records[group], __ATOMIC_RELAXED);
    while (index <= limits[group] && !__atomic_compare_exchange_n(&records[group], &index, index + 1, 1,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (index == limits[group])
        fprintf(stderr, "[airplay-wire] group=%u further_requests=omitted\n", group);
    return index < limits[group];
}

void uxair_trace_exchange(http_request_t *request, http_response_t *response, uxair_route_t route) {
    const char *raw_path = http_request_get_url(request);
    const char *path = known_path(raw_path);
    const char *method = known_method(http_request_get_method(request));
    const char *protocol = http_request_get_protocol(request);
    if (!protocol || (strcmp(protocol, "HTTP/1.1") && strcmp(protocol, "RTSP/1.0"))) protocol = "other";
    const int is_setup = !strcmp(method, "SETUP");
    const int is_command = !strcmp(path, "/command");
    const int is_mode = !strcmp(path, "/audioMode");
    unsigned group = !strcmp(path, "/feedback") || !strcmp(path, "/playback-info") ||
        !strcmp(method, "GET_PARAMETER") || !strcmp(method, "SET_PARAMETER") ? 1 :
        route == UX_ROUTE_DIRECT || route == UX_ROUTE_DISCOVERY || is_setup || is_command || is_mode ? 0 : 2;
    if (!trace_budget(group)) return;

    int bytes = 0;
    const char *body = http_request_get_data(request, &bytes);
    const char *format = body_type(body, bytes, http_request_get_header(request, "Content-Type"));
    const char *detail = "none";
    unsigned streams = 0;
    const char *category = route == UX_ROUTE_DISCOVERY ? "discovery" :
        route == UX_ROUTE_DIRECT ? "direct-video" : route == UX_ROUTE_RAOP ? "raop" : "unknown";
    if (is_command || is_mode) category = "media-control";
    if (!strcmp(format, "binary-plist") && bytes <= 16384 && (is_command || is_mode || is_setup)) {
        plist_t plist = NULL;
        plist_from_bin(body, (uint32_t)bytes, &plist);
        if (PLIST_IS_DICT(plist)) {
            if (is_command) {
                const char *const names[] = {"updateMRSupportedCommands", "updateMRNowPlayingInfo",
                    "updateMRPlaybackState", "updateMRNowPlayingClient", "sendMediaRemoteCommand", "fadeAudio"};
                detail = plist_label(plist_dict_get_item(plist, "type"), names, sizeof(names)/sizeof(*names));
            } else if (is_mode) {
                const char *const names[] = {"default", "moviePlayback"};
                detail = plist_label(plist_dict_get_item(plist, "audioMode"), names, sizeof(names)/sizeof(*names));
            } else {
                streams = stream_types(plist);
                category = streams & 4 ? "mirroring" : streams & 2 ? "buffered-audio" :
                    streams & 1 ? "realtime-audio" : streams & 8 ? "remote-control" : "transport-setup";
            }
        } else detail = "invalid-plist";
        plist_free(plist);
    }
    const char *const routes[] = {"unhandled", "rejected", "raop", "discovery", "direct"};
    int query = uxair_info_query(raw_path);
    fprintf(stderr, "[airplay-wire] method=%s path=%s protocol=%s route=%s status=%d category=%s "
            "cseq=%d session_header=%d query=%s txt=%d content=%s bytes=%d detail=%s stream_mask=0x%x\n",
            method, path, protocol, routes[route], response_status(response), category,
            http_request_get_header(request, "CSeq") != NULL,
            http_request_get_header(request, "X-Apple-Session-ID") != NULL,
            raw_path && strchr(raw_path, '?') ? "present" : "none", query < 0 ? 0 : query,
            format, bytes, detail, streams);
}
