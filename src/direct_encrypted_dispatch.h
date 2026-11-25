/* Included after the native Direct Video dispatcher in the development receiver. */

int uxair_video_cipher_pending(void *opaque) {
    const raop_conn_t *conn = opaque;
    return conn->video_crypto.phase == UX_CRYPTO_ACK;
}

int uxair_video_cipher_active(void *opaque) {
    const raop_conn_t *conn = opaque;
    return conn->video_crypto.phase == UX_CRYPTO_ENCRYPTED;
}

int uxair_video_cipher_activate(void *opaque) {
    raop_conn_t *conn = opaque;
    if (!uxair_video_cipher_pending(conn)) return -1;
    /* Classify the socket for the existing Disconnect action. The RAOP
       handler is never called to assign a role or tear down a sibling. */
    if (httpd_set_connection_type(conn->raop->httpd, conn, CONNECTION_TYPE_AIRPLAY) < 0) {
        conn->video_crypto.close_reason = "socket-role";
        return -1;
    }
    conn->video_crypto.phase = UX_CRYPTO_ENCRYPTED;
    conn->video_crypto.deadline_ms = 0;
    ++conn->raop->wire.crypto_upgrades;
    fprintf(stderr, "[video-crypto] conn=%u stage=upgrade result=encrypted\n", conn->wire_id);
    return 0;
}

void uxair_video_cipher_abort(void *opaque, const char *reason) {
    raop_conn_t *conn = opaque;
    if (conn->video_pair_bound) conn->video_crypto.close_reason = reason;
}

int uxair_video_cipher_expired(void *opaque) {
    raop_conn_t *conn = opaque;
    uxair_crypto_t *crypto = &conn->video_crypto;
    if (!crypto->deadline_ms && !crypto->record_deadline_ms && !crypto->http_deadline_ms && !crypto->reverse_ack_ms) return 0;
    const uint64_t now = uxair_crypto_now_ms();
    if (crypto->reverse_ack_ms && now>=crypto->reverse_ack_ms) {
        ++conn->raop->wire.reverse_failures;
        crypto->close_reason="reverse-ack-timeout";
        fprintf(stderr,"[reverse-channel] conn=%u reason=reverse-ack-timeout event=%u generation=%llu outstanding=1\n",
                conn->wire_id,crypto->reverse_sequence,(unsigned long long)crypto->reverse_generation);
        return 1;
    }
    if ((crypto->deadline_ms && now >= crypto->deadline_ms) ||
        (crypto->record_deadline_ms && now >= crypto->record_deadline_ms) ||
        (crypto->http_deadline_ms && now >= crypto->http_deadline_ms)) {
        crypto->close_reason = "timeout";
        return 1;
    }
    return 0;
}

static bool uxair_connection_upgrade(const char *value) {
    if (!value) return false;
    while (*value) {
        const char *end=strchr(value, ',');
        const char *next=end?end+1:value+strlen(value);
        if (!end) end=next;
        while (value<end && (*value==' ' || *value=='\t')) ++value;
        while (end>value && (end[-1]==' ' || end[-1]=='\t')) --end;
        if (end-value==7 && !strncasecmp(value,"Upgrade",7)) return true;
        value=next;
    }
    return false;
}

