/**
 *  Copyright (C) 2018  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *===================================================================
 * modfied by fduncanh 2021-2023
 */
// TEARDOWN excerpt: UxPlay 9f3c2bbc658645533fa1057e76c29ec8c947fa0f
static void
raop_handler_teardown(raop_conn_t *conn,
                      http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen)
{
    /* get the teardown request type(s):  (type 96, 110, or none) */
    raop_t *raop = conn->raop;
    const char *data = NULL;
    int data_len = 0;
    bool teardown_96 = false, teardown_110 = false;
    data = http_request_get_data(request, &data_len);
    plist_t req_root_node = NULL;
    plist_from_bin(data, data_len, &req_root_node);
    plist_t req_streams_node = plist_dict_get_item(req_root_node, "streams");
    /* Process stream teardown requests */
    if (PLIST_IS_ARRAY(req_streams_node)) {
        uint64_t val = 0;
        int count = plist_array_get_size(req_streams_node);
        for (int i = 0; i < count; i++) {
            plist_t req_stream_node = plist_array_get_item(req_streams_node,i);
            plist_t req_stream_type_node = plist_dict_get_item(req_stream_node, "type");
            plist_get_uint_val(req_stream_type_node, &val);
            if (val == 96) {
                teardown_96 = true;
            } else if (val == 110) { 
                teardown_110 = true;
            }
        }
    }
    plist_free(req_root_node);
    logger_log(raop->logger, LOGGER_DEBUG, "TEARDOWN request,  96=%d, 110=%d", teardown_96, teardown_110);
  
    http_response_add_header(response, "Connection", "close");
  
    if (teardown_96) {
        if (conn->raop_rtp) {
            /* Stop our audio RTP session */
            raop_rtp_stop(conn->raop_rtp);
            /* stop any  coverart rendering */
            if (raop->callbacks.audio_stop_coverart_rendering) {
                raop->callbacks.audio_stop_coverart_rendering(raop->callbacks.cls);
            }
        }
    } else if (teardown_110) {
        if (raop->hls_pending) {
            raop->callbacks.video_reset(raop->callbacks.cls, RESET_TYPE_RTP_TO_HLS_TEARDOWN);
        } else {
            raop->callbacks.video_reset(raop->callbacks.cls, RESET_TYPE_RTP_SHUTDOWN);
        }
        if (conn->raop_rtp_mirror) {
        /* Stop our video RTP session */
            raop_rtp_mirror_stop(conn->raop_rtp_mirror);
        }
    } else {
        /* Destroy our sessions */
        if (conn->raop_rtp) {
            raop_rtp_destroy(conn->raop_rtp);
            conn->raop_rtp = NULL;
        }
        if (conn->raop_rtp_mirror) {
            raop_rtp_mirror_destroy(conn->raop_rtp_mirror);
            conn->raop_rtp_mirror = NULL;
            /*fix for iOS >= 27 (does not send teardown_110 when mirrroring is stopped) */
            raop->callbacks.video_reset(raop->callbacks.cls, RESET_TYPE_RTP_SHUTDOWN);
        }
        /* shut down any HLS connections */
        int hls_count = httpd_count_connection_type(raop->httpd, CONNECTION_TYPE_HLS);
        if (hls_count) {
            raop->callbacks.video_reset(raop->callbacks.cls, RESET_TYPE_HLS_SHUTDOWN);
        }
    }
}
