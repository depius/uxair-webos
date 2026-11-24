#pragma once

#include <stddef.h>
#include "http_request.h"
#include "http_response.h"

typedef enum {
    UX_ROUTE_UNHANDLED, UX_ROUTE_REJECTED, UX_ROUTE_RAOP, UX_ROUTE_DISCOVERY, UX_ROUTE_DIRECT
} uxair_route_t;

typedef enum {
    UX_VIDEO_NONE, UX_VIDEO_INFO, UX_VIDEO_SERVER_INFO, UX_VIDEO_REVERSE, UX_VIDEO_CONTROL
} uxair_video_route_t;

enum { UX_TXT_AIRPLAY = 1, UX_TXT_RAOP = 2 };

int uxair_info_query(const char *path);
uxair_video_route_t uxair_video_route(http_request_t *request);
void uxair_trace_exchange(http_request_t *request, http_response_t *response, uxair_route_t route);