static http_response_t *uxair_encrypted_route(raop_conn_t *conn, http_request_t *request, const char **route) {
    const char *method = http_request_get_method(request);
    const char *path = http_request_get_url(request);
    const char *owner = http_request_get_header(request, "X-Apple-Session-ID");
    http_response_t *response = NULL;
    int n = 0, output_length = 0;
    const char *body=http_request_get_data(request, &n);
    char *output=NULL;
    const char *content_type=NULL;
    const bool play=!strcmp(method,"POST")&&!strcmp(path,"/play");
    const bool reverse=!strcmp(method,"POST")&&!strcmp(path,"/reverse");
    if (play) ++conn->raop->wire.admission_attempts;
    int status = 501;
    *route = "unsupported";
    if (!conn->video_verified || !conn->video_pair_owner[0]) {
        status = 470;
        *route = "proof-required";
    } else if (owner && strcmp(owner, conn->video_pair_owner)) {
        status = 409;
        *route = "session-conflict";
    } else if (reverse) {
        const char *upgrade=http_request_get_header(request,"Upgrade");
        *route="reverse";
        if (n || !upgrade || strcasecmp(upgrade,"PTTH/1.0") ||
            !uxair_connection_upgrade(http_request_get_header(request,"Connection"))) status=400;
        else if (!uxair_direct_encrypted_reverse(conn->raop->callbacks.cls,conn->video_pair_owner,
                 conn->video_controller,(unsigned long)conn)) status=409;
        else {
            status=101;
            conn->video_crypto.reverse=1;
        }
    } else if (!n && !strcmp(method, "GET") && !strcmp(path, "/server-info")) {
        *route = "discovery";
        uxair_info_response(conn, request, &response, NULL, 1);
    } else if (play || (!n && !strcmp(method,"GET") &&
               (!strcmp(path,"/playback-info") || !strcmp(path,"/scrub"))) ||
               (!strcmp(method,"PUT") && uxair_control_path(path,"/setProperty")) ||
               (!strcmp(method,"POST") && ((!n && !strcmp(path,"/stop")) || !strcmp(path,"/action") ||
                   uxair_control_path(path,"/rate") || uxair_control_path(path,"/scrub")))) {
        const char *reason = NULL;
        *route = play?"play":"direct";
        status=uxair_direct_request(conn->raop->callbacks.cls,conn->video_pair_owner,method,path,
            http_request_get_header(request,"Content-Type"),body,n,&output,&output_length,&content_type,
            &reason,conn->video_controller,(unsigned long)conn);
        if (play && status==200 && (!reason || strcmp(reason,"duplicate-play"))) ++conn->raop->wire.admitted;
        if (play && status!=200 && !conn->video_crypto.admission_logged)
            fprintf(stderr,"[direct-session] conn=%u admission=rejected status=%d reason=%s\n",
                    conn->wire_id,status,reason?reason:"handler");
        if (!play && !strcmp(method,"POST") && (!strcmp(path,"/action") || uxair_control_path(path,"/scrub")) &&
            conn->video_crypto.received<=16)
            fprintf(stderr,"[direct-control] conn=%u path=%s status=%d reason=%s\n",conn->wire_id,
                    uxair_wire_path(path),status,reason?reason:"queued");
    }
    if (response) return response;
    response = uxair_response("HTTP/1.1", NULL, status);
    if (response) {
        if (status==101) {
            http_response_add_header(response,"Connection","Upgrade");
            http_response_add_header(response,"Upgrade","PTTH/1.0");
        }
        if (output_length && content_type) http_response_add_header(response,"Content-Type",content_type);
        if (!output_length) http_response_add_header(response,"Content-Length","0");
        http_response_finish(response,output,output_length);
    }
    free(output);
    return response;
}

typedef struct {
    raop_conn_t *conn;
    int fd;
} uxair_encrypted_dispatch_t;

static int uxair_encrypted_send(raop_conn_t *conn,int fd,const char *data,int n) {
    if (n <= 0 || n > UX_CRYPTO_RECORD_MAX) return -5;
    unsigned char *output = malloc((size_t)n + UX_CRYPTO_OVERHEAD);
    if (!output) return -5;
    const int sealed = uxair_crypto_seal(&conn->video_crypto, (const unsigned char*)data, n, output);
    int sent = 0;
    while (sent < sealed) {
        const ssize_t wrote = send(fd, output + sent, (size_t)(sealed - sent), MSG_NOSIGNAL);
        if (wrote <= 0) break;
        sent += (int)wrote;
    }
    free(output);
    if (sent != n + UX_CRYPTO_OVERHEAD) return -5;
    ++conn->video_crypto.sent;
    return 0;
}

