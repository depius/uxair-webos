/* Included by the pinned, generated raop.c after its native handlers. */
#include "direct_config.h"
#include "direct_bridge.h"
#include "direct_controls.h"
#include "direct_crypto.h"
#include "direct_wire.h"
#include <strings.h>
#include <openssl/err.h>
#include <openssl/evp.h>

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
    case 415: reason = "Unsupported Media Type"; break;
    case 470: reason = "Client Authentication Failure"; break;
    case 501: reason = "Not Implemented"; break;
    case 503: reason = "Service Unavailable"; break;
    case 505: reason = "Protocol Version Not Supported"; break;
    }
    http_response_init(response, protocol, status, reason);
    http_response_add_header(response, "Server", "AirTunes/" GLOBAL_VERSION);
    if (cseq) http_response_add_header(response, "CSeq", cseq);
    return response;
}

/* The legacy video handshake gets its own connection-local pairing state.
 * The X-Apple-Session-ID binds its messages, but is never a credential. */
static void uxair_video_pairing_clear(raop_conn_t *conn) {
    pairing_session_destroy(conn->video_verify);
    conn->video_verify = NULL;
    fairplay_destroy(conn->video_fairplay);
    conn->video_fairplay = NULL;
    conn->video_verified = false;
    conn->video_pair_bound = false;
    OPENSSL_cleanse(conn->video_secret, sizeof(conn->video_secret));
    OPENSSL_cleanse(conn->video_controller, sizeof(conn->video_controller));
    uxair_crypto_clear(&conn->video_crypto);
}

static bool uxair_video_pairing_path(const char *method, const char *path) {
    return !strcmp(method, "POST") && (!strcmp(path, "/pair-setup") || !strcmp(path, "/pair-verify"));
}

/* The pinned handshake treats an invalid X25519 peer as fatal. Reject peers
 * that OpenSSL cannot derive with before passing them into that implementation.
 * This probe key is public and is independent of the actual pairing keys. */
static bool uxair_video_peer_valid(const unsigned char *raw) {
    const unsigned char probe[X25519_KEY_SIZE] = {1};
    unsigned char shared[X25519_KEY_SIZE] = {0};
    size_t length = sizeof(shared);
    EVP_PKEY *ours = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, probe, sizeof(probe));
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, raw, X25519_KEY_SIZE);
    EVP_PKEY_CTX *ctx = ours ? EVP_PKEY_CTX_new(ours, NULL) : NULL;
    bool valid = ctx && peer && EVP_PKEY_derive_init(ctx) == 1 &&
        EVP_PKEY_derive_set_peer(ctx, peer) == 1 && EVP_PKEY_derive(ctx, shared, &length) == 1;
    OPENSSL_cleanse(shared, sizeof(shared));
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer);
    EVP_PKEY_free(ours);
    ERR_clear_error();
    return valid;
}

