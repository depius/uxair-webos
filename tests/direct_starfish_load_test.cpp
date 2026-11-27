#include "direct_native_diag.h"
#include "identity.h"
#include "media.h"
#include <AcbAPI.h>
#include <StarfishMediaAPIs.h>
#include <plist/plist.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <thread>

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
    const auto open = [&](MirrorCodec codec=MirrorCodec::H264,bool audio=true) {
        return media.open(UXAIR_DEV_APP_ID, 1280, 720, audio, 44100, 2097152,
                          1920, 1080, codec, 60);
    };
    StarfishMediaAPIs::load_exception = "No such node (plugins)";
    assert(!open() && media.open_failure() == MediaOpenFailure::StarfishLoad);
    assert(media.direct_released() && !media.ready && StarfishMediaAPIs::unloads == 1);
    auto *failed_context = StarfishMediaAPIs::context;
    const auto callback = StarfishMediaAPIs::callback;

    StarfishMediaAPIs::load_exception = nullptr;
    StarfishMediaAPIs::synchronous_load = false;
    assert(open() && acb_player_type == PLAYER_TYPE_MSE);
    callback(PF_EVENT_TYPE_INT_ERROR, 0, nullptr, failed_context);
    assert(!media.failed && !media.ready);
    media.tick();
    assert(!media.play_accepted());
    const unsigned char sample[] = {1,2,3,4};
    assert(media.feed(sample, sizeof(sample), 0, true) == FeedResult::Accepted);
    assert(media.feed(sample, sizeof(sample), 0, false) == FeedResult::Accepted);
    assert(!media.ready && !media.frames);
    auto* live_context = StarfishMediaAPIs::context;
    std::thread ready([&] { callback(PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED, 0, nullptr, live_context); });
    ready.join();
    callback(PF_EVENT_TYPE_STR_VIDEO_INFO, 0, "{\"width\":1280,\"height\":720,\"uri\":\"https://SECRET.invalid/?token=PRIVATE\"}",
             StarfishMediaAPIs::context);
    assert(media.video_info_width == 1280 && media.video_info_height == 720);
    media.direct_hold_start(true);
    media.tick();
    assert(media.ready && !media.playing && !media.play_accepted() && !media.frames);
    media.direct_hold_start(false);
    media.tick();
    assert(media.ready && media.playing && media.play_accepted() && !media.frames);
    callback(PF_EVENT_TYPE_FRAMEREADY, 0, nullptr, live_context);
    assert(media.frames==1);

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

    media.close();
    assert(open(MirrorCodec::ExperimentalHevc) && !media.ready && media.frames==0);
    assert(StarfishMediaAPIs::payload.find("\"video\":\"H265\"")!=std::string::npos);
    callback(PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED, 0, nullptr, live_context);
    callback(PF_EVENT_TYPE_FRAMEREADY, 0, nullptr, live_context);
    callback(PF_EVENT_TYPE_INT_ERROR, 0, nullptr, live_context);
    assert(!media.ready && !media.failed && media.frames==0);
    auto* replacement_context = StarfishMediaAPIs::context;
    callback(PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED, 0, nullptr, replacement_context);
    assert(media.ready);
    media.close();
    assert(open(MirrorCodec::H264,false));
    assert(StarfishMediaAPIs::payload.find("\"video\":\"H264\"")!=std::string::npos);
    assert(StarfishMediaAPIs::payload.find("\"audio\":")==std::string::npos);
    assert(StarfishMediaAPIs::payload.find("\"pcmInfo\":")==std::string::npos);
    replacement_context=StarfishMediaAPIs::context;
    StarfishMediaAPIs::unload_exception = "unload failed for https://SECRET.invalid/?token=PRIVATE";
    media.close();
    StarfishMediaAPIs::unload_exception = nullptr;
    std::thread late([&] { callback(PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED, 0, nullptr, replacement_context); });
    late.join();
    assert(!media.ready);
    assert(!media.direct_released() && !open() && StarfishMediaAPIs::unloads == 4);
    std::puts("PASS: Starfish BUFFERSTREAM schema, startup pause, delayed callbacks, and unload isolation");
}
