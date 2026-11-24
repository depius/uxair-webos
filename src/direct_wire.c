#include "direct_wire.h"
#include "direct_wire_shapes.h"
#include <limits.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int uxair_info_query(const char *path) {
    if (!uxair_control_path(path, "/info")) return -1;
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
    if (!strcmp(protocol, "HTTP/1.1") && !strcmp(path, "/fp-setup")) return UX_VIDEO_FAIRPLAY;
    const char *controls[] = {"/play", "/stop", "/playback-info", "/scrub", "/rate",
        "/action", "/getProperty", "/setProperty", "/fp-setup2"};
    for (size_t i = 0; i < sizeof(controls)/sizeof(*controls); ++i)
        if (uxair_control_path(path, controls[i])) return UX_VIDEO_CONTROL;
    return UX_VIDEO_NONE;
}

const char *uxair_wire_path(const char *path) {
    const char *names[] = {"/info", "/server-info", "/reverse", "/play", "/playback-info", "/stop",
        "/scrub", "/rate", "/getProperty", "/setProperty", "/action", "/event", "/audioMode",
        "/command", "/feedback", "/pair-pin-start", "/pair-setup-pin", "/pair-setup", "/pair-verify",
        "/pair-add", "/pair-remove", "/pair-list", "/fp-setup", "/fp-setup2", "/photo",
        "/slideshows", "/configure", "/auth-setup"};
    for (size_t i = 0; i < sizeof(names)/sizeof(*names); ++i)
        if (uxair_control_path(path, names[i])) return names[i];
    if (path && !strcmp(path, "*")) return "*";
    return "redacted";
}

const char *uxair_wire_method(const char *value) {
    const char *names[] = {"GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS", "ANNOUNCE",
        "SETUP", "RECORD", "FLUSH", "FLUSHBUFFERED", "TEARDOWN", "SET_PARAMETER", "GET_PARAMETER",
        "SETPEERS", "SETPEERSX", "SETRATEANCHORTIME", "SETRATEANCHORTIM", "PAUSE"};
    for (size_t i = 0; value && i < sizeof(names)/sizeof(*names); ++i)
        if (!strcmp(value, names[i])) return names[i];
    return "other";
}

