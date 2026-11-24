#pragma once

#include <stddef.h>
#include "http_request.h"
#include "http_response.h"

typedef enum {
    UX_ROUTE_UNHANDLED, UX_ROUTE_REJECTED, UX_ROUTE_RAOP, UX_ROUTE_DISCOVERY, UX_ROUTE_DIRECT,
    UX_ROUTE_VIDEO_PAIRING
} uxair_route_t;

typedef enum {
    UX_VIDEO_NONE, UX_VIDEO_INFO, UX_VIDEO_SERVER_INFO, UX_VIDEO_REVERSE, UX_VIDEO_CONTROL, UX_VIDEO_FAIRPLAY
} uxair_video_route_t;

enum { UX_TXT_AIRPLAY = 1, UX_TXT_RAOP = 2 };

typedef struct {
    unsigned records[5];
    unsigned char alias_key[32];
    int alias_ready;
    unsigned connections, video, audio, pair_failures, unknown, admitted;
    unsigned crypto_attempts, crypto_upgrades, crypto_envelope_failures, crypto_tag_failures;
    unsigned crypto_records, crypto_requests, admission_attempts;
    unsigned reverse_upgrades, reverse_failures, reverse_events, crypto_closed;
} uxair_wire_stats_t;

int uxair_info_query(const char *path);
int uxair_parse_cseq(const char *value);
uxair_video_route_t uxair_video_route(http_request_t *request);
const char *uxair_wire_path(const char *path);
const char *uxair_wire_method(const char *method);
int uxair_wire_status(http_response_t *response);
void uxair_trace_exchange(uxair_wire_stats_t *stats, unsigned connection, unsigned port, unsigned sequence,
                          http_request_t *request, http_response_t *response,
                          uxair_route_t route, const char *reason, const char *auth);
void uxair_trace_summary(const uxair_wire_stats_t *stats, unsigned port, const char *point);
