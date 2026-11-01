#include "audio.h"
#include "h264.h"
#include "hevc_stream.h"
#include "hevc_startup.h"
#include "media.h"
#include "receiver.h"
#include "shell.h"
#include "identity.h"
#include "settings.h"
#include "usage_stats.h"
#include "system_bus.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>

static long process_rss_kb() {
    std::ifstream status("/proc/self/status");
    std::string key, unit;
    long value=0;
    while(status>>key) {
        if(key=="VmRSS:") { status>>value>>unit;return value; }
        std::string rest;std::getline(status,rest);
    }
    return -1;
}

static std::string fmt(const char *format,double value) {
    char text[80];snprintf(text,sizeof(text),format,value);return text;
}
static std::string duration_text(uint64_t milliseconds) {
    uint64_t seconds=milliseconds/1000;
    char text[80];
    if(seconds<3600)snprintf(text,sizeof(text),"%llum %02llus",(unsigned long long)(seconds/60),(unsigned long long)(seconds%60));
    else snprintf(text,sizeof(text),"%lluh %02llum",(unsigned long long)(seconds/3600),(unsigned long long)((seconds/60)%60));
    return text;
}
static const char *audio_codec_name(unsigned code) {
    switch(code) {case 1:return "PCM";case 2:return "ALAC";case 4:return "AAC-LC";case 8:return "AAC-ELD";default:return "Unknown";}
}
static std::string utc_text(uint64_t epoch) {
    if(!epoch)return "Never";
    time_t stamp=time_t(epoch);tm value;
    if(!gmtime_r(&stamp,&value))return "Unknown";
    char text[48];strftime(text,sizeof(text),"%Y-%m-%d %H:%M UTC",&value);return text;
}

// When frames are due. Starfish (webOS 3-4) needs a PTS pre-roll after Load
// and paces by PTS itself. NDL (webOS 5+) renders as data arrives: the schedule
// is anchored to the first frame's arrival, so frames queued during Load go out
// at once, and no video frame waits more than 50 ms after it arrived.
#if defined(UXAIR_MEDIA_NDL)
static int64_t schedule_start(int64_t arrival_ns,int64_t now,int64_t) {return arrival_ns?arrival_ns:now;}
static const int64_t kMaxHoldNs=50000000LL;
#else
static int64_t schedule_start(int64_t,int64_t now,int64_t preroll_ns) {return now+preroll_ns;}
static const int64_t kMaxHoldNs=0;
#endif