int uxair_wire_status(http_response_t *response) {
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

int uxair_parse_cseq(const char *value) {
    const unsigned char *digits = (const unsigned char *)value;
    int result = 0;
    if (!digits || *digits < '0' || *digits > '9') return -1;
    do {
        const int digit = *digits++ - '0';
        if (result > (INT_MAX - digit) / 10) return -1;
        result = result * 10 + digit;
    } while (*digits >= '0' && *digits <= '9');
    while (*digits == ' ' || *digits == '\t') ++digits;
    return *digits ? -1 : result;
}

static void session_alias(uxair_wire_stats_t *stats, const char *value, char alias[17]) {
    if (!value || !*value || strlen(value) > 1024) return;
    if (!stats->alias_ready) {
        if (RAND_bytes(stats->alias_key, sizeof(stats->alias_key)) != 1) return;
        stats->alias_ready = 1;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    if (!HMAC(EVP_sha256(), stats->alias_key, sizeof(stats->alias_key),
              (const unsigned char *)value, strlen(value), digest, NULL)) return;
    for (unsigned i = 0; i < 8; ++i) snprintf(alias + 2*i, 3, "%02x", (unsigned)digest[i]);
}

static int trace_budget(uxair_wire_stats_t *stats, unsigned port, unsigned group) {
    const unsigned limits[] = {64, 48, 32, 32, 48};
    unsigned *count = &stats->records[group];
    if (*count < limits[group]) { ++*count; return 1; }
    if (*count == limits[group]) {
        ++*count;
        fprintf(stderr, "[airplay-wire] port=%u group=%u further_requests=omitted\n", port, group);
    }
    return 0;
}

void uxair_trace_summary(const uxair_wire_stats_t *stats, unsigned port, const char *point) {
    if (!stats->connections) return;
    fprintf(stderr, "[airplay-wire-summary] point=%s service=airplay+raop port=%u connections=%u video=%u "
            "audio_setup=%u pairing_failures=%u unknown=%u admitted=%u encrypted_attempts=%u "
            "encrypted_upgrades=%u envelope_failures=%u gcm_auth_failures=%u encrypted_records=%u "
            "encrypted_requests=%u admission_attempts=%u\n", point, port,
            stats->connections, stats->video, stats->audio, stats->pair_failures, stats->unknown, stats->admitted,
            stats->crypto_attempts, stats->crypto_upgrades, stats->crypto_envelope_failures,
            stats->crypto_tag_failures, stats->crypto_records, stats->crypto_requests, stats->admission_attempts);
}

void uxair_trace_exchange(uxair_wire_stats_t *stats, unsigned connection, unsigned port, unsigned sequence,
                          http_request_t *request, http_response_t *response,
                          uxair_route_t route, const char *reason, const char *auth) {
    const char *raw_path = http_request_get_url(request);
    const char *path = uxair_wire_path(raw_path);
    const char *method = uxair_wire_method(http_request_get_method(request));
    const char *protocol = http_request_get_protocol(request);
    if (!protocol || (strcmp(protocol, "HTTP/1.1") && strcmp(protocol, "RTSP/1.0") &&
        strcmp(protocol, "HTTP/1.0") && strcmp(protocol, "RTSP/2.0"))) protocol = "other";
    const int is_setup = !strcmp(method, "SETUP");
    const int is_command = !strcmp(path, "/command");
    const int is_mode = !strcmp(path, "/audioMode");
    const int is_pair = !strncmp(path, "/pair-", 6) || !strncmp(path, "/fp-setup", 9);
    const int status = uxair_wire_status(response);
    const int disconnected = response && http_response_get_disconnect(response);
    const int is_video = route == UX_ROUTE_DIRECT;
    const int admitted = is_video && !strcmp(method, "POST") && !strcmp(raw_path, "/play") && status == 200;
    const int failed_pair = is_pair && (status >= 400 || !status || disconnected);
    const int unknown = route == UX_ROUTE_UNHANDLED || status == 501 || status == 505;
    if (is_video) ++stats->video;
    if (failed_pair) ++stats->pair_failures;
    if (unknown) ++stats->unknown;
    if (admitted) ++stats->admitted;
    if (is_video && !strcmp(method, "POST") && !strcmp(raw_path, "/play")) ++stats->admission_attempts;

    int bytes = 0;
    const char *body = http_request_get_data(request, &bytes);
    const char *format = body_type(body, bytes, http_request_get_header(request, "Content-Type"));
    const char *detail = "none";
    unsigned streams = 0;
    const char *category = route == UX_ROUTE_DISCOVERY ? "discovery" :
        route == UX_ROUTE_DIRECT ? "direct-video" : is_pair ? "pairing" :
        route == UX_ROUTE_RAOP ? "raop" : "unknown";
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
    const int audio = !(streams & 4) && (streams & 3) && route == UX_ROUTE_RAOP && status == 200;
    if (audio) ++stats->audio;
    if ((is_video && stats->video == 1) || (audio && stats->audio == 1) ||
        (failed_pair && stats->pair_failures == 1) || (admitted && stats->admitted == 1))
        uxair_trace_summary(stats, port, "milestone");

    const int polling = !strcmp(path, "/feedback") || !strcmp(path, "/playback-info") ||
        !strcmp(method, "GET_PARAMETER") || !strcmp(method, "SET_PARAMETER");
    const unsigned group = is_pair ? 0 : polling ? 1 : is_video ? 2 :
        route == UX_ROUTE_DISCOVERY || is_setup || is_mode ? 3 : 4;
    if (!trace_budget(stats, port, group)) return;

    const char *const routes[] = {"unhandled", "rejected", "raop", "discovery", "direct", "video-pairing"};
    const char *cseq = http_request_get_header(request, "CSeq");
    const char *session = http_request_get_header(request, "Session");
    const char *apple_session = http_request_get_header(request, "X-Apple-Session-ID");
    char alias[17] = "-", rtsp_alias[17] = "-";
    session_alias(stats, apple_session, alias);
    session_alias(stats, session, rtsp_alias);
    if (!reason) {
        if (disconnected) reason = "handler-disconnect";
        else if (!status) reason = "no-response";
        else if (route == UX_ROUTE_UNHANDLED) reason = "no-handler";
        else if (status >= 400) reason = "handler-rejected";
        else reason = "handled";
    }
    const char *query_part = raw_path ? strchr(raw_path, '?') : NULL;
    const char *control = "none", *control_shape = "none";
    char media_array[1024] = "-";
    int property = !strcmp(path, "/setProperty") || !strcmp(path, "/getProperty");
    int scrub = !strcmp(path, "/scrub");
    if (property || scrub) {
        plist_t root = uxair_control_plist(body, (size_t)bytes);
        if (property) {
            const char *name = query_part ? uxair_known_property(query_part + 1) : NULL;
            control = name ? name : query_part ? "unknown-property" : "missing-property";
            if (PLIST_IS_DICT(root)) {
                plist_t value = plist_dict_get_item(root, "value");
                control_shape = uxair_property_value_shape(value);
                if (PLIST_IS_ARRAY(value))
                    uxair_media_array_shape(value, media_array, sizeof(media_array));
            } else if (bytes) control_shape = "not-binary-dict";
        } else {
            control = !query_part ? "no-position-query" :
                !strncmp(query_part, "?position=", 10) ? "position-query" : "other-query";
            if (PLIST_IS_DICT(root)) control_shape = PLIST_IS_STRING(plist_dict_get_item(root, "uuid")) ?
                "dict-with-uuid" : "dict";
            else if (bytes) control_shape = "not-binary-dict";
        }
        plist_free(root);
    }
    const char *envelope = !strcmp(path, "/fp-setup2") ? uxair_fp_envelope_shape(body, (size_t)bytes) : "-";
    const char *pd = is_pair ? http_request_get_header(request, "X-Apple-PD") : NULL;
    int query = uxair_info_query(raw_path);
    fprintf(stderr, "[airplay-wire] method=%s path=%s protocol=%s route=%s status=%d category=%s "
            "conn=%u port=%u reason=%s auth=%s cseq=%s session=%d rtsp_sid=%s "
            "session_header=%d sid=%s query=%s txt=%d content=%s bytes=%d detail=%s stream_mask=0x%x queued=%d seq=%u control=%s control_body=%s media_array=%s fp_envelope=%s pair_pd=%s\n",
            method, path, protocol, routes[route], status, category, connection, port, reason, auth,
            !cseq ? "absent" : uxair_parse_cseq(cseq) < 0 ? "invalid" : "decimal",
            session != NULL, rtsp_alias, apple_session != NULL, alias,
            raw_path && strchr(raw_path, '?') ? "present" : "none", query < 0 ? 0 : query,
            format, bytes, detail, streams, admitted, sequence, control, control_shape, media_array, envelope,
            !pd ? "absent" : !strcmp(pd, "1") ? "1" : "other");
}
