#include "direct_crypto.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/crypto.h>

static pthread_once_t initialize = PTHREAD_ONCE_INIT;
static struct {
    uintptr_t (*create)(void);
    int (*destroy)(uintptr_t);
    int (*sap)(uintptr_t, unsigned char *, int, unsigned char *);
    int (*upgrade)(uintptr_t, unsigned char *, int, unsigned char *);
    int (*open)(uintptr_t, unsigned char *, int, unsigned char *);
    int (*seal)(uintptr_t, unsigned char *, int, unsigned char *);
} api;

static void load_library(void) {
    void *library = dlopen("libuxair-video-control.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) return;
    int (*version)(void) = dlsym(library, "uxair_vc_version");
    api.create = dlsym(library, "uxair_vc_new");
    api.destroy = dlsym(library, "uxair_vc_free");
    api.sap = dlsym(library, "uxair_vc_sap");
    api.upgrade = dlsym(library, "uxair_vc_upgrade");
    api.open = dlsym(library, "uxair_vc_open");
    api.seal = dlsym(library, "uxair_vc_seal");
    if (!version || version() != 1 || !api.destroy || !api.sap || !api.upgrade ||
        !api.open || !api.seal) api.create = NULL;
    /* Go's process-wide runtime can outlive handles. Keep the DSO mapped. */
}

uint64_t uxair_crypto_now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t) now.tv_sec * 1000 + (uint64_t) now.tv_nsec / 1000000;
}

int uxair_crypto_begin(uxair_crypto_t *state) {
    if (state->handle || pthread_once(&initialize, load_library) || !api.create) return -1;
    state->handle = api.create();
    return state->handle ? 0 : -1;
}

int uxair_crypto_sap(uxair_crypto_t *state, const unsigned char *body, int n, unsigned char output[142]) {
    return api.sap(state->handle, (unsigned char *)body, n, output);
}

int uxair_crypto_upgrade(uxair_crypto_t *state, const unsigned char *body, int n, unsigned char secret[32]) {
    return api.upgrade(state->handle, (unsigned char *)body, n, secret);
}

int uxair_crypto_seal(uxair_crypto_t *state, const unsigned char *data, int n, unsigned char *output) {
    return api.seal(state->handle, (unsigned char *)data, n, output);
}

void uxair_crypto_clear(uxair_crypto_t *state) {
    if (state->handle) api.destroy(state->handle);
    http_request_destroy(state->request);
    if (state->record) OPENSSL_clear_free(state->record, state->record_size);
    memset(state, 0, sizeof(*state));
}

int uxair_crypto_feed(uxair_crypto_t *state, const unsigned char *data, size_t n,
                       int (*plaintext)(void *, const unsigned char *, size_t), void *opaque) {
    while (n) {
        if (state->buffered < sizeof(state->header)) {
            if (!state->buffered) state->record_deadline_ms = uxair_crypto_now_ms() + 10000;
            size_t take = sizeof(state->header) - state->buffered;
            if (take > n) take = n;
            memcpy(state->header + state->buffered, data, take);
            state->buffered += take;
            data += take;
            n -= take;
            if (state->buffered < sizeof(state->header)) return 0;
            const unsigned char *h = state->header;
            uint32_t payload = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                               ((uint32_t)h[2] << 8) | h[3];
            if (payload > UX_CRYPTO_RECORD_MAX) return -2;
            state->record_size = (size_t)payload + UX_CRYPTO_OVERHEAD;
            state->record = OPENSSL_malloc(state->record_size);
            if (!state->record) return -3;
            memcpy(state->record, h, sizeof(state->header));
        }
        size_t take = state->record_size - state->buffered;
        if (take > n) take = n;
        memcpy(state->record + state->buffered, data, take);
        state->buffered += take;
        data += take;
        n -= take;
        if (state->buffered == state->record_size) {
            // The Go ABI authenticates into an independent temporary slice.
            // Once validated it copies plaintext over the consumed wire record.
            const int count = api.open(state->handle, state->record, (int)state->record_size, state->record);
            if (count < 0) return -1;
            ++state->received;
            state->record_deadline_ms = 0;
            const int result = plaintext(opaque, state->record, (size_t)count);
            OPENSSL_clear_free(state->record, state->record_size);
            state->record = NULL;
            state->buffered = state->record_size = 0;
            if (result) return result;
        }
    }
    return 0;
}
