#include "direct_wire_shapes.h"
#include <assert.h>

static void envelope_shape(void) {
    char frame[120] = {'F', 'P', 'L', 'Y', 1, 2, 1, 0, 0, 0, 0, 108};
    frame[35] = 64;
    memset(frame + 56, 0xa5, sizeof(frame) - 56);
    assert(!strcmp(uxair_fp_envelope_shape(frame, sizeof(frame)), "legacy-control-envelope-shape"));
    assert(!strcmp(uxair_fp_envelope_shape(frame, sizeof(frame)-1), "other-fply"));
    assert(!strcmp(uxair_fp_envelope_shape(frame, 3), "short"));
    assert(!strcmp(uxair_fp_envelope_shape(NULL, sizeof(frame)), "short"));
    frame[35] = 63;
    assert(!strcmp(uxair_fp_envelope_shape(frame, sizeof(frame)), "other-fply"));
    frame[0] = 'x';
    assert(!strcmp(uxair_fp_envelope_shape(frame, sizeof(frame)), "not-fply"));
}

static void media_array_shape(void) {
    plist_t root = plist_new_dict();
    plist_t array = plist_new_array();
    plist_t track = plist_new_dict();
    plist_dict_set_item(track, "mediaType", plist_new_string("private-media-type"));
    plist_dict_set_item(track, "languageTag", plist_new_string("private-language"));
    plist_dict_set_item(track, "https://private.example/key?token=secret", plist_new_uint(178));
    plist_dict_set_item(track, "selected", plist_new_bool(1));
    plist_dict_set_item(track, "displayName", plist_new_string("private-name"));
    plist_dict_set_item(track, "option", plist_new_dict());
    plist_dict_set_item(track, "isDefault", plist_new_bool(1));
    plist_array_append_item(array, track);
    plist_array_append_item(array, plist_new_string("private-element"));
    plist_array_append_item(array, plist_new_data("private-data", 12));
    plist_array_append_item(array, plist_new_array());
    plist_array_append_item(array, plist_new_uint(137));
    plist_dict_set_item(root, "value", array);

    char *binary = NULL;
    uint32_t length = 0;
    plist_to_bin(root, &binary, &length);
    assert(binary && length);
    plist_free(root);
    root = uxair_control_plist(binary, length);
    free(binary);
    assert(PLIST_IS_DICT(root));
    array = plist_dict_get_item(root, "value");

    char output[1024];
    uxair_media_array_shape(array, output, sizeof(output));
    assert(!strcmp(output, "5:[dict(7){mediaType:string,languageTag:string,unlisted:integer,"
        "selected:boolean,displayName:string,option:dict,...},string,data,array,...]"));
    assert(!strstr(output, "private") && !strstr(output, "token") && !strstr(output, "178"));

    char short_output[8];
    uxair_media_array_shape(array, short_output, sizeof(short_output));
    assert(short_output[sizeof(short_output)-1] == '\0');
    uxair_media_array_shape(root, output, sizeof(output));
    assert(!*output);
    plist_free(root);
}

int main(void) {
    envelope_shape();
    media_array_shape();
    puts("PASS: bounded AirPlay envelope and binary-plist trace shapes redact data");
    return 0;
}