static int uxair_video_verify(raop_conn_t *conn, const unsigned char *body, int length, bool pair_bound,
                              unsigned char *output, int *size, const char **reason) {
    if (!body || length < 4 || body[1] || body[2] || body[3] ||
        !((body[0] == 1 && length == 4 + X25519_KEY_SIZE + ED25519_KEY_SIZE) ||
          (body[0] == 0 && length == 4 + PAIRING_SIG_SIZE))) {
        *reason = "unsupported-pairing-format";
        return 400;
    }
    if (body[0] == 0) {
        if (!conn->video_verify) {
            *reason = "no-pending-proof";
            return 470;
        }
        int result = pairing_session_finish(conn->video_verify, body + 4);
        if (!result && conn->video_pair_bound &&
            !pairing_get_ecdh_secret_key(conn->video_verify, conn->video_secret)) result = -1;
        pairing_session_destroy(conn->video_verify);
        conn->video_verify = NULL;
        if (result) {
            uxair_video_pairing_clear(conn);
            *reason = "proof-rejected";
            return 470;
        }
        conn->video_verified = true;
        if (conn->video_pair_bound) {
            conn->video_crypto.deadline_ms = uxair_crypto_now_ms() + 30000;
            fprintf(stderr, "[video-crypto] conn=%u stage=pair-verify result=verified mode=pair-bound\n", conn->wire_id);
        }
        *reason = "proof-verified";
        return 200;
    }

    uxair_video_pairing_clear(conn);
    const unsigned char *key = body + 4 + X25519_KEY_SIZE;
    unsigned char encoded[4 * ((ED25519_KEY_SIZE + 2) / 3) + 1];
    const int encoded_length = EVP_EncodeBlock(encoded, key, ED25519_KEY_SIZE);
    const raop_callbacks_t *callbacks = &conn->raop->callbacks;
    bool registered = encoded_length == sizeof(encoded) - 1 && callbacks->check_register &&
        callbacks->check_register(callbacks->cls, (const char *)encoded);
    if (!registered) {
        *reason = "pin-registration-required";
        return 470;
    }
    if (!uxair_video_peer_valid(body + 4)) {
        *reason = "invalid-peer-key";
        return 400;
    }
    conn->video_verify = pairing_session_init(conn->raop->pairing);
    if (!conn->video_verify) return 503;
    pairing_session_set_setup_status(conn->video_verify);
    if (pairing_session_handshake(conn->video_verify, body + 4, key) ||
        pairing_session_get_public_key(conn->video_verify, output) ||
        pairing_session_get_signature(conn->video_verify, output + X25519_KEY_SIZE)) {
        uxair_video_pairing_clear(conn);
        return 503;
    }
    *size = X25519_KEY_SIZE + PAIRING_SIG_SIZE;
    memcpy(conn->video_controller,key,ED25519_KEY_SIZE);
    conn->video_pair_bound = pair_bound;
    if (pair_bound) conn->video_crypto.deadline_ms = uxair_crypto_now_ms() + 30000;
    *reason = "proof-required";
    return 200;
}

static int uxair_dispatch_video_pairing(raop_conn_t *conn, http_request_t *request,
                                         http_response_t **result, const char *cseq,
                                         uxair_route_t *route, const char **reason) {
    const char *protocol = http_request_get_protocol(request);
    const char *path = http_request_get_url(request);
    const char *owner = http_request_get_header(request, "X-Apple-Session-ID");
    const char *pd = http_request_get_header(request, "X-Apple-PD");
    if (cseq || !owner || !*owner || !uxair_video_pairing_path(http_request_get_method(request), path)) return 0;
    const bool bound_attempt = conn->video_pair_bound || (pd && conn->connection_type == CONNECTION_TYPE_UNKNOWN);

    *route = UX_ROUTE_VIDEO_PAIRING;
    unsigned char output[X25519_KEY_SIZE + PAIRING_SIG_SIZE] = {0};
    int size = 0, length = 0;
    const unsigned char *body = (const unsigned char *)http_request_get_data(request, &length);
    int status = 400;
    size_t owner_length = strlen(owner);
    if (owner_length >= sizeof(conn->video_pair_owner)) {
        *reason = "invalid-session";
    } else if (conn->video_pair_bound &&
               (!strcmp(path, "/pair-setup") || (length == 68 && body[0] == 1))) {
        *reason = "pairing-already-started";
        status = 409;
    } else if (conn->video_pair_owner[0] && strcmp(conn->video_pair_owner, owner)) {
        *reason = "session-conflict";
        status = 409;
    } else {
        if (!strcmp(path, "/pair-setup")) {
            if (length == ED25519_KEY_SIZE) {
                uxair_video_pairing_clear(conn);
                pairing_get_public_key(conn->raop->pairing, output);
                size = ED25519_KEY_SIZE;
                *reason = "public-key-only";
                status = 200;
            } else {
                *reason = "unsupported-pairing-format";
            }
        } else {
            *reason = "pairing-unavailable";
            /* Safari uses RTSP/1.0 for legacy video Pair-Verify and can then
               switch to HTTP/1.1 on this socket for the FairPlay exchanges. */
            const bool legacy_wire = !strcmp(protocol, "RTSP/1.0") || !strcmp(protocol, "HTTP/1.1");
            if (pd && (strcmp(pd, "1") || !legacy_wire ||
                       conn->connection_type != CONNECTION_TYPE_UNKNOWN ||
                       (length == 68 && body[0] == 0 && !conn->video_pair_bound))) {
                *reason = "unsupported-pairing-mode";
                status = 400;
            } else {
                status = uxair_video_verify(conn, body, length, pd != NULL, output, &size, reason);
            }
        }
        if (status == 200) memcpy(conn->video_pair_owner, owner, owner_length + 1);
    }

    *result = uxair_response(protocol, NULL, status);
    if (*result) {
        if (status == 200) http_response_add_header(*result, "Content-Type", "application/octet-stream");
        http_response_finish(*result, (const char *)output, size);
        if (status != 200 && bound_attempt) {
            conn->video_crypto.close_reason = *reason;
            http_response_set_disconnect(*result, 1);
        }
    } else {
        uxair_video_pairing_clear(conn);
    }
    return 1;
}

