#pragma once

#include <plist/plist.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline int uxair_control_path(const char *path, const char *endpoint) {
    const size_t length = strlen(endpoint);
    return path && !strncmp(path, endpoint, length) && (path[length] == '\0' || path[length] == '?');
}

static inline const char *uxair_known_property(const char *name) {
    static const char *const names[] = {
        "actionAtItemEnd", "selectedMediaArray", "reverseEndTime", "forwardEndTime",
        "interstitialEvents", "textMarkupArray", "isInterestedInDateRange",
        "playbackPosition", "duration", "rate", "playbackAccessLog", "playbackErrorLog"
    };
    for (size_t i = 0; name && i < sizeof(names) / sizeof(*names); ++i)
        if (!strcmp(name, names[i])) return names[i];
    return NULL;
}

static inline plist_t uxair_control_plist(const char *data, size_t length) {
    plist_t root = NULL;
    if (data && length >= 8 && length <= 16384 && !memcmp(data, "bplist00", 8))
        plist_from_bin(data, (uint32_t)length, &root);
    return root;
}

static inline const char *uxair_property_value_shape(plist_t value) {
    if (!value) return "missing";
    if (PLIST_IS_DICT(value)) {
        const char *const fields[] = {"value", "timescale", "epoch", "flags"};
        for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i)
            if (!PLIST_IS_UINT(plist_dict_get_item(value, fields[i]))) return "dict";
        return "cm-time";
    }
    if (PLIST_IS_UINT(value)) return "integer";
    if (PLIST_IS_REAL(value)) return "real";
    if (PLIST_IS_BOOLEAN(value)) return "boolean";
    if (PLIST_IS_ARRAY(value)) return "array";
    if (PLIST_IS_STRING(value)) return "string";
    if (PLIST_IS_DATA(value)) return "data";
    return "other";
}