int main(int argc,char **argv) {
    umask(0077);if(!start_log("uxair.log"))return 1;
    Shell shell;if(!shell.open())return 1;
    shell.screen("Starting AirPlay receiver","");
    const std::string state_dir=data_directory()+"/state";
    std::string build_commit="Unknown",build_version="Unknown";
    {
        std::ifstream build_info((app_directory()+"/build-info.txt").c_str());
        std::string line;
        while(std::getline(build_info,line)) {
            if(line.compare(0,7,"commit=")==0)build_commit=line.substr(7,12);
            if(line.compare(0,8,"version=")==0)build_version=line.substr(8);
        }
    }
    Settings settings;
    const std::string settings_path=state_dir+"/settings-v1";
    if(load_settings(settings_path,settings)) {
        fprintf(stderr,"[settings] loaded quality=%dx%d/%d codec=%s hdr10-pipeline=disabled diagnostics=%s\n",settings.width,settings.height,settings.max_fps,settings.codec==MirrorCodec::ExperimentalHevc?"experimental-HEVC":"H264",diagnostics_mode_name(settings.diagnostics));
    } else {
        struct stat saved_settings;
        if(stat(settings_path.c_str(),&saved_settings)==0) {
            settings.width=1280;settings.height=720;settings.max_fps=30;settings.codec=MirrorCodec::H264;
            fprintf(stderr,"[settings] saved settings invalid; preserving file and using safe H264 720p/30 fallback\n");
        } else fprintf(stderr,"[settings] no saved profile; using factory H264 1080p/30 defaults\n");
    }
    UsageStats usage;
    if(!load_usage_stats(state_dir+"/usage-v1",usage) && usage.storage_corrupt)fprintf(stderr,"[stats] usage history is corrupt or oversized; preserving it until explicit reset\n");
    Receiver receiver;
    bool receiver_started=receiver.start(state_dir,settings);
    if(!receiver_started) {
        receiver.stop();
        if(settings.width==1920 || settings.codec==MirrorCodec::ExperimentalHevc || settings.max_fps==60) {
            fprintf(stderr,"[settings] receiver start failed; trying H264 720p/30 recovery\n");
            set_mirror_codec(settings,MirrorCodec::H264);
            set_mirror_resolution(settings,1280,720);
            set_mirror_fps(settings,30);
            receiver_started=receiver.start(state_dir,settings);
            if(receiver_started) {
                fprintf(stderr,"[settings] startup fallback active for this run; saved preference retained\n");
                receiver.show("Quality startup failed. H.264 720p active for this run; saved preference retained.");
            } else receiver.stop();
        }
        if(!receiver_started) {
            shell.screen("Could not start. See uxair.log","");
            while(shell.pump())SDL_Delay(50);
            return 1;
        }
    }
    auto activate_profile=[&](const Settings& requested)->bool {
        // raop_destroy joins its HTTP thread. Let in-flight callbacks finish
        // before clearing the state they may still update.
        receiver.stop();receiver.disconnect();
        if(receiver.start(state_dir,requested))return true;
        receiver.stop();return false;
    };
    const char *runtime_app_id=getenv("APPID");
    const char *app_id=runtime_app_id&&*runtime_app_id?runtime_app_id:UXAIR_APP_ID;
    fprintf(stderr,"[media-identity] runtime-source=%s runtime-app-id=%s compiled-app-id=%s match=%d\n",
            runtime_app_id&&*runtime_app_id?"APPID":"compiled-fallback",app_id,UXAIR_APP_ID,
            int(std::string(app_id)==UXAIR_APP_ID));
    Media media;AudioDecoder decoder;HevcStream hevc;HevcStartupGate hevc_startup;
    // the TV's system bus: screen saver, coming to the front, going back
    SystemBus bus;
    const bool bus_ready=bus.start(app_id);
#if defined(UXAIR_MEDIA_NDL)
    bool background_enabled=true;
#else
    bool background_enabled=false;
#endif
    if(const char *option=getenv("UXAIR_BACKGROUND"))background_enabled=strcmp(option,"1")==0;
    background_enabled=background_enabled && bus_ready;
    unsigned seen_stream_requests=0;
    // started at boot with {"background":true}: stay discoverable behind the Home
    // screen. Asked once the window is up - earlier, UxAir's own window comes to
    // the front after the Home screen.
    bool start_in_background=background_enabled && argc>1 && strstr(argv[1],"\"background\":true");
    std::string return_to;   // {"return_to":"<app>"}: the app that was in front then
    if(argc>1) {
        const char *r=strstr(argv[1],"\"return_to\"");
        const char *q=r?strchr(r+11,'"'):nullptr;
        const char *e=q?strchr(q+1,'"'):nullptr;
        if(e)return_to.assign(q+1,e);
    }
    UiNavigation nav;
    UiPage& page=nav.page;
    int& focus=nav.focus;
    bool ui_visible=true,session_success=false,donation_open=false;
    UiHistoryData history_data;
    bool history_dirty=true;
    uint32_t history_day=0;
    size_t history_storage=usage_storage_bytes(state_dir+"/usage-v1");
    std::string last_rendered_pin;
    std::vector<std::string> last_overlay_lines;
    bool pairing_overlay_dirty=false;
    uint64_t session_started_utc=0, ingress_at_start=0, drops_at_start=0,video_drops_at_start=0,hardware_decoder_errors=0;
    int observed_width=0,observed_height=0;
    MirrorCodec session_codec=MirrorCodec::H264;
    unsigned decoder_reinitializations=0;
    std::string fallback_reason;
    double live_incoming_fps=0,live_feed_fps=0,live_pts_fps=0,live_decoder_callbacks_fps=0,live_queue_ms=0,live_feed_ms=0;
    uint64_t live_ingress_last=0,live_feed_last=0;
    ReceiverDiagnostics live_queue;
    long live_rss_kb=-1;
    bool opened=false,have_pending=false,last_paused=false,audio_started=false,audio_only_playback=false,audio_only_failed=false,audio_flush_paused=false,hevc_wait_keyframe=false;
    bool hevc_preroll_wait_logged=false,hevc_keyframe_wait_logged=false;
    bool hevc_schedule_wait_logged=false,hevc_invalid_logged=false;
    bool hevc_first_feed_logged=false;
    int64_t hevc_configuration_wait=0;
    int width=settings.width,height=settings.height;
    Packet pending;
    std::vector<uint8_t> configuration;
    uint64_t origin=0;
    int64_t audio_end=0;
    std::vector<uint8_t> silence(441*4,0);
    int64_t start=0,last_ui=0,last_stats=now_ns(),last_live=last_stats,stall=0,session_started=0,app_started=last_stats;
    // Now Playing (audio-only): last draw, last full-brightness moment, shown track and progress
    int64_t np_drawn=0,np_bright=0,np_flash=0,np_tick=0,np_progress_seen=0,np_fed_at=0;uint64_t np_generation=0,np_fed=0;double np_elapsed=0;std::string np_track;bool np_anim=false;
    unsigned video_count=0,audio_count=0,decode_errors=0,late_audio=0,silence_blocks=0,invalid_video=0;
    unsigned sessions_started=0,sessions_ended=0,last_decoder_events=0,hevc_transport_failures=0;
    uint64_t last_feed_video=0,last_feed_audio=0,last_feed_video_retries=0,last_feed_audio_retries=0;
    uint64_t audio_decode_calls=0,audio_decode_ns=0,audio_decode_max_ns=0;
    VideoTiming video_feed_timing;
    DurationWindow video_parse_time, video_schedule_late, video_pending_time;
    DurationWindow video_feed_time, audio_feed_time, media_tick_time, loop_gap_time;
    int64_t previous_loop=0;
    NowPlayingCadence np_cadence;DurationWindow np_render_time;
    auto finish=[&](bool error=false,bool preserve_receiver=false,bool preserve_pending=false){
        fprintf(stderr,"[session] generation=%u finish error=%d opened=%d audio_only=%d preserve_receiver=%d\n",sessions_started,error,opened,audio_only_playback,preserve_receiver);
        if(!preserve_receiver)receiver.begin_close();
        if(opened && media.playing && (video_count || (audio_only_playback && audio_count)))session_success=true;
        if(error && !opened) {++usage.session_errors;save_usage_stats(state_dir+"/usage-v1",usage);history_storage=usage_storage_bytes(state_dir+"/usage-v1");history_dirty=true;}
        if(opened) {
            ++sessions_ended;
            fprintf(stderr,"[session] ended duration_s=%.1f started=%u ended=%u\n",session_started?(now_ns()-session_started)/1e9:0.0,sessions_started,sessions_ended);
            if(session_success) {
                SessionSummary summary;
                summary.started_utc=session_started_utc;
                summary.duration_ms=uint64_t(std::max<int64_t>(0,now_ns()-session_started))/1000000;
                summary.kind=audio_only_playback?SessionKind::Audio:SessionKind::Mirror;
                summary.width=audio_only_playback?0:unsigned(observed_width);summary.height=audio_only_playback?0:unsigned(observed_height);
                summary.codec=audio_only_playback?0:session_codec==MirrorCodec::ExperimentalHevc?2:1;
                summary.incoming_frames=receiver.total_video_arrivals.load()-ingress_at_start;
                summary.feed_frames=video_count;
                ReceiverDiagnostics final_queue=receiver.queue_snapshot();
                summary.packet_drops=final_queue.dropped_packets-drops_at_start;
                summary.video_frame_drops=final_queue.dropped_video_frames-video_drops_at_start;
                summary.decoder_errors=decode_errors+hardware_decoder_errors;
                summary.session_errors=error?1:0;
                usage.record(summary);history_dirty=true;
            } else if(error) {
                ++usage.session_errors;
                usage.decoder_errors+=decode_errors+hardware_decoder_errors;
                ReceiverDiagnostics final_queue=receiver.queue_snapshot();
                usage.packet_drops+=final_queue.dropped_packets-drops_at_start;
                usage.video_frame_drops+=final_queue.dropped_video_frames-video_drops_at_start;
            }
            if(!save_usage_stats(state_dir+"/usage-v1",usage))fprintf(stderr,"[stats] could not save usage history\n");
            history_storage=usage_storage_bytes(state_dir+"/usage-v1");history_dirty=true;
        }
        media.close();decoder.reset();hevc.reset();hevc_startup.reset();if(!preserve_receiver){receiver.disconnect();if(background_enabled)bus.return_to_previous();}opened=false;have_pending=preserve_pending;
        origin=0;start=0;stall=0;session_started=0;audio_end=0;configuration.clear();last_ui=0;last_paused=false;audio_started=false;audio_only_playback=false;audio_flush_paused=false;
        session_success=false;session_started_utc=0;observed_width=observed_height=0;hardware_decoder_errors=0;
        nav=UiNavigation();ui_visible=true;np_drawn=np_progress_seen=np_flash=np_tick=np_fed_at=0;np_fed=0;np_anim=false;np_elapsed=0;np_track.clear();
        video_feed_timing.reset();video_parse_time.reset();video_schedule_late.reset();video_pending_time.reset();
        video_feed_time.reset();audio_feed_time.reset();media_tick_time.reset();
        width=settings.width;height=settings.height;
        session_codec=MirrorCodec::H264;hevc_wait_keyframe=false;
        hevc_preroll_wait_logged=hevc_keyframe_wait_logged=false;
        hevc_schedule_wait_logged=hevc_invalid_logged=false;
        hevc_first_feed_logged=false;hevc_configuration_wait=0;
    };
    auto recover_decoder=[&](bool failed_hevc,const char *reason){
        const bool starfish_load_failure=media.open_failure()==MediaOpenFailure::StarfishLoad;
        if(failed_hevc) {
            fallback_reason=reason;
            fprintf(stderr,"[codec] fallback reason=%s next-session=H264-720p\n",reason);
            Settings previous=settings;
            set_mirror_codec(settings,MirrorCodec::H264);
            set_mirror_resolution(settings,1280,720);
            set_mirror_fps(settings,30);
            if(activate_profile(settings)) {
                fprintf(stderr,"[settings] HEVC fallback is session-only; saved preference retained\n");
                receiver.show(starfish_load_failure?"Starfish Load failed. H.264 720p active; reconnect sender.":"HEVC failed. H.264 720p active; reconnect sender.");
            } else {
                settings=previous;
                bool restored=activate_profile(previous);
                receiver.show(restored?"HEVC fallback failed; previous profile restored.":"HEVC fallback failed. Restart app.");
            }
            return;
        }
        if(settings.max_fps==60) {
            Settings previous=settings;
            set_mirror_fps(settings,30);
            if(activate_profile(settings)) {
                fprintf(stderr,"[settings] decoder recovery lowered advertised max FPS to 30 for this run; saved preference retained\n");
                receiver.show(starfish_load_failure?"Starfish Load failed. Restored 30 FPS; reconnect sender.":"60 FPS failed. Restored 30 FPS; reconnect sender.");
            } else {
                settings=previous;
                bool restored=activate_profile(previous);
                receiver.show(restored?"30 FPS recovery failed; previous profile restored.":"FPS recovery failed. Restart app.");
            }
            return;
        }
        if(settings.width!=1920) {
            receiver.show(starfish_load_failure?"Starfish Load failed. Adjust Settings or reconnect.":"TV decoder failed. Connect again.");return;
        }
        ProfileSwitchResult result=switch_mirror_profile(settings,1280,720,activate_profile);
        if(result==ProfileSwitchResult::Applied) {
            fprintf(stderr,"[settings] 720p recovery is session-only; saved preference retained\n");
            receiver.show(starfish_load_failure?"Starfish Load failed. Restored 720p; reconnect sender.":"1080p decoder failed. Restored 720p; reconnect sender.");
            fprintf(stderr,"[settings] 1080p decoder failed; restored 720p advertisement\n");
        } else receiver.show(starfish_load_failure?"Starfish Load failed. Adjust Settings or reconnect.":"Decoder failed. Restart app and select 720p.");
    };
    auto report_media_start_failure=[&](bool failed_hevc,const char *reason){
        const MediaOpenFailure failure=media.open_failure();
        finish(true);
        recover_decoder(failed_hevc,reason);
        receiver.show(std::string(media_open_failure_message(failure))+" Settings remain available; reconnect to retry.");
    };
    auto flush_audio=[&](){
        decoder.reset();
        apply_audio_flush(audio_only_playback,have_pending && pending.video,have_pending,stall,audio_flush_paused);
        fprintf(stderr,"[session] generation=%u transition=audio-flush epoch=%u paused=%d pending=%d\n",
                sessions_started,receiver.audio_epoch.load(),int(audio_flush_paused),int(have_pending));
    };
    auto recover_playback=[&](const char *reason){
        Settings saved;
        const bool saved_ok=load_settings(settings_path,saved);
        fprintf(stderr,"[session] generation=%u recovery=%s runtime=%dx%d/%d codec=%d saved_available=%d saved=%dx%d/%d codec=%d\n",
                sessions_started,reason,settings.width,settings.height,settings.max_fps,int(settings.codec),int(saved_ok),
                saved.width,saved.height,saved.max_fps,int(saved.codec));
        const bool audio_only=audio_only_playback;
        const bool failed_hevc=session_codec==MirrorCodec::ExperimentalHevc;
        finish(true);
        if(audio_only) {
            fprintf(stderr,"[audio-only] playback failed reason=%s; video profile left unchanged\n",reason);
            receiver.show("Audio-only playback stopped. Reconnect; mirroring settings were preserved.");
        } else recover_decoder(failed_hevc,reason);
    };
    auto rows_for=[&](UiPage which)->std::vector<UiRow> {
        std::vector<UiRow> rows;
        if(which==UiPage::Mirroring) {
            rows.push_back(UiRow("Resolution",settings.width==3840?"4K":settings.width==1920?"1080p":"720p","H.264: up to 1080p; HEVC: up to 4K. Frame rate is configured separately.",receiver.idle_for_profile_change()));
            rows.push_back(UiRow("Video codec",settings.codec==MirrorCodec::ExperimentalHevc?"H.265 (HEVC)":"H.264","Choose the video format. H.265 enables 4K.",receiver.idle_for_profile_change()));
            rows.push_back(UiRow("Maximum frame rate",std::to_string(settings.max_fps)+" FPS",settings.max_fps==60?"Advertised maximum; the sender may choose less":"30 FPS maximum",receiver.idle_for_profile_change()));

        } else if(which==UiPage::Media) {
            rows.push_back(UiRow("Audio delay",std::to_string(receiver.audio_delay_ms.load())+" ms","Set before connecting; step size 25 ms",receiver.idle_for_profile_change()));

        } else if(which==UiPage::Diagnostics) {
            rows.push_back(UiRow("On-screen overlay",diagnostics_mode_name(settings.diagnostics),"Off, Compact or Detailed"));
            std::string resolution=observed_width?std::to_string(observed_width)+" x "+std::to_string(observed_height):"Awaiting SPS";
            rows.push_back(UiRow("Negotiated codec / SPS",std::string(receiver.incoming_video_codec==2?"HEVC":receiver.incoming_video_codec==1?"H.264":"Awaiting codec")+"  "+resolution,
                "Receiver limit "+std::to_string(settings.width)+"x"+std::to_string(settings.height)+" / "+std::to_string(settings.max_fps)+" FPS max; iOS selects codec/rate"));
            rows.push_back(UiRow("Incoming / Feed FPS",fmt("%.1f",live_incoming_fps)+" / "+fmt("%.1f",live_feed_fps),
                "Measured over 1s; advertised maximum "+std::to_string(settings.max_fps)+" FPS"));
            rows.push_back(UiRow("Video timing / output",fmt("%.1f",live_pts_fps)+" PTS FPS / unmeasured",
                "Decoder callbacks/s "+fmt("%.1f",live_decoder_callbacks_fps)+" (not physical display FPS); Starfish "+std::to_string(media.video_info_width.load())+"x"+std::to_string(media.video_info_height.load())+"; ACB "+std::to_string(media.acb_window_width.load())+"x"+std::to_string(media.acb_window_height.load())));
            rows.push_back(UiRow("Video / audio queue",std::to_string(live_queue.video_queue_packets)+" / "+std::to_string(live_queue.queue_packets-live_queue.video_queue_packets),"Video queue residence "+fmt("%.1f",live_queue_ms)+" ms, last 5s"));
            rows.push_back(UiRow("Starfish",media.failed?"Error":media.playing?"Playing":media.ready?"Loaded":opened?"Loading":"Idle",
                "Feed "+std::to_string(media.feed_video_accepted.load())+" accepted / "+std::to_string(media.feed_video_rejected.load())+" rejected; "+fmt("%.1f",live_feed_ms)+" ms avg"));
            rows.push_back(UiRow("Session / memory",session_started?duration_text(uint64_t(std::max<int64_t>(0,now_ns()-session_started))/1000000):"Idle",
                (live_rss_kb>=0?fmt("%.1f",live_rss_kb/1024.0)+" MiB RSS":"RSS unavailable")+"; "+std::to_string(usage.successful_sessions)+" sessions; "+
                (fallback_reason.empty()?"no fallback":"fallback "+fallback_reason)));
            rows.push_back(UiRow("Incoming audio",receiver.has_audio?audio_codec_name(receiver.last_audio_codec.load()):"Awaiting audio","Negotiated AirPlay audio codec"));
            rows.push_back(UiRow("PCM output","Stereo 44.1 kHz","Decoded locally and sent to Starfish"));
            const AirPlayAudioIntent audio_intent{receiver.last_audio_codec.load(),receiver.audio_samples_per_frame.load(),receiver.audio_using_screen.load(),receiver.audio_is_media.load(),receiver.has_audio.load()};
            rows.push_back(UiRow("Audio playback",audio_only_playback?(media.playing?"Standalone playing":"Standalone starting"):
                standalone_audio_candidate(audio_intent,receiver.mirror.load())?"Standalone ready":"Idle",
                "Protected or unsupported sessions are not decoded"));
            rows.push_back(UiRow("Mirroring audio",receiver.mirror?"Active":"Idle","Stereo audio during screen mirroring"));
        } else if(which==UiPage::History) {
            for(const SessionSummary& session:usage.recent) {
                std::string kind=session.kind==SessionKind::Audio?"AirPlay music":
                    std::string(session_kind_name(session.kind))+" - "+(session.codec==2?"HEVC":"H.264")+" - "+
                    std::to_string(session.width)+"x"+std::to_string(session.height);
                rows.push_back(UiRow(utc_text(session.started_utc),duration_text(session.duration_ms),kind,false));
            }
            rows.push_back(UiRow("Reset history","Select","Permanently clears saved usage statistics"));
        } else if(which==UiPage::System) {
            rows.push_back(UiRow("UxAir",build_version,"AirPlay Receiver for webOS",false));
            rows.push_back(UiRow("Developer","Matej Skuhala (@depius)","Based on UxPlay by FDH2; uxplay-webOS by @danlflh",false));
            rows.push_back(UiRow("License","GPL-3.0-or-later","See bundled license and third-party notices",false));
            rows.push_back(UiRow("Enjoying UxAir? Buy me a coffee","","Support development · UxAir is free and open source"));
        }
        return rows;
    };
    auto dialog_for=[&]()->UiDialog {
        UiDialog d;d.selected=nav.choice;
        switch(nav.dialog) {
            case UiDialogKind::Resolution:
                d.current=settings.width==3840?2:settings.width==1920?1:0;
                d.title="Screen mirroring resolution";d.description="Frame rate is selected separately. Disconnect sender before applying.";
                d.choices={"720p","1080p","4K"};
                break;
            case UiDialogKind::Codec:
                d.current=settings.codec==MirrorCodec::ExperimentalHevc?1:0;
                d.title="Screen mirroring codec";d.description="Choose the video format. H.265 enables 4K.";
                d.choices={"H.264","H.265 (HEVC)"};break;
            case UiDialogKind::FrameRate:
                d.current=settings.max_fps==60?1:0;
                d.title="Maximum mirroring frame rate";d.description="This is an AirPlay limit; the sender may choose a lower rate.";
                d.choices={"30 FPS","60 FPS"};break;
            case UiDialogKind::Overlay:
                d.current=int(settings.diagnostics);
                d.title="Playback diagnostics overlay";d.description="Incoming and feed FPS are not displayed FPS.";
                d.choices={"Off","Compact","Detailed"};break;
            case UiDialogKind::AudioDelay:
                d.current=(receiver.audio_delay_ms.load()+1000)/25;
                d.title="Audio delay";d.description="Choose before connecting; 25 ms steps.";
                for(int ms=-1000;ms<=1000;ms+=25)d.choices.push_back(std::to_string(ms)+" ms");break;
            case UiDialogKind::ResetHistory:
                d.title="Reset usage history?";d.description="This permanently deletes saved sessions and usage statistics. Pairing and settings will not be affected.";
                d.choices={"Cancel","Reset history"};break;
            default:break;
        }
        return d;
    };
    auto draw_page=[&](const std::string& subtitle,const std::string& footer) {
        const std::string pin=receiver.pin();
        UiDialog d=dialog_for();
        const std::vector<UiRow> rows=rows_for(page);
        if(page==UiPage::History) {
            const uint32_t today=usage_utc_day(uint64_t(time(nullptr)));
            if(today!=history_day){history_day=today;history_dirty=true;}
            const size_t current_storage=usage_storage_bytes(state_dir+"/usage-v1");
            if(current_storage!=history_storage){history_storage=current_storage;history_dirty=true;}
        }
        if(page==UiPage::History && history_dirty) {
            const uint32_t today=usage_utc_day(uint64_t(time(nullptr)));
            UsageSummary thirty=summarize_usage(usage,UsagePeriod::Days30,today);
            UsageSummary days=summarize_usage(usage,UsagePeriod::ThisMonth,today);
            UsageSummary months=summarize_usage(usage,UsagePeriod::ThisYear,today);
            history_data=UiHistoryData();
            history_data.session_count=usage.recent.size();history_data.empty=usage.recent.empty();
            uint64_t sessions=usage.successful_sessions;
            history_data.summary=std::to_string(sessions)+" completed";
            history_data.total_time=duration_text(usage.playback_ms);
            history_data.average=sessions?duration_text(usage.playback_ms/sessions):"No usage";
            history_data.mirror_time=duration_text(thirty.mirror_ms);
            history_data.music_time=duration_text(thirty.audio_ms);
            history_data.mirror_percent=usage_mirror_percent(thirty.mirror_ms,thirty.audio_ms);
            history_data.audio_percent=(thirty.mirror_ms||thirty.audio_ms)?100-history_data.mirror_percent:0;
            history_data.storage=usage.storage_corrupt?"History file unreadable; reset to restart":"History storage "+std::to_string((history_storage+1023)/1024)+" KB | "+std::to_string(usage.recent.size())+"/100 sessions | 365d";
            history_data.daily_title=days.title;history_data.monthly_title=months.title;
            history_data.daily_bars=days.bars;history_data.monthly_bars=months.bars;
            history_dirty=false;
        }
        shell.page(page,subtitle,rows,focus,footer,
                   nav.dialog==UiDialogKind::None?nullptr:&d,pin,donation_open,page==UiPage::History?&history_data:nullptr);
    };
    auto apply_resolution=[&](int target) {
        if(!receiver.idle_for_profile_change()) {receiver.show("Disconnect sender before changing quality.");return;}
        const Settings previous=settings;
        ProfileSwitchResult result=switch_mirror_profile(settings,target,target==3840?2160:target==1920?1080:720,activate_profile);
        if(result==ProfileSwitchResult::Applied) {
            if(save_settings(state_dir+"/settings-v1",settings))
                receiver.show(std::string(target==3840?"4K HEVC":target==1920?"1080p":"720p")+" selected. Reconnect; check incoming SPS.");
            else {
                fprintf(stderr,"[settings] save failed; restoring previous receiver profile\n");
                bool restored=activate_profile(previous);settings=previous;
                receiver.show(restored?"Could not save quality; previous profile restored.":"Quality save and recovery failed. Restart app.");
            }
        } else if(result==ProfileSwitchResult::Recovered) receiver.show("Quality start failed; previous profile restored.");
        else if(result==ProfileSwitchResult::Unavailable) receiver.show("Receiver unavailable. Restart app to recover.");
    };
    auto apply_fps=[&](int fps) {
        if(!receiver.idle_for_profile_change()) {receiver.show("Disconnect sender before changing frame rate.");return;}
        const Settings previous=settings;
        Settings proposed=settings;
        if(!set_mirror_fps(proposed,fps) || proposed.max_fps==settings.max_fps)return;
        if(!activate_profile(proposed)) {
            bool restored=activate_profile(previous);
            receiver.show(restored?"Frame-rate start failed; previous setting restored.":"Receiver unavailable. Restart app.");
            return;
        }
        if(!save_settings(state_dir+"/settings-v1",proposed)) {
            bool restored=activate_profile(previous);
            receiver.show(restored?"Frame-rate save failed; previous setting restored.":"Frame-rate save failed. Restart app.");
            return;
        }
        settings=proposed;
        receiver.show(fps==60?"60 FPS maximum advertised. Reconnect; sender may choose less.":"30 FPS maximum active. Reconnect sender.");
    };
    auto apply_codec=[&](MirrorCodec codec) {
        if(!receiver.idle_for_profile_change()) {receiver.show("Disconnect sender before changing codec.");return;}
        const Settings previous=settings;
        Settings proposed=settings;
        if(!set_mirror_codec(proposed,codec) || proposed.codec==settings.codec)return;
        if(!activate_profile(proposed)) {
            bool restored=activate_profile(previous);
            receiver.show(restored?"Codec start failed; previous profile restored.":"Receiver unavailable. Restart app.");
            return;
        }
        if(!save_settings(state_dir+"/settings-v1",proposed)) {
            bool restored=activate_profile(previous);
            receiver.show(restored?"Codec save failed; previous profile restored.":"Codec save failed. Restart app.");
            return;
        }
        settings=proposed;
        fallback_reason.clear();
        receiver.show(codec==MirrorCodec::ExperimentalHevc?
            "HEVC enabled; iOS chooses codec. Check actual codec after reconnect.":
            "H.264 active. Reconnect sender.");
    };
    bool running=true;
    while(running) {
        int64_t now=now_ns();
        if(previous_loop)loop_gap_time.record(uint64_t(std::max<int64_t>(0,now-previous_loop)));
        previous_loop=now;
        Action action=shell.action();
        if(action==Action::Quit || (action==Action::Exit && !background_enabled))break;
        if(action!=Action::None)np_bright=now;   // any key brightens Now Playing again
        bus.block_screen_saver(opened && media.playing && !last_paused && !audio_flush_paused && !ui_visible);
        const unsigned stream_requests=receiver.stream_requests.load();
        if(stream_requests!=seen_stream_requests || action!=Action::None || !receiver.pin().empty())start_in_background=false;
        if(start_in_background && now-app_started>2500000000LL) {
            start_in_background=false;fprintf(stderr,"[bus] started in the background\n");
            bus.to_background(return_to.empty()?nullptr:return_to.c_str());
        }
        // a device starts streaming (or needs the PIN): to the front, as LG's AirPlay
        if(stream_requests!=seen_stream_requests) {seen_stream_requests=stream_requests;if(background_enabled)bus.bring_to_front();}
        if(shell.take_pointer_motion()&&ui_visible&&page==UiPage::History)last_ui=0;
        // Background execution is opt-in for the Starfish/webOS 3-4 build.
        if(action==Action::Exit) {
            if(opened||receiver.mirror||!receiver.pin().empty())finish();
            else bus.to_background();
            action=Action::None;
        }
        std::string pairing_pin=receiver.pin();
        const bool pairing_changed=pairing_pin!=last_rendered_pin;
        if(pairing_changed) {
            fprintf(stderr,"[pairing] UI observed prompt=%s\n",pairing_pin.empty()?"dismissed":"shown");
            last_rendered_pin=pairing_pin;last_ui=0;
            if(opened && !ui_visible)pairing_overlay_dirty=true;
            if(!pairing_pin.empty() && background_enabled)bus.bring_to_front();
        }
        const PairingInputResult pairing_input=pairing_input_result(!pairing_pin.empty(),action==Action::Back,action==Action::Exit);
        if((pairing_input==PairingInputResult::Consumed || donation_open) && action!=Action::Back)action=Action::None;
        if(opened && !ui_visible && action==Action::Select) {
            fprintf(stderr,"[ui] remote opened session settings\n");
            ui_visible=true;nav.category(3);action=Action::None;last_ui=0;
        }
        if(action==Action::Select && ui_visible) {
            int x,y;
            if(shell.pointer_select(x,y)) {
                UiDialog d=dialog_for();
                UiHit hit=shell.hit_test(x,y,int(rows_for(page).size()),nav.dialog==UiDialogKind::None?nullptr:&d,focus,page);
                if(hit.kind==UiHitKind::Category) {nav.category(hit.index);action=Action::None;}
                else if(hit.kind==UiHitKind::Row) focus=hit.index;
                else if(hit.kind==UiHitKind::DialogChoice) nav.choice=hit.index;
                else if(hit.kind==UiHitKind::Cancel) {nav.back();action=Action::None;}
                else action=Action::None;
                last_ui=0;
            }
        }
        if(action==Action::Back) {
            if(pairing_input==PairingInputResult::Cancel) {
                fprintf(stderr,"[pairing] Back requested cancellation; restarting RAOP session\n");
                if(activate_profile(settings))receiver.show("Pairing cancelled. Ready to connect.");
                else receiver.show("Pairing cancelled; receiver restart failed. Restart app.");
                pairing_pin=receiver.pin();last_rendered_pin=pairing_pin;last_ui=0;
                if(opened && !ui_visible)pairing_overlay_dirty=true;
                action=Action::None;
            }
            else if(donation_open) {donation_open=false;last_ui=0;}
            else if(ui_visible && nav.dialog!=UiDialogKind::None) {nav.back();last_ui=0;}
            else if(opened && ui_visible) {ui_visible=false;shell.transparent();last_ui=0;np_drawn=0;}
            else if(ui_visible && nav.back()) {last_ui=0;}
            else if(opened||receiver.mirror||!receiver.pin().empty())finish();
            else if(background_enabled)bus.to_background();
            else break;
        }
        if(ui_visible && (action==Action::Up||action==Action::Down)) {
            int delta=action==Action::Down?1:-1;
            if(nav.dialog==UiDialogKind::None)nav.move_row(delta,int(rows_for(page).size()));
            else nav.move_choice(delta,int(dialog_for().choices.size()));
            last_ui=0;
        }
        if(ui_visible && (action==Action::Left||action==Action::Right) && nav.dialog==UiDialogKind::None) {
            nav.move_category(action==Action::Right?1:-1);last_ui=0;
        }
        if(ui_visible && action==Action::Select) {
            if(nav.dialog!=UiDialogKind::None) {
                UiDialogKind kind=nav.dialog;
                int choice=nav.confirm();
                if(kind==UiDialogKind::Resolution && choice>=0 && choice<=2)apply_resolution(choice==2?3840:choice?1920:1280);
                else if(kind==UiDialogKind::Codec && choice>=0 && choice<=1)apply_codec(choice?MirrorCodec::ExperimentalHevc:MirrorCodec::H264);
                else if(kind==UiDialogKind::FrameRate && choice>=0 && choice<=1)apply_fps(choice?60:30);
                else if(kind==UiDialogKind::Overlay && choice>=0 && choice<=2) {
                    settings.diagnostics=DiagnosticsMode(choice);
                    if(!save_settings(state_dir+"/settings-v1",settings))fprintf(stderr,"[settings] save failed\n");
                } else if(kind==UiDialogKind::AudioDelay && choice>=0 && choice<=80 && receiver.idle_for_profile_change())
                    receiver.adjust_audio_delay(choice*25-1000-receiver.audio_delay_ms.load());
                else if(kind==UiDialogKind::ResetHistory && choice==1) {
                    usage.reset();if(!save_usage_stats(state_dir+"/usage-v1",usage)){fprintf(stderr,"[stats] reset save failed\n");receiver.show("History cleared in memory, but could not save the reset.");}history_storage=usage_storage_bytes(state_dir+"/usage-v1");history_dirty=true;
                }
            } else {
                const std::vector<UiRow> rows=rows_for(page);
                if(focus>=0 && focus<int(rows.size()) && rows[focus].enabled) {
                    if(page==UiPage::Mirroring && focus==0)nav.begin(UiDialogKind::Resolution,settings.width==3840?2:settings.width==1920?1:0);
                    else if(page==UiPage::Mirroring && focus==1)nav.begin(UiDialogKind::Codec,settings.codec==MirrorCodec::ExperimentalHevc?1:0);
                    else if(page==UiPage::Mirroring && focus==2)nav.begin(UiDialogKind::FrameRate,settings.max_fps==60?1:0);
                    else if(page==UiPage::Diagnostics && focus==0)nav.begin(UiDialogKind::Overlay,int(settings.diagnostics));
                    else if(page==UiPage::History && focus==int(usage.recent.size()))nav.begin(UiDialogKind::ResetHistory,0);
                    else if(page==UiPage::Media && focus==0)nav.begin(UiDialogKind::AudioDelay,(receiver.audio_delay_ms.load()+1000)/25);
                    else if(page==UiPage::System && focus==3)donation_open=true;
                }
            }
            last_ui=0;
        }
        const ResetNotice reset=receiver.take_reset();
        if(reset.pending() && reset.epoch!=receiver.transport_epoch.load())
            fprintf(stderr,"[session] obsolete-reset sources=%u event_epoch=%u current_epoch=%u video_reason=%d transport_reason=%d\n",
                reset.sources,reset.epoch,receiver.transport_epoch.load(),reset.video_reason,reset.transport_reason);
        if(reset.pending() && reset.epoch==receiver.transport_epoch.load()) {
            ReceiverDiagnostics q=receiver.queue_snapshot();
            fprintf(stderr,"[session] transport requested reset generation=%u epoch=%u opened=%d stall_ms=%.1f queue=%zu/%zu\n",
                sessions_started,receiver.transport_epoch.load(),int(opened),stall?(now-stall)/1e6:0.0,q.queue_packets,q.queue_bytes);
            fprintf(stderr,"[session] reset-class=%s sources=%u video_reason=%d transport_reason=%d\n",
                reset.failure()?"failure":"sender-teardown",reset.sources,reset.video_reason,reset.transport_reason);
            audio_only_failed=false;
            bool hevc_startup=reset.failure() && receiver.incoming_video_codec==2 && !opened;
            if(hevc_startup)++hevc_transport_failures;
            finish(reset.failure());
            if(hevc_startup && hevc_transport_failures>=2) {
                recover_decoder(true,"repeated-transport-reset");
                hevc_transport_failures=0;
            }
        }
        if(receiver.activity && now-receiver.activity>20000000000LL && (opened||receiver.mirror)) {
            fprintf(stderr,"[session] idle timeout\n");
            finish(true);receiver.show("Connection lost. Connect again.");
        }
        if(receiver.audio_flush.exchange(false))flush_audio();
        if(have_pending && !pending.video && !current_audio_epoch(pending.audio_epoch,receiver.audio_epoch.load())) {
            have_pending=false;stall=0;
        }
        if(opened) {
            int64_t tick_begin=now_ns();
            media.tick();
            media_tick_time.record(uint64_t(std::max<int64_t>(0,now_ns()-tick_begin)));
            bool paused=audio_only_playback?audio_flush_paused:receiver.paused.load();
            if(paused!=last_paused) {media.pause(paused);last_paused=paused;}
            if(media.failed) {
                ++hardware_decoder_errors;recover_playback("starfish-error");
            }
            if(opened && audio_only_playback && !media.playing && now-session_started>15000000000LL) {
                fprintf(stderr,"[audio-only] startup timeout ready=%d; audio format was negotiated but Starfish did not enter PLAYING\n",int(media.ready.load()));
                recover_playback("audio-startup-timeout");
            }
            if(opened && session_codec==MirrorCodec::ExperimentalHevc &&
               hevc_startup.startup_timed_out(now,media.ready.load(),media.playing.load(),receiver.paused.load())) {
                ReceiverDiagnostics pending_queue=receiver.queue_snapshot();
                fprintf(stderr,"[codec-start] timeout stage=%s ready=%d playing=%d video_feed=%u generation_feed=%u preroll=%u/%zu queue=%zu elapsed_ms=%.0f session_age_ms=%.0f\n",
                    media.ready?"play":"load",int(media.ready.load()),int(media.playing.load()),video_count,hevc_startup.accepted_frames(),
                    hevc_startup.preroll_frames(),hevc_startup.preroll_bytes(),pending_queue.video_queue_packets,
                    hevc_startup.age_ns(now)/1e6,(now-session_started)/1e6);
                ++hardware_decoder_errors;finish(true);recover_decoder(true,"startup-timeout");
            }
            if(opened && media.playing && video_count)session_success=true;
            // Some senders negotiate audio after mirroring starts. Prime the PCM
            // track only until the first real packet arrives. Filling later gaps
            // advances audio_end past delayed RTP packets and drops real audio.
            int fills=0;
            while(opened && !audio_started && !last_paused && now-receiver.last_audio_arrival>500000000 && now-start-audio_end>100000000 && fills++<10) {
                auto r=media.feed(silence.data(),silence.size(),audio_end,false);
                if(r!=FeedResult::Accepted)break;
                audio_end+=10000000;
                ++silence_blocks;
            }
        }
        if(!opened && (page==UiPage::History?(last_ui==0||usage_utc_day(uint64_t(time(nullptr)))!=history_day):now-last_ui>1000000000LL) && (pairing_pin.empty() || pairing_changed)) {
            draw_page(receiver.status(),"Left/Right sections  •  Up/Down focus  •  OK select  •  Back cancel");
            last_ui=now;
        }
        if(now-last_live>1000000000LL) {
            double seconds=(now-last_live)/1e9;
            uint64_t ingress=receiver.total_video_arrivals.load();
            uint64_t feed=media.feed_video_accepted.load();
            live_incoming_fps=(ingress-live_ingress_last)/seconds;
            live_feed_fps=opened?(feed>=live_feed_last?feed-live_feed_last:feed)/seconds:0;
            live_ingress_last=ingress;live_feed_last=feed;
            live_queue=receiver.queue_snapshot();live_rss_kb=process_rss_kb();last_live=now;
            if(opened && !ui_visible && !audio_only_playback && settings.diagnostics!=DiagnosticsMode::Off && pairing_pin.empty()) {
                    std::vector<std::string> lines;
                    if(audio_only_playback) {
                        lines.push_back(std::string("AirPlay audio ")+audio_codec_name(receiver.last_audio_codec.load())+"  -  "+(media.playing?"Playing":media.ready?"Loaded":"Loading"));
                        if(settings.diagnostics==DiagnosticsMode::Detailed) {
                            lines.push_back("PCM output stereo 44.1 kHz  audio Feed "+std::to_string(media.feed_audio_accepted.load())+" accepted");
                            lines.push_back("Audio queue "+std::to_string(live_queue.queue_packets)+" packets  "+fmt("%.1f",live_queue_ms)+" ms");
                        }
                    } else {
                        lines.push_back(std::string(receiver.incoming_video_codec==2?"HEVC ":receiver.incoming_video_codec==1?"H.264 ":"Video ")+
                            (observed_width?std::to_string(observed_width)+"x"+std::to_string(observed_height):"awaiting codec")+"  "+fmt("%.1f",live_incoming_fps)+" incoming FPS  "+
                            (media.playing?"Playing":media.ready?"Loaded":"Loading"));
                        if(settings.diagnostics==DiagnosticsMode::Detailed) {
                            lines.push_back("Feed "+fmt("%.1f",live_feed_fps)+" FPS  Decoder callbacks "+fmt("%.1f",live_decoder_callbacks_fps)+"/s (not display FPS)");
                            lines.push_back("Video queue "+std::to_string(live_queue.video_queue_packets)+"  Drops "+std::to_string(live_queue.dropped_video_frames)+"  Feed rejects "+std::to_string(media.feed_video_rejected.load()));
                            lines.push_back("PTS "+fmt("%.1f",live_pts_fps)+" FPS  Queue "+fmt("%.1f",live_queue_ms)+" ms  Feed "+fmt("%.1f",live_feed_ms)+" ms");
                            lines.push_back(std::string("Audio ")+audio_codec_name(receiver.last_audio_codec.load())+" -> PCM stereo 44.1k");
                            lines.push_back("Starfish "+std::to_string(media.video_info_width.load())+"x"+std::to_string(media.video_info_height.load())+"  ACB "+std::to_string(media.acb_window_width.load())+"x"+std::to_string(media.acb_window_height.load())+"  reinit "+std::to_string(decoder_reinitializations));
                        }
                    }
                    if(settings.diagnostics==DiagnosticsMode::Detailed)
                        lines.push_back("RSS "+(live_rss_kb>=0?fmt("%.1f",live_rss_kb/1024.0)+" MiB":"N/A")+"  Session "+duration_text(uint64_t(std::max<int64_t>(0,now-session_started))/1000000));
                    last_overlay_lines=lines;
                    shell.overlay(last_overlay_lines);
                    pairing_overlay_dirty=false;
            }
        }
        if((page==UiPage::History?(opened&&ui_visible&&last_ui==0):session_ui_redraw_due(opened,ui_visible,last_ui,now)) && (pairing_pin.empty() || pairing_changed)) {
            draw_page("Live session  •  Back returns to playback","Incoming and Feed are receiver rates  •  Displayed FPS unavailable");
            last_ui=now;
        }
        if(pairing_overlay_dirty && opened && !ui_visible) {
            shell.overlay(last_overlay_lines,pairing_pin);
            pairing_overlay_dirty=false;
        }
        // Audio-only: the Now Playing screen (as LG's own AirPlay shows), redrawn
        // Every second for progress, and immediately on metadata changes.
        // OLED dimming remains; whole-layout drift is disabled for B7 diagnosis.
        if(opened && audio_only_playback && !ui_visible && pairing_pin.empty()) {
            NowPlaying m=receiver.now_playing();
            const std::string track=m.artist+" | "+m.title;
            if(track!=np_track) {np_track=track;np_flash=now;np_drawn=0;}
            if(m.generation!=np_generation) {np_generation=m.generation;np_drawn=0;}
            if(np_cadence.due(now,!np_drawn,np_anim)) {
                const int64_t render_begin=now_ns();
                // Playing = audio handed to the player lately. Arrivals do not tell:
                // AirPlay audio comes in bursts well ahead of playback.
                const uint64_t fed=media.feed_audio_accepted.load();
                if(fed!=np_fed) {np_fed=fed;np_fed_at=now;}
                const bool playing=np_fed_at && now-np_fed_at<1500000000LL;
                const uint32_t length=m.end-m.start;   // RTP timestamps wrap; unsigned difference
                if(m.progress_ns && m.progress_ns!=np_progress_seen) {
                    np_progress_seen=m.progress_ns;
                    np_elapsed=uint32_t(m.current-m.start)/44100.0+(playing?(now-m.progress_ns)/1e9:0);
                } else if(playing && np_tick) np_elapsed+=(now-np_tick)/1e9;
                np_tick=now;
                NowPlayingView v;
                v.title=m.title;v.artist=m.artist;v.album=m.album;v.source=m.source;
                // A cover from before the current track (one that came without
                // artwork) is hidden; iOS sends a track's cover just before its text.
                static const std::vector<uint8_t> no_cover;
                const bool stale_cover=m.track_ns>m.cover_ns+5000000000LL && now>m.track_ns+5000000000LL;
                v.cover=stale_cover||!m.cover?&no_cover:m.cover.get();
                v.cover_generation=stale_cover?(m.cover_generation|(uint64_t(1)<<63)):m.cover_generation;
                if(length) {v.duration_s=length/44100.0;v.elapsed_s=std::min(v.duration_s,np_elapsed);}
                // Dimming follows the remote only: 20 s without a key, then down to
                // 35% within 10 s. A new track shows at full brightness for 8 s and
                // fades back over 2 s. Fades redraw at 30 fps.
                const double idle=(now-np_bright)/1e9;
                double brightness=idle<20?1.0:std::max(0.35,1.0-(idle-20)/10*0.65);
                const double since_track=np_flash?(now-np_flash)/1e9:1e9;
                if(since_track<10)brightness=since_track<8?1.0:1.0-(since_track-8)/2*(1.0-brightness);
                v.brightness=float(brightness);
                const bool fading=(idle>=20&&idle<30)||(since_track>=8&&since_track<10);
                np_anim=shell.now_playing(v)||fading;
                np_drawn=now_ns();np_cadence.finish(np_drawn);
                np_render_time.record(uint64_t(std::max<int64_t>(0,np_drawn-render_begin)));
                now=np_drawn;
            }
        }
        now=now_ns(); // Renderer/metadata work must not leave packet pacing on an old clock.
        if(now-last_stats>5000000000LL) {
            double window_s=(now-last_stats)/1e9;
            VideoTimingSample timing=receiver.take_video_timing();
            VideoTimingSample fed=video_feed_timing.take_window();
            ReceiverDiagnostics q=receiver.diagnostics();
            const DurationSample rendered=np_render_time.take_window();
            if(opened && audio_only_playback)fprintf(stderr,"[now-playing-perf] generation=%u draws=%llu render_ms=%.2f/%.2f audio_queue=%zu/%zu audio_pts_ms=%.1f..%.1f queued_span_ms=%.1f head_due_in_ms=%.1f\n",
                sessions_started,(unsigned long long)rendered.count,rendered.average_ms(),rendered.maximum_ms(),
                q.audio_queue_packets,q.audio_queue_bytes,
                q.audio_queue_packets?(int64_t(q.audio_pts_min)-int64_t(origin))/1e6:0.0,
                q.audio_queue_packets?(int64_t(q.audio_pts_max)-int64_t(origin))/1e6:0.0,
                (q.audio_pts_max-q.audio_pts_min)/1e6,
                q.audio_queue_packets?(int64_t(q.audio_pts_min)-int64_t(origin)-(now-start))/1e6:0.0);
            DurationSample parsed=video_parse_time.take_window(),late=video_schedule_late.take_window();
            DurationSample pending_wait=video_pending_time.take_window(),video_feed=video_feed_time.take_window();
            DurationSample audio_feed=audio_feed_time.take_window(),tick=media_tick_time.take_window(),loop=loop_gap_time.take_window();
            unsigned frame_events=media.frames.load();
            unsigned event_delta=frame_events-last_decoder_events;
            double incoming_fps=timing.frames/window_s;
            double pts_fps=timing.interval_total_ns?timing.intervals*1e9/timing.interval_total_ns:0;
            live_pts_fps=pts_fps;
            live_decoder_callbacks_fps=event_delta/window_s;
            live_queue_ms=q.video_queue_residence.average_ms();live_feed_ms=video_feed.average_ms();
            double avg_frame_ms=timing.intervals?timing.interval_total_ns/double(timing.intervals)/1e6:0;
            if(opened) fprintf(stderr,"[video] advertised_max_fps=%d incoming_fps=%.2f pts_fps=%.2f frame_ms_avg=%.2f min=%.2f max=%.2f feed_accepted_fps=%.2f decoder_callbacks_s=%.2f (not displayed FPS) invalid=%u queue_dropped_frames=%llu\n",
                settings.max_fps,incoming_fps,pts_fps,avg_frame_ms,timing.interval_min_ns/1e6,timing.interval_max_ns/1e6,
                (media.feed_video_accepted.load()-last_feed_video)/window_s,live_decoder_callbacks_fps,invalid_video,(unsigned long long)q.dropped_video_frames);
            if(opened) fprintf(stderr,"[pipeline] queue=%zu/512 packets bytes=%zu/%zu (%.1f%%) peak=%zu packets/%zu bytes dropped_packets=%llu retries_video=%llu rejected_video=%llu retries_audio=%llu feed_audio_fps=%.2f feed_avg_us=%.1f feed_max_us=%.1f\n",
                q.queue_packets,q.queue_bytes,receiver.max_queue_bytes,q.queue_bytes*100.0/receiver.max_queue_bytes,q.peak_queue_packets,q.peak_queue_bytes,
                (unsigned long long)q.dropped_packets,
                (unsigned long long)(media.feed_video_retries.load()-last_feed_video_retries),
                (unsigned long long)media.feed_video_rejected.load(),
                (unsigned long long)(media.feed_audio_retries.load()-last_feed_audio_retries),
                (media.feed_audio_accepted.load()-last_feed_audio)/window_s,
                media.feed_calls.load()?media.feed_time_ns.load()/double(media.feed_calls.load())/1000:0.0,media.feed_max_time_ns.load()/1000.0);
            if(opened) fprintf(stderr,"[pacing] arrival_gap_max_ms=%.2f pts_gap_max_ms=%.2f feed_gap_max_ms=%.2f arrival_gaps_gt100=%llu pts_gaps_gt100=%llu both=%llu arrival_only=%llu pts_only=%llu pts_discontinuities=%llu video_idle_ms=%.0f mirror=%d paused=%d callbacks_idle_ms=%.0f\n",
                timing.arrival_interval_max_ns/1e6,timing.interval_max_ns/1e6,fed.arrival_interval_max_ns/1e6,
                (unsigned long long)timing.arrival_gaps_over_100ms,(unsigned long long)timing.pts_gaps_over_100ms,
                (unsigned long long)timing.gaps_in_both,(unsigned long long)timing.arrival_only_gaps,(unsigned long long)timing.pts_only_gaps,
                (unsigned long long)timing.pts_discontinuities,
                receiver.last_video_arrival.load()?std::max<int64_t>(0,now-receiver.last_video_arrival.load())/1e6:-1.0,
                int(receiver.mirror.load()),int(receiver.paused.load()),
                media.last_frame_event_ns.load()?std::max<int64_t>(0,now-media.last_frame_event_ns.load())/1e6:-1.0);
            if(opened) fprintf(stderr,"[queue-video] current=%zu packets/%zu bytes peak=%zu packets/%zu bytes ingress_to_queue_ms=%.2f/%.2f queue_residence_ms=%.2f/%.2f pending_to_feed_ms=%.2f/%.2f schedule_late_ms=%.2f/%.2f parse_ms=%.2f/%.2f copy_ms=%.2f lock_wait_ms=%.2f pop_ms=%.2f\n",
                q.video_queue_packets,q.video_queue_bytes,q.peak_video_queue_packets,q.peak_video_queue_bytes,
                q.ingress_to_queue.average_ms(),q.ingress_to_queue.maximum_ms(),
                q.video_queue_residence.average_ms(),q.video_queue_residence.maximum_ms(),
                pending_wait.average_ms(),pending_wait.maximum_ms(),late.average_ms(),late.maximum_ms(),
                parsed.average_ms(),parsed.maximum_ms(),
                q.ingress_copy.maximum_ms(),q.queue_lock_wait.maximum_ms(),q.queue_pop.maximum_ms());
            if(opened) fprintf(stderr,"[stage] loop_ms=%.2f tick_ms=%.2f video_feed_ms=%.2f audio_feed_ms=%.2f loop_gt50=%llu video_feed_gt50=%llu audio_feed_gt50=%llu audio_queue_residence_ms=%.2f/%.2f\n",
                loop.maximum_ms(),tick.maximum_ms(),video_feed.maximum_ms(),audio_feed.maximum_ms(),
                (unsigned long long)loop.over_50ms,(unsigned long long)video_feed.over_50ms,(unsigned long long)audio_feed.over_50ms,
                q.audio_queue_residence.average_ms(),q.audio_queue_residence.maximum_ms());
            if(opened) fprintf(stderr,"[audio] sent=%u decode_errors=%u decode_avg_us=%.1f decode_max_us=%.1f late=%u silence=%u\n",
                audio_count,decode_errors,audio_decode_calls?audio_decode_ns/double(audio_decode_calls)/1000:0.0,audio_decode_max_ns/1000.0,late_audio,silence_blocks);
            fprintf(stderr,"[health] uptime_s=%.0f session_age_s=%.1f sessions_started=%u sessions_ended=%u reset_events=%u rss_kb=%ld\n",
                (now-app_started)/1e9,session_started?(now-session_started)/1e9:0.0,sessions_started,sessions_ended,receiver.session_resets.load(),process_rss_kb());
            last_decoder_events=frame_events;
            last_feed_video=media.feed_video_accepted.load();last_feed_audio=media.feed_audio_accepted.load();
            last_feed_video_retries=media.feed_video_retries.load();last_feed_audio_retries=media.feed_audio_retries.load();
            audio_decode_calls=audio_decode_ns=audio_decode_max_ns=0;
            rotate_log();last_stats=now;
        }
        now=now_ns();
        if(!have_pending) {
            // Leave future packets in the shared queue so late-arriving video can
            // precede deliberately delayed audio. Avoid head-of-line blocking.
            uint64_t latest=opened?origin+uint64_t(std::max<int64_t>(0,now-start)):UINT64_MAX;
            have_pending=receiver.pop(pending,latest);
        }
        if(!have_pending){SDL_Delay(3);continue;}
        if(!current_audio_epoch(pending.transport_epoch,receiver.transport_epoch.load())) {
            fprintf(stderr,"[session] obsolete-packet video=%d packet_epoch=%u current_epoch=%u\n",
                int(pending.video),pending.transport_epoch,receiver.transport_epoch.load());
            have_pending=false;stall=0;continue;
        }
        if(!opened && !pending.video) {
            const AirPlayAudioIntent intent{receiver.last_audio_codec.load(),receiver.audio_samples_per_frame.load(),
                receiver.audio_using_screen.load(),receiver.audio_is_media.load(),receiver.has_audio.load()};
            if(audio_only_failed || !standalone_audio_candidate(intent,receiver.mirror.load())) {have_pending=false;continue;}
            int64_t open_begin=now_ns();
            bool media_opened=media.open(app_id,0,0,true,44100,1048576,0,0,MirrorCodec::H264,30,true);
            fprintf(stderr,"[audio-only] Starfish open_ms=%.2f success=%d codec=%u using_screen=%d is_media=%d\n",
                (now_ns()-open_begin)/1e6,int(media_opened),intent.codec,int(intent.using_screen),int(intent.is_media));
            if(!media_opened) {
                have_pending=false;audio_only_failed=true;
                fprintf(stderr,"[audio-only] receiver negotiated audio but Starfish load failed; mirroring remains available\n");
                receiver.show(std::string(media_open_failure_message(media.open_failure()))+" Audio-only playback stopped; mirroring remains available.");
                continue;
            }
            opened=true;audio_only_playback=true;session_codec=MirrorCodec::H264;
            origin=pending.timestamp;session_started=start=now_ns();stall=0;++sessions_started;
            session_started_utc=uint64_t(std::time(nullptr));session_success=false;
            ingress_at_start=receiver.total_video_arrivals.load();
            ReceiverDiagnostics initial_queue=receiver.queue_snapshot();
            drops_at_start=initial_queue.dropped_packets;video_drops_at_start=initial_queue.dropped_video_frames;
            video_count=audio_count=decode_errors=late_audio=silence_blocks=invalid_video=0;
            audio_end=0;audio_started=false;audio_decode_calls=audio_decode_ns=audio_decode_max_ns=0;
            last_decoder_events=0;last_feed_video=last_feed_audio=0;last_feed_video_retries=last_feed_audio_retries=0;
            ui_visible=false;shell.transparent();np_drawn=0;np_bright=now_ns();
            fprintf(stderr,"[audio-only] session started codec=%u sample_rate=44100 output=PCM-stereo timestamps=remote-ntp\n",intent.codec);
        }
        if(!opened && pending.video) {
            // Start on a random-access video frame. Pre-roll audio is intentionally
            // discarded, then both streams share this frame's remote NTP origin.
            if(!pending.video) {have_pending=false;continue;}
            int64_t parse_begin=now_ns();
            const bool incoming_hevc=pending.codec==2;
            bool valid=false,keyframe=false;
            int input_width=0,input_height=0;
            HevcFrameInfo hevc_info;
            if(incoming_hevc) {
                hevc_info=hevc.inspect(pending.data.data(),pending.data.size());
                valid=hevc_info.valid;keyframe=hevc_info.keyframe;
                input_width=hevc_info.width;input_height=hevc_info.height;
            } else {
                auto info=inspect_h264(pending.data.data(),pending.data.size());
                valid=info.valid;keyframe=info.keyframe;
                input_width=info.width;input_height=info.height;
                if(!info.configuration.empty())configuration=info.configuration;
            }
            video_parse_time.record(uint64_t(std::max<int64_t>(0,now_ns()-parse_begin)));
            if(incoming_hevc && hevc_info.waiting_configuration) {
                if(!hevc_configuration_wait)hevc_configuration_wait=now;
                if(!hevc_invalid_logged)fprintf(stderr,"[codec-start] waiting-configuration epoch=%u bytes=%zu present=%d/%d/%d complete=%d keyframe=%d\n",
                    pending.transport_epoch,pending.data.size(),int(hevc_info.has_vps),int(hevc_info.has_sps),int(hevc_info.has_pps),
                    int(hevc_info.configuration_complete),int(hevc_info.keyframe));
                hevc_invalid_logged=true;have_pending=false;
                if(hevc_configuration_wait_expired(hevc_configuration_wait,now)) {
                    fprintf(stderr,"[codec-start] incomplete-configuration timeout; reconnect required; runtime profile retained\n");
                    finish(true);receiver.show("HEVC headers missing. Reconnect sender; selected profile retained.");
                }
                continue;
            }
            if(!valid) {
                if(incoming_hevc && !hevc_invalid_logged) {
                    fprintf(stderr,"[codec-start] waiting-for-valid-parameter-sets\n");
                    hevc_invalid_logged=true;
                }
                ++invalid_video;have_pending=false;
                if(settings.codec==MirrorCodec::ExperimentalHevc && invalid_video>=30) {
                    fprintf(stderr,"[codec] rejected 30 access units before startup codec=%s\n",incoming_hevc?"HEVC":"H264");
                    finish(true);recover_decoder(true,incoming_hevc?"invalid-parameter-sets":"unsupported-h264-stream");
                }
                continue;
            }
            hevc_configuration_wait=0;
            if(incoming_hevc && !supported_hevc_mirror_profile(hevc_info)) {
                fprintf(stderr,"[codec] unsupported HEVC profile=%d bit-depth=%d; Main 10/HDR10 path disabled\n",
                    hevc_info.profile,hevc_info.bit_depth);
                finish(true);recover_decoder(true,"unsupported-hevc-profile");continue;
            }
            if(input_width) {
                if(!input_fits_mirror_profile(settings,input_width,input_height)) {
                    fprintf(stderr,"[video] sender resolution %dx%d exceeds selected profile %dx%d\n",input_width,input_height,settings.width,settings.height);
                    finish(true);receiver.show("Sender resolution exceeds selected profile");continue;
                }
                width=observed_width=input_width;height=observed_height=input_height;
            }
            if(!keyframe||(!incoming_hevc&&configuration.empty())||!observed_width) {
                if(incoming_hevc && !hevc_keyframe_wait_logged) {
                    fprintf(stderr,"[codec-start] waiting-for-valid-keyframe\n");
                    hevc_keyframe_wait_logged=true;
                }
                have_pending=false;continue;
            }
            if(incoming_hevc) {
                bool added=false;
                if(!hevc.prepend_missing_parameter_sets(pending.data,hevc_info,added)) {
                    finish(true);recover_decoder(true,"hevc-parameter-sets-missing");continue;
                }
                if(added)fprintf(stderr,"[codec-start] cached-headers prepended before initial Load epoch=%u\n",pending.transport_epoch);
            }
            session_codec=incoming_hevc?MirrorCodec::ExperimentalHevc:MirrorCodec::H264;
            int64_t open_begin=now_ns();
            bool media_opened=media.open(app_id,width,height,true,44100,mirror_limits(settings).video_source_buffer_bytes,
                std::min(settings.width,1920),std::min(settings.height,1080),session_codec,settings.max_fps);
            fprintf(stderr,"[media-init] open_ms=%.2f success=%d\n",(now_ns()-open_begin)/1e6,int(media_opened));
            if(!media_opened) {
                bool was_hevc=session_codec==MirrorCodec::ExperimentalHevc;
                report_media_start_failure(was_hevc,"starfish-load-failed");continue;
            }
            if(incoming_hevc) {
                hevc_startup.accept_load(now_ns());hevc_preroll_wait_logged=hevc_first_feed_logged=false;
                fprintf(stderr,"[codec-start] load-accepted=1 first-video=validated-keyframe preroll-delay-ms=200 limit=%u-frames/%zu-bytes\n",
                    HevcStartupGate::kMaxPrerollFrames,HevcStartupGate::kMaxPrerollBytes);
            }
            // The pre-roll starts after synchronous decoder initialization.
            // Reusing the loop's old `now` would silently consume it in Load().
            opened=true;origin=pending.timestamp;session_started=now_ns();
            start=schedule_start(pending.arrival_ns,session_started,200000000LL);stall=0;++sessions_started;
            if(incoming_hevc)hevc_transport_failures=0;
            session_started_utc=uint64_t(std::time(nullptr));session_success=false;
            ingress_at_start=receiver.total_video_arrivals.load();if(ingress_at_start)--ingress_at_start;
            ReceiverDiagnostics initial_queue=receiver.queue_snapshot();
            drops_at_start=initial_queue.dropped_packets;video_drops_at_start=initial_queue.dropped_video_frames;
            live_feed_last=0;
            video_count=audio_count=decode_errors=late_audio=silence_blocks=invalid_video=0;
            audio_decode_calls=audio_decode_ns=audio_decode_max_ns=0;
            last_decoder_events=0;last_feed_video=last_feed_audio=0;last_feed_video_retries=last_feed_audio_retries=0;
            ui_visible=false;shell.transparent();
            fprintf(stderr,"[session] codec=%s input=%dx%d advertised=%dx%d advertised_max_fps=%d audio=%d profile=%s bitdepth=%d\n",
                    incoming_hevc?"HEVC":"H264",width,height,settings.width,settings.height,settings.max_fps,int(receiver.has_audio),
                    incoming_hevc?"Main":"H264",incoming_hevc?8:0);
        }
        if(standalone_audio_yield_to_video(audio_only_playback,pending.video)) {
            fprintf(stderr,"[audio-only] video stream arrived; closing audio-only Starfish session and starting mirroring\n");
            finish(false,true,true);
            continue;
        }
        int64_t pts=pending.timestamp>=origin ? int64_t(pending.timestamp-origin) : -int64_t(origin-pending.timestamp);
        if(pts<0){have_pending=false;continue;}
        // Large clock discontinuities require a fresh AirPlay session, not an unbounded queue.
        if(pts>now-start+5000000000LL) {fprintf(stderr,"[session] timestamp discontinuity\n");finish(true);receiver.show("Device clock changed. Connect again.");continue;}
        // NDL: a schedule that fell behind (anchored to a keyframe held over a
        // reinit) catches up; audio shares the same start, so it shifts with
        // the picture.
        if(kMaxHoldNs && pending.video && pending.arrival_ns && start+pts>pending.arrival_ns+kMaxHoldNs)
            start=pending.arrival_ns+kMaxHoldNs-pts;
        if(now<start+pts) {
            if(session_codec==MirrorCodec::ExperimentalHevc && video_count==0 && !hevc_schedule_wait_logged) {
                fprintf(stderr,"[codec-start] feed-wait reason=pts-schedule remaining_ms=%.0f\n",(start+pts-now)/1e6);
                hevc_schedule_wait_logged=true;
            }
            SDL_Delay(2);continue;
        }
        if(pending.video && ((pending.codec==2 && session_codec!=MirrorCodec::ExperimentalHevc) ||
                             (pending.codec==0 && session_codec!=MirrorCodec::H264))) {
            fprintf(stderr,"[codec] mid-session codec change requires reconnect\n");
            bool was_hevc=session_codec==MirrorCodec::ExperimentalHevc;
            finish(true);
            if(was_hevc)recover_decoder(true,"codec-switch");
            else receiver.show("Codec changed. Reconnect sender.");
            continue;
        }
        if(pending.video && pending.codec==0) {
            int64_t parse_begin=now_ns();
            auto info=inspect_h264(pending.data.data(),pending.data.size());
            video_parse_time.record(uint64_t(std::max<int64_t>(0,now_ns()-parse_begin)));
            if(!info.valid){++invalid_video;have_pending=false;continue;}
            if(info.width) {
                if(!input_fits_mirror_profile(settings,info.width,info.height)) {
                    fprintf(stderr,"[video] sender resolution %dx%d exceeds selected profile %dx%d\n",info.width,info.height,settings.width,settings.height);
                    finish(true);receiver.show("Sender resolution exceeds selected profile");continue;
                }
                observed_width=info.width;observed_height=info.height;
            }
            if(!info.configuration.empty())configuration=info.configuration;
            if(info.width && (width!=info.width||height!=info.height)) {
                media.close();width=info.width;height=info.height;
                int64_t open_begin=now_ns();
                bool media_opened=info.keyframe && media.open(app_id,width,height,true,44100,mirror_limits(settings).video_source_buffer_bytes,
                    std::min(settings.width,1920),std::min(settings.height,1080),MirrorCodec::H264,settings.max_fps);
                fprintf(stderr,"[media-init] reopen_ms=%.2f success=%d\n",(now_ns()-open_begin)/1e6,int(media_opened));
                if(!media_opened) {report_media_start_failure(false,"h264-reopen-failed");continue;}
                origin=pending.timestamp;start=schedule_start(pending.arrival_ns,now_ns(),0);pts=0;audio_end=0;audio_started=false;decoder.reset();
                video_feed_timing.reset();
                live_feed_last=0;
                last_decoder_events=0;last_feed_video=last_feed_audio=0;last_feed_video_retries=last_feed_audio_retries=0;
                fprintf(stderr,"[session] geometry=%dx%d\n",width,height);
            }
            pending.codec=1; // Already parsed: do not repeat on decoder back-pressure.
        }
        if(pending.video && pending.codec==2) {
            bool reopened=false;
            int64_t parse_begin=now_ns();
            HevcFrameInfo info=hevc.inspect(pending.data.data(),pending.data.size());
            video_parse_time.record(uint64_t(std::max<int64_t>(0,now_ns()-parse_begin)));
            if(info.waiting_configuration){have_pending=false;continue;}
            if(!info.valid) {
                have_pending=false;
                if(++invalid_video>=30) {finish(true);recover_decoder(true,"invalid-access-units");}
                continue;
            }
            if(!supported_hevc_mirror_profile(info)) {
                fprintf(stderr,"[codec] unsupported HEVC profile=%d bit-depth=%d; Main 10/HDR10 path disabled\n",
                    info.profile,info.bit_depth);
                finish(true);recover_decoder(true,"unsupported-hevc-profile");continue;
            }
            if(!input_fits_mirror_profile(settings,info.width,info.height)) {
                fprintf(stderr,"[codec] HEVC input %dx%d exceeds advertised profile\n",info.width,info.height);
                finish(true);recover_decoder(true,"unsupported-dimensions");continue;
            }
            observed_width=info.width;observed_height=info.height;
            bool changed=info.configuration_changed || info.width!=width || info.height!=height;
            if(changed)hevc_wait_keyframe=true;
            if(hevc_wait_keyframe) {
                if(!info.keyframe) {have_pending=false;continue;}
                bool parameter_sets_added=false;
                if(!hevc.prepend_missing_parameter_sets(pending.data,info,parameter_sets_added)) {
                    finish(true);recover_decoder(true,"hevc-parameter-sets-missing");continue;
                }
                if(parameter_sets_added)
                    fprintf(stderr,"[codec] restored-cached-parameter-sets bytes=%zu before-random-access-frame\n",pending.data.size());
                media.close();width=info.width;height=info.height;
                int64_t open_begin=now_ns();
                bool media_opened=media.open(app_id,width,height,true,44100,
                    mirror_limits(settings).video_source_buffer_bytes,
                    std::min(settings.width,1920),std::min(settings.height,1080),
                    MirrorCodec::ExperimentalHevc,settings.max_fps);
                fprintf(stderr,"[codec] reinitialize=%u input=%dx%d load_ms=%.2f success=%d\n",
                    ++decoder_reinitializations,width,height,(now_ns()-open_begin)/1e6,int(media_opened));
                if(!media_opened) {report_media_start_failure(true,"hevc-reopen-failed");continue;}
                hevc_startup.accept_load(now_ns());hevc_preroll_wait_logged=hevc_first_feed_logged=false;
                hevc_schedule_wait_logged=false;stall=0;last_paused=false;
                fprintf(stderr,"[codec-start] load-accepted=1 reason=parameter-change input=%dx%d\n",width,height);
                hevc_wait_keyframe=false;
                origin=pending.timestamp;start=schedule_start(pending.arrival_ns,now_ns(),200000000LL);pts=0;audio_end=0;audio_started=false;decoder.reset();
                video_feed_timing.reset();live_feed_last=0;
                last_decoder_events=0;last_feed_video=last_feed_audio=0;
                last_feed_video_retries=last_feed_audio_retries=0;
                reopened=true;
            }
            if(!ensure_hevc_aud(pending.data,info.has_aud)) {
                finish(true);recover_decoder(true,"access-unit-bound");continue;
            }
            pending.codec=3; // Parsed; retain AU unchanged across Feed retries.
            if(reopened) {
                fprintf(stderr,"[codec-start] reinit-keyframe retained=1 bytes=%zu pts_ns=0\n",pending.data.size());
                continue; // The next loop honors the new schedule (Starfish: its 200 ms PTS pre-roll).
            }
        }
        HevcFeedStage hevc_feed_stage=HevcFeedStage::Loaded;
        if(pending.video && session_codec==MirrorCodec::ExperimentalHevc) {
            hevc_feed_stage=hevc_startup.video_stage(media.ready.load(),media.failed.load(),pending.data.size());
            if(hevc_feed_stage==HevcFeedStage::WaitingForLoad) {
                if(!hevc_preroll_wait_logged) {
                    fprintf(stderr,"[codec-start] feed-wait reason=preroll-limit accepted=%u bytes=%zu ready=0\n",
                        hevc_startup.preroll_frames(),hevc_startup.preroll_bytes());
                    hevc_preroll_wait_logged=true;
                }
                SDL_Delay(5);continue;
            }
            if(hevc_feed_stage==HevcFeedStage::NoLoad || hevc_feed_stage==HevcFeedStage::Failed) {
                fprintf(stderr,"[codec-start] feed-blocked reason=%s\n",
                    hevc_feed_stage==HevcFeedStage::NoLoad?"load-not-accepted":"decoder-error");
                finish(true);recover_decoder(true,"hevc-feed-state");continue;
            }
        }
        if(!pending.video && pending.codec!=0) {
            std::vector<uint8_t> pcm;
            int64_t decode_start=now_ns();
            if(!decoder.decode(pending.codec,pending.data.data(),pending.data.size(),pcm)) {
                uint64_t elapsed=uint64_t(std::max<int64_t>(0,now_ns()-decode_start));++audio_decode_calls;audio_decode_ns+=elapsed;audio_decode_max_ns=std::max(audio_decode_max_ns,elapsed);
                if(++decode_errors<=5)fprintf(stderr,"[audio] decode failure ct=%u\n",pending.codec);
                have_pending=false;continue;
            }
            uint64_t decode_elapsed=uint64_t(std::max<int64_t>(0,now_ns()-decode_start));++audio_decode_calls;audio_decode_ns+=decode_elapsed;audio_decode_max_ns=std::max(audio_decode_max_ns,decode_elapsed);
            pending.data.swap(pcm);pending.codec=0;
            float volume=receiver.volume;
            if(volume<1) {
                for(size_t i=0;i+1<pending.data.size();i+=2) {
                    int16_t sample=int16_t(unsigned(pending.data[i])|(unsigned(pending.data[i+1])<<8));
                    int16_t out=int16_t(sample*volume);pending.data[i]=out&255;pending.data[i+1]=(uint16_t(out)>>8)&255;
                }
            }
            if(pending.data.empty()){have_pending=false;continue;}
        }
        if(!pending.video && receiver.audio_flush.exchange(false)) {
            flush_audio();continue;
        }
        if(!pending.video && !current_audio_epoch(pending.audio_epoch,receiver.audio_epoch.load())) {
            have_pending=false;stall=0;continue;
        }
        if(audio_only_playback && !pending.video && audio_flush_paused) {
            audio_flush_paused=false;media.pause(false);last_paused=false;
            fprintf(stderr,"[audio-only] resumed Starfish on new audio packet\n");
        }
        if(!pending.video) audio_started=true;
        if(!pending.video && pts<audio_end) {
            size_t trim=size_t((audio_end-pts)*44100/1000000000)*4;
            if(trim>=pending.data.size()){++late_audio;have_pending=false;continue;}
            pending.data.erase(pending.data.begin(),pending.data.begin()+trim);
            pending.timestamp=origin+audio_end;pts=audio_end;
        }
        int64_t feed_begin=now_ns();
        auto result=media.feed(pending.data.data(),pending.data.size(),pts,pending.video);
        int64_t feed_end=now_ns();
        (pending.video?video_feed_time:audio_feed_time).record(uint64_t(std::max<int64_t>(0,feed_end-feed_begin)));
        if(result==FeedResult::Accepted) {
            if(pending.video) {
                if(session_codec==MirrorCodec::ExperimentalHevc) {
                    hevc_startup.accepted_video(pending.data.size(),hevc_feed_stage);
                    if(!hevc_first_feed_logged) {
                        fprintf(stderr,"[codec-start] first-video-feed accepted=1 stage=%s ready=%d playing=%d bytes=%zu elapsed_ms=%.0f\n",
                            hevc_feed_stage==HevcFeedStage::Preroll?"preroll":"loaded",int(media.ready.load()),
                            int(media.playing.load()),pending.data.size(),hevc_startup.age_ns(feed_end)/1e6);
                        hevc_first_feed_logged=true;
                    }
                }
                ++video_count;
                video_feed_timing.record(pending.timestamp,feed_end);
                video_schedule_late.record(uint64_t(std::max<int64_t>(0,feed_begin-(start+pts))));
                video_pending_time.record(uint64_t(std::max<int64_t>(0,feed_begin-pending.dequeued_ns)));
            } else {++audio_count;audio_end=pts+int64_t(pending.data.size()/4)*1000000000/44100;}
            if(stall)fprintf(stderr,"[feed-stall] generation=%u recovered duration_ms=%.1f\n",sessions_started,(now-stall)/1e6);
            have_pending=false;stall=0;
        } else if(result==FeedResult::Retry) {
            if(!stall) {
                stall=now;
                ReceiverDiagnostics q=receiver.queue_snapshot();
                fprintf(stderr,"[feed-stall] generation=%u begin video=%d pts_ms=%.1f bytes=%zu queue=%zu/%zu ready=%d playing=%d flush_paused=%d\n",
                        sessions_started,int(pending.video),pts/1e6,pending.data.size(),q.queue_packets,q.queue_bytes,
                        int(media.ready.load()),int(media.playing.load()),int(audio_flush_paused));
            }
            if(feed_retry_expired(stall,now)){
                ReceiverDiagnostics q=receiver.queue_snapshot();
                fprintf(stderr,"[feed-stall] generation=%u timeout duration_ms=%.1f video=%d queue=%zu/%zu retries_audio=%llu ready=%d playing=%d\n",
                        sessions_started,(now-stall)/1e6,int(pending.video),q.queue_packets,q.queue_bytes,
                        (unsigned long long)media.feed_audio_retries.load(),int(media.ready.load()),int(media.playing.load()));
                recover_playback("feed-backpressure-timeout");
            }
            SDL_Delay(5);
        } else {
            recover_playback("feed-rejected");
        }
    }
    bus.block_screen_saver(false);
    if(opened)finish(false,true);
    receiver.stop();receiver.disconnect();media.close();
    fprintf(stderr,"[app] stopped cleanly\n");
    return 0;
}
