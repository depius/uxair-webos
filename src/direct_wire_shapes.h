#pragma once

#include "direct_controls.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static inline const char *uxair_fp_envelope_shape(const char *body, size_t length) {
    if (!body || length < 4) return "short";
    if (memcmp(body, "FPLY", 4)) return "not-fply";
    // Compare only the public format discriminators, never the envelope material.
    static const unsigned char control_header[] = {1, 2, 1, 0, 0, 0, 0, 108, 0, 0, 0, 0};
    static const unsigned char material_length[] = {0, 0, 0, 64};
    if (length == 120 && !memcmp(body + 4, control_header, sizeof(control_header)) &&
        !memcmp(body + 32, material_length, sizeof(material_length))) return "legacy-control-envelope-shape";
    return "other-fply";
}

static inline void uxair_shape_append(char *output, size_t capacity, size_t *used, const char *format, ...) {
    if (*used >= capacity - 1) return;
    const size_t remaining = capacity - *used;
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(output + *used, remaining, format, arguments);
    va_end(arguments);
    if (length >= 0) *used += (size_t)length < remaining ? (size_t)length : remaining - 1;
}

static inline const char *uxair_media_key_label(const char *key) {
    static const char *const names[] = {
        "mediaType", "mediaCharacteristic", "mediaSubType", "language", "languageTag",
        "extendedLanguageTag", "locale", "type", "value", "option", "choices",
        "mediaSelectionOption", "mediaSelectionGroup", "trackID", "groupID", "displayName",
        "characteristics", "selected", "isDefault", "audio", "subtitles", "soun", "sbtl"
    };
    for (size_t i = 0; key && i < sizeof(names) / sizeof(*names); ++i)
        if (!strcmp(key, names[i])) return names[i];
    return "unlisted";
}

static inline void uxair_media_array_shape(plist_t array, char *output, size_t capacity) {
    if (!capacity) return;
    output[0] = '\0';
    if (!PLIST_IS_ARRAY(array)) return;

    const uint32_t count = plist_array_get_size(array);
    const uint32_t limit = 4, key_limit = 6;
    size_t used = 0;
    uxair_shape_append(output, capacity, &used, "%u:[", (unsigned)count);
    for (uint32_t i = 0; i < count && i < limit; ++i) {
        plist_t item = plist_array_get_item(array, i);
        uxair_shape_append(output, capacity, &used, "%s%s", i ? "," : "", uxair_property_value_shape(item));
        if (!PLIST_IS_DICT(item)) continue;

        const uint32_t keys = plist_dict_get_size(item);
        uxair_shape_append(output, capacity, &used, "(%u){", (unsigned)keys);
        plist_dict_iter iterator = NULL;
        plist_dict_new_iter(item, &iterator);
        if (iterator) {
            for (uint32_t j = 0; j < keys && j < key_limit; ++j) {
                char *key = NULL;
                plist_t value = NULL;
                plist_dict_next_item(item, iterator, &key, &value);
                uxair_shape_append(output, capacity, &used, "%s%s:%s", j ? "," : "",
                    uxair_media_key_label(key), uxair_property_value_shape(value));
                free(key);
            }
            free(iterator);
        }
        uxair_shape_append(output, capacity, &used, "%s}", keys > key_limit ? ",..." : "");
    }
    uxair_shape_append(output, capacity, &used, "%s]", count > limit ? ",..." : "");
}