int uxair_video_cipher_service(void *opaque,int fd) {
    raop_conn_t *conn=opaque;
    uxair_crypto_t *crypto=&conn->video_crypto;
    if(!crypto->reverse||crypto->reverse_ack_ms) return 0;
    char event[8192];
    const int n=uxair_direct_next_event(conn->raop->callbacks.cls,(unsigned long)conn,event,sizeof(event),&crypto->reverse_generation);
    if(n<=0) {
        if(n<0) crypto->close_reason="reverse-session-ended";
        return n;
    }
    char wire[8640];
    const int header=snprintf(wire,sizeof(wire),"POST /event HTTP/1.1\r\nX-Apple-Session-ID: %s\r\n"
            "Content-Type: text/x-apple-plist+xml\r\nContent-Length: %d\r\n\r\n",conn->video_pair_owner,n);
    if(header<=0 || (size_t)(header+n)>sizeof(wire)) return -1;
    memcpy(wire+header,event,(size_t)n);
    if(uxair_encrypted_send(conn,fd,wire,header+n)) {
        ++conn->raop->wire.reverse_failures;
        crypto->close_reason="reverse-event-write";
        return -1;
    }
    ++conn->raop->wire.reverse_events;
    ++crypto->reverse_sequence;
    crypto->reverse_ack_ms=uxair_crypto_now_ms()+5000;
    if(crypto->reverse_sequence<=16)
        fprintf(stderr,"[reverse-channel] conn=%u event=%u generation=%llu outstanding=1 action=sent\n",
                conn->wire_id,crypto->reverse_sequence,(unsigned long long)crypto->reverse_generation);
    return 0;
}

static int uxair_reverse_ack_rejected(const raop_conn_t *conn, const char *reason) {
    const uxair_crypto_t *crypto=&conn->video_crypto;
    fprintf(stderr,"[reverse-channel] conn=%u ack=rejected reason=%s event=%u generation=%llu outstanding=%d\n",
            conn->wire_id,reason,crypto->reverse_sequence,(unsigned long long)crypto->reverse_generation,
            crypto->reverse_ack_ms!=0);
    return -4;
}