/* A video setup exchange uses an isolated copy of the legacy transport
 * handshake. Its state is never used to decrypt video or set up RAOP. */
static int uxair_video_fairplay(raop_conn_t *conn, http_request_t *request,
                                unsigned char output[142], int *size, const char **reason) {
    if (strcmp(http_request_get_method(request), "POST")) {
        *reason = "invalid-method";
        return 405;
    }
    if (!conn->video_verified || !conn->video_pair_owner[0]) {
        *reason = "video-proof-required";
        return 470;
    }
    int length = 0;
    const unsigned char *data = (const unsigned char *)http_request_get_data(request, &length);
    if (conn->video_pair_bound) {
        uxair_crypto_t *crypto = &conn->video_crypto;
        const bool first = crypto->phase == 0 && length == 16;
        if (strcmp(http_request_get_protocol(request), "HTTP/1.1") || !data ||
            (!first && !(crypto->phase == UX_CRYPTO_SAP_M3 && length == 164))) {
            *reason = "sap-sequence";
            return 400;
        }
        if (first && uxair_crypto_begin(crypto)) {
            *reason = "crypto-library-unavailable";
            return 503;
        }
        const int n = uxair_crypto_sap(crypto, data, length, output);
        if (n != (first ? 142 : 32)) {
            *reason = "sap-invalid";
            return 400;
        }
        crypto->phase = first ? UX_CRYPTO_SAP_M3 : UX_CRYPTO_SAP_COMPLETE;
        *reason = first ? "sap-m1-accepted" : "sap-complete";
        *size = n;
        if (!first) fprintf(stderr, "[video-crypto] conn=%u stage=sap result=complete\n", conn->wire_id);
        return 200;
    }
    if (!data || length < 12 || memcmp(data, "FPLY", 4)) {
        *reason = "invalid-fairplay-frame";
        return 400;
    }
    if (data[4] != 3 || data[5] != 1) {
        *reason = "unsupported-fairplay-variant";
        return 501;
    }
    const bool first = length == 16 && data[6] == 1;
    const bool second = length == 164 && data[6] == 3;
    if ((!first && !second) || data[7] || data[8] || data[9] || data[10] ||
        data[11] != length - 12 || (first && data[14] > 3)) {
        *reason = "invalid-fairplay-frame";
        return 400;
    }
    if (second && !conn->video_fairplay) {
        *reason = "fairplay-setup-required";
        return 409;
    }
    if (!conn->video_fairplay) conn->video_fairplay = fairplay_init(conn->raop->logger);
    if (!conn->video_fairplay) {
        *reason = "fairplay-unavailable";
        return 503;
    }
    const int result = first ? fairplay_setup(conn->video_fairplay, data, output) :
        fairplay_handshake(conn->video_fairplay, data, output);
    if (result) {
        *reason = "fairplay-rejected";
        return 400;
    }
    *reason = first ? "fairplay-v3-setup" : "fairplay-v3-handshake";
    *size = first ? 142 : 32;
    return 200;
}

