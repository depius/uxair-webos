#include "direct_native_diag.h"
#include "identity.h"
#include "media.h"
#include <AcbAPI.h>
#include <StarfishMediaAPIs.h>
#include <plist/plist.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>

void (*StarfishMediaAPIs::callback)(int,int64_t,const char*,void*) = nullptr;
void *StarfishMediaAPIs::context = nullptr;
std::string StarfishMediaAPIs::payload, StarfishMediaAPIs::last_seek;
int StarfishMediaAPIs::unloads = 0;
bool StarfishMediaAPIs::load_success = true, StarfishMediaAPIs::play_success = true,
     StarfishMediaAPIs::synchronous_load = true;
const char *StarfishMediaAPIs::load_exception = nullptr, *StarfishMediaAPIs::unload_exception = nullptr;
int foreground_calls = 0, load_calls = 0;
bool acb_initialize_success = true, acb_sink_success = true;
int acb_player_type = -1, acb_sink_calls = 0;
extern "C" int AcbAPI_setMediaVideoData(long, const char*, long*) { return 1; }

static std::string string_field(plist_t dict, const char *key) {
    char *value = nullptr;
    plist_get_string_val(plist_dict_get_item(dict, key), &value);
    assert(value);
    const std::string result(value);
    std::free(value);
    return result;
}

int main() {
    Media media;
    media.guard_direct_callbacks(direct_native_json_summary);
    const auto open = [&] {
        return media.open(UXAIR_DEV_APP_ID, 1280, 720, true, 44100, 2097152,
                          1920, 1080, MirrorCodec::H264, 60);
    };
    StarfishMediaAPIs::load_exception = "No such node (plugins)";
    assert(!open() && media.open_failure() == MediaOpenFailure::StarfishLoad);
    assert(media.direct_released() && !media.ready && StarfishMediaAPIs::unloads == 1);
    auto *failed_context = StarfishMediaAPIs::context;
    const auto callback = StarfishMediaAPIs::callback;

    StarfishMediaAPIs::load_exception = nullptr;
    assert(open() && acb_player_type == PLAYER_TYPE_MSE);
    callback(PF_EVENT_TYPE_INT_ERROR, 0, nullptr, failed_context);
    assert(!media.failed && media.ready);
    callback(PF_EVENT_TYPE_STR_VIDEO_INFO, 0, "{\"width\":1280,\"height\":720,\"uri\":\"https://SECRET.invalid/?token=PRIVATE\"}",
             StarfishMediaAPIs::context);
    assert(media.video_info_width == 1280 && media.video_info_height == 720);
    media.tick();
    assert(media.playing);

    const auto& payload = StarfishMediaAPIs::payload;
    plist_t root = nullptr;
    plist_from_json(payload.data(), uint32_t(payload.size()), &root);
    const auto args = plist_dict_get_item(root, "args");
    assert(plist_array_get_size(args) == 1);
    const auto arg = plist_array_get_item(args, 0);
    const auto option = plist_dict_get_item(arg, "option");
    const auto stream = plist_dict_get_item(option, "externalStreamingInfo");
    const auto contents = plist_dict_get_item(stream, "contents");
    const auto codec = plist_dict_get_item(contents, "codec");
    const auto pcm = plist_dict_get_item(contents, "pcmInfo");
    assert(string_field(arg, "mediaTransportType") == "BUFFERSTREAM");
    assert(string_field(option, "appId") == UXAIR_DEV_APP_ID);
    assert(string_field(codec, "video") == "H264" && string_field(codec, "audio") == "PCM");
    assert(string_field(contents, "format") == "RAW" && string_field(contents, "provider") == "Chrome");
    assert(string_field(pcm, "format") == "S16LE");
    assert(plist_get_node_type(plist_dict_get_item(pcm, "sampleRate")) == PLIST_REAL);
    assert(plist_get_node_type(plist_dict_get_item(contents, "esInfo")) == PLIST_DICT);
    assert(plist_get_node_type(plist_dict_get_item(stream, "bufferingCtrInfo")) == PLIST_DICT);
    assert(!plist_dict_get_item(root, "uri") && !plist_dict_get_item(arg, "uri"));
    for (auto node : {root, arg, option, stream, contents}) assert(!plist_dict_get_item(node, "plugins"));
    plist_free(root);

    StarfishMediaAPIs::unload_exception = "unload failed for https://SECRET.invalid/?token=PRIVATE";
    media.close();
    StarfishMediaAPIs::unload_exception = nullptr;
    assert(!media.direct_released() && !open() && StarfishMediaAPIs::unloads == 2);
    std::puts("PASS: real Starfish adapter builds the v1.3 BUFFERSTREAM schema, recovers from Load exceptions, and blocks reuse after failed Unload");
}
