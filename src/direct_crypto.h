#pragma once

#include <stddef.h>
#include <stdint.h>
#include "http_request.h"

enum {
    UX_CRYPTO_SAP_M3 = 1, UX_CRYPTO_SAP_COMPLETE, UX_CRYPTO_ACK, UX_CRYPTO_ENCRYPTED,
    UX_CRYPTO_RECORD_MAX = 262144, UX_CRYPTO_OVERHEAD = 20, UX_CRYPTO_HTTP_MAX = 24576
};

typedef struct {
    uintptr_t handle;
    int phase;
    unsigned received, sent;
    int admission_logged;
    int reverse;
    unsigned char header[4], *record;
    size_t buffered, record_size, http_bytes;
    http_request_t *request;
    const char *close_reason;
    uint64_t deadline_ms, record_deadline_ms, http_deadline_ms, reverse_ack_ms;
} uxair_crypto_t;

int uxair_crypto_begin(uxair_crypto_t *state);
int uxair_crypto_sap(uxair_crypto_t *state, const unsigned char *body, int n, unsigned char output[142]);
int uxair_crypto_upgrade(uxair_crypto_t *state, const unsigned char *body, int n, unsigned char secret[32]);
int uxair_crypto_seal(uxair_crypto_t *state, const unsigned char *data, int n, unsigned char *output);
void uxair_crypto_clear(uxair_crypto_t *state);
uint64_t uxair_crypto_now_ms(void);
int uxair_crypto_feed(uxair_crypto_t *state, const unsigned char *data, size_t n,
                       int (*plaintext)(void *, const unsigned char *, size_t), void *opaque);

/* Generated from the pinned parser and RAOP source for the Direct Dev build. */
int uxair_request_consumed(const http_request_t *request);
const char *uxair_reply_rejection(const http_request_t *request);
void uxair_request_encrypted(http_request_t *request);
void uxair_request_encrypted_reply(http_request_t *request);
int uxair_video_cipher_active(void *conn);
int uxair_video_cipher_pending(void *conn);
int uxair_video_cipher_activate(void *conn);
void uxair_video_cipher_abort(void *conn, const char *reason);
int uxair_video_cipher_expired(void *conn);
int uxair_video_cipher_input(void *conn, int fd, const char *data, size_t n);
int uxair_video_cipher_service(void *conn, int fd);