static void uxair_video_fp2(raop_conn_t *conn, http_request_t *request,
                           http_response_t **response, const char **reason) {
    uxair_crypto_t *crypto = &conn->video_crypto;
    ++conn->raop->wire.crypto_attempts;
    const char *owner = http_request_get_header(request, "X-Apple-Session-ID");
    int status = 470;
    *reason = "pair-bound-proof-required";
    if (strcmp(http_request_get_method(request), "POST")) {
        status = 405;
        *reason = "invalid-method";
    } else if (strcmp(http_request_get_protocol(request), "HTTP/1.1")) {
        status = 505;
        *reason = "unsupported-protocol";
    } else if (owner && conn->video_pair_owner[0] && strcmp(owner, conn->video_pair_owner)) {
        status = 409;
        *reason = "session-conflict";
    } else if (conn->video_verified && conn->video_pair_bound && conn->connection_type == CONNECTION_TYPE_UNKNOWN) {
        status = 409;
        *reason = "sap-required";
        if (crypto->phase == UX_CRYPTO_SAP_COMPLETE) {
            int length = 0;
            const unsigned char *data = (const unsigned char *)http_request_get_data(request, &length);
            const int upgraded = uxair_crypto_upgrade(crypto, data, length, conn->video_secret);
            status = upgraded ? 400 : 200;
            if (upgraded == -2) ++conn->raop->wire.crypto_envelope_failures;
            *reason = upgraded == -2 ? "envelope-rejected" : upgraded ? "key-material-rejected" : "envelope-authenticated";
        }
    }
    OPENSSL_cleanse(conn->video_secret, sizeof(conn->video_secret));
    fprintf(stderr, "[video-crypto] conn=%u stage=fp-setup2 result=%s\n", conn->wire_id, *reason);
    *response = uxair_response(http_request_get_protocol(request), NULL, status);
    if (!*response) return;
    http_response_add_header(*response, "Content-Length", "0");
    http_response_finish(*response, NULL, 0);
    if (status == 200) crypto->phase = UX_CRYPTO_ACK;
    else if (conn->connection_type == CONNECTION_TYPE_UNKNOWN) {
        crypto->close_reason = *reason;
        http_response_set_disconnect(*response, 1);
    }
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
                                http_response_t **result, const char *cseq, uxair_route_t *route,
                                const char **reason) {
    if (!uxair_direct_enabled()) return 0;
    if (uxair_dispatch_video_pairing(conn, request, result, cseq, route, reason)) return 1;
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
    if (!strcmp(http_request_get_url(request), "/fp-setup2")) {
        uxair_video_fp2(conn, request, result, reason);
        return 1;
    }
    if (owner && conn->video_pair_owner[0] && strcmp(owner, conn->video_pair_owner)) {
        *reason = "session-conflict";
        *result = uxair_response(protocol, cseq, 409);
        if (*result) http_response_finish(*result, NULL, 0);
        return 1;
    }
    if (selected == UX_VIDEO_FAIRPLAY) {
        unsigned char output[142];
        int size = 0;
        const int status = uxair_video_fairplay(conn, request, output, &size, reason);
        *result = uxair_response(protocol, cseq, status);
        if (*result) {
            if (size) http_response_add_header(*result, "Content-Type", "application/octet-stream");
            http_response_finish(*result, (const char *)output, size);
            if (status != 200 && conn->video_pair_bound) {
                conn->video_crypto.close_reason = *reason;
                http_response_set_disconnect(*result, 1);
            }
        }
        return 1;
    }
    if (conn->video_pair_bound && conn->video_crypto.phase != UX_CRYPTO_ENCRYPTED) {
        *reason = "encrypted-upgrade-required";
        *result = uxair_response(protocol, cseq, 409);
        if (*result) {
            http_response_add_header(*result, "Content-Length", "0");
            http_response_finish(*result, NULL, 0);
        }
        return 1;
    }
    if (selected==UX_VIDEO_REVERSE || (!conn->video_verified&&!strcmp(method,"POST")&&
            (!strcmp(http_request_get_url(request),"/play")||!strcmp(http_request_get_url(request),"/action")))) {
        *reason="video-proof-required";
        *result=uxair_response(protocol,cseq,470);
        if (*result) http_response_finish(*result,NULL,0);
        return 1;
    }
    if (!conn->video_verified && !strcmp(method, "PUT") &&
        uxair_control_path(http_request_get_url(request), "/setProperty")) {
        *reason = "video-proof-required";
        *result = uxair_response(protocol, cseq, 470);
        if (*result) http_response_finish(*result, NULL, 0);
        return 1;
    }
    if (!owner && conn->video_verified) owner = conn->video_pair_owner;
    int status = 400;
    int length = 0, output_length = 0;
    char *output = NULL;
    const char *type = "text/parameters";
    const char *data = http_request_get_data(request, &length);
    status = uxair_direct_request(raop->callbacks.cls, owner, method, http_request_get_url(request),
        http_request_get_header(request, "Content-Type"), data, length, &output, &output_length, &type, reason, NULL, 0, conn->wire_id);
    *result = uxair_response(protocol, cseq, status);
    if (*result) {
        http_response_add_header(*result, "Content-Type", type);
        http_response_finish(*result, output, output_length);
    }
    free(output);
    return 1;
}