static int uxair_encrypted_http(void *opaque, const unsigned char *data, size_t n) {
    uxair_encrypted_dispatch_t *dispatch = opaque;
    raop_conn_t *conn = dispatch->conn;
    uxair_crypto_t *crypto = &conn->video_crypto;
    if (crypto->received == 1)
        fprintf(stderr, "[video-crypto] conn=%u encrypted_records_rx=1\n", conn->wire_id);
    while (n) {
        if (crypto->reverse && !crypto->reverse_ack_ms) return uxair_reverse_ack_rejected(conn,"no-event-outstanding");
        if (!crypto->request) {
            crypto->request = http_request_init();
            if (!crypto->request) return -3;
            if (crypto->reverse) uxair_request_encrypted_reply(crypto->request);
            else uxair_request_encrypted(crypto->request);
            crypto->http_deadline_ms = uxair_crypto_now_ms() + 10000;
        }
        http_request_t *request = crypto->request;
        const size_t limit=crypto->reverse?8192:UX_CRYPTO_HTTP_MAX;
        size_t chunk = limit - crypto->http_bytes;
        if (chunk > n) chunk = n;
        http_request_add_data(request, (const char *)data, (int)chunk);
        if (http_request_has_error(request))
            return crypto->reverse ? uxair_reverse_ack_rejected(conn,uxair_reply_rejection(request)) : -4;
        if (!http_request_is_complete(request)) {
            crypto->http_bytes += chunk;
            if (crypto->http_bytes < limit) return 0;
            return crypto->reverse ? uxair_reverse_ack_rejected(conn,"header-limit") : -4;
        }
        const int consumed = uxair_request_consumed(request);
        if (consumed <= 0 || (size_t)consumed > chunk)
            return crypto->reverse ? uxair_reverse_ack_rejected(conn,"parser-length") : -4;
        if (crypto->reverse) {
            const char *session = http_request_get_header(request,"X-Apple-Session-ID");
            if (session && strcmp(session,conn->video_pair_owner))
                return uxair_reverse_ack_rejected(conn,"session-mismatch");
            if ((size_t)consumed!=n) return uxair_reverse_ack_rejected(conn,"trailing-data");
            if (crypto->reverse_sequence<=16)
                fprintf(stderr,"[reverse-channel] conn=%u ack=accepted fragmented=%d event=%u generation=%llu outstanding=0\n",
                        conn->wire_id,crypto->http_bytes!=0,crypto->reverse_sequence,
                        (unsigned long long)crypto->reverse_generation);
            crypto->reverse_ack_ms=0;
            http_request_destroy(crypto->request);
            crypto->request=NULL;
            crypto->http_bytes=0;
            crypto->http_deadline_ms=0;
            return 0;
        }
        uxair_wire_stats_t *stats = &conn->raop->wire;
        ++stats->crypto_requests;
        const char *route;
        http_response_t *response = uxair_encrypted_route(conn, request, &route);
        const bool first = !crypto->sent;
        const int status = uxair_wire_status(response);
        int length=0;
        const char *wire=response?http_response_get_data(response,&length):NULL;
        const int result = uxair_encrypted_send(conn,dispatch->fd,wire,length);
        const bool admission = !strcmp(route, "play") && (status==200 || !crypto->admission_logged);
        if (admission) crypto->admission_logged = 1;
        if (!strcmp(route,"reverse")) {
            if (status==101&&!result) ++stats->reverse_upgrades;
            else ++stats->reverse_failures;
            fprintf(stderr,"[reverse-channel] conn=%u upgrade_status=%d sent=%d\n",conn->wire_id,status,!result);
        }
        if (first || crypto->sent <= 8 || admission)
            fprintf(stderr, "[video-control] conn=%u method=%s path=%s authenticated=1 first=%d route=%s status=%d sent=%d\n",
                    conn->wire_id, uxair_wire_method(http_request_get_method(request)),
                    uxair_wire_path(http_request_get_url(request)), first, route, status, !result);
        http_response_destroy(response);
        http_request_destroy(crypto->request);
        crypto->request = NULL;
        crypto->http_bytes = 0;
        crypto->http_deadline_ms = 0;
        if (result) return result;
        data += consumed;
        n -= (size_t)consumed;
    }
    return 0;
}

int uxair_video_cipher_input(void *opaque, int fd, const char *data, size_t n) {
    raop_conn_t *conn = opaque;
    uxair_crypto_t *crypto = &conn->video_crypto;
    const unsigned before = crypto->received;
    uxair_encrypted_dispatch_t dispatch = {conn, fd};
    const int result = uxair_crypto_feed(crypto, (const unsigned char *)data, n, uxair_encrypted_http, &dispatch);
    conn->raop->wire.crypto_records += crypto->received - before;
    switch (result) {
    case 0: return 0;
    case -1:
        ++conn->raop->wire.crypto_tag_failures;
        crypto->close_reason = "record-authentication";
        break;
    case -2: crypto->close_reason = "record-size"; break;
    case -3: crypto->close_reason = "allocation"; break;
    case -4:
        if(crypto->reverse) ++conn->raop->wire.reverse_failures;
        crypto->close_reason = crypto->reverse?"reverse-ack-framing":"encrypted-http-framing";
        break;
    default: crypto->close_reason = "encrypted-write"; break;
    }
    return -1;
}

static void uxair_video_cipher_log_close(const raop_conn_t *conn) {
    const uxair_crypto_t *crypto = &conn->video_crypto;
    if (crypto->phase==UX_CRYPTO_ENCRYPTED) ++conn->raop->wire.crypto_closed;
    if (crypto->phase || crypto->close_reason || conn->video_pair_bound) {
        const char *reason = crypto->close_reason;
        if (!reason) reason = crypto->buffered ? "truncated-record" : crypto->request ? "truncated-http" : "peer-or-listener-stop";
        fprintf(stderr, "[video-crypto] conn=%u stage=closed state=%d reason=%s rx=%u tx=%u\n",
                conn->wire_id, crypto->phase, reason, crypto->received, crypto->sent);
    }
}
