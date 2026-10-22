#include "direct_player.h"
#include <StarfishMediaAPIs.h>
#include <AcbAPI.h>
#include <cstdio>
#include <map>
#include <cstring>
#include <cstdlib>
#include <chrono>
extern "C" int direct_acb_video_info(long,const char*,long*) __asm__("AcbAPI_setMediaVideoData");
static std::mutex context_mutex;static std::map<uintptr_t,DirectPlayer*> contexts;static std::atomic<uintptr_t> next_context{1};
static int64_t clock_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static void direct_acb(long,long task,long event,long app,long play,const char*){
    static std::atomic<unsigned> records{0};
    if(records.load()<64&&records.fetch_add(1)<64)fprintf(stderr,"[direct-player] acb-event=%ld task=%ld app=%ld play=%ld\n",event,task,app,play);
}
void DirectPlayer::begin(DirectStage s){stage_=int(s);if(phase_records_<128)fprintf(stderr,"[direct-player] stage=%s begin\n",direct_stage_name(s));}
void DirectPlayer::result(DirectStage s,int64_t value,int64_t began){
    if(error_==int(DirectError::None))native_result_=value;
    if(phase_records_++<128)fprintf(stderr,"[direct-player] stage=%s end result=%lld ms=%lld\n",direct_stage_name(s),(long long)value,(long long)(clock_ms()-began));
}
void DirectPlayer::fail(DirectError e,int64_t value){
    error_=int(e);native_result_=value;state_=int(DirectState::Error);
    const auto s=snapshot();
    fprintf(stderr,"[direct-player] failure=%s stage=%s result=%lld callbacks=%llu last_event=%d raw_value=%lld native_requests=%llu upstream_fetches=%llu bytes=%llu failures=%llu\n",
        direct_error_name(e),direct_stage_name(s.stage),(long long)value,(unsigned long long)s.callbacks,s.last_callback,(long long)s.last_callback_value,
        (unsigned long long)s.relay_requests,(unsigned long long)s.upstream_fetches,(unsigned long long)s.downloaded_bytes,(unsigned long long)s.relay_failures);
}
void DirectPlayer::event(int type,int64_t value,const char* message,void *user){
    std::lock_guard<std::mutex> context_lock(context_mutex);
    auto entry=contexts.find(reinterpret_cast<uintptr_t>(user));if(entry==contexts.end())return;
    auto& p=*entry->second;++p.callbacks_;p.last_callback_=type;p.last_callback_value_=value;
    // Firmware text may contain signed URLs. Count every callback, but print only
    // the first of each event type (including previously ignored source/resource events).
    const bool first=type>=0&&type<64&&!(p.event_types_&(uint64_t(1)<<type));
    if(type>=0&&type<64)p.event_types_|=uint64_t(1)<<type;
    if(first&&p.event_records_++<32)fprintf(stderr,"[direct-player] event=0x%x raw_value=%lld\n",type,(long long)value);
    if(p.state_==int(DirectState::Error))return;
    switch(type){
    case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED:p.loaded_=true;break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING:p.stage_=int(DirectStage::Playing);p.state_=int(DirectState::Playing);break;
    case 0x1b:p.stage_=int(DirectStage::Paused);p.state_=int(DirectState::Paused);break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM:p.state_=int(DirectState::Ended);break;
    case 4:
        if(message&&strnlen(message,8193)<=8192){std::lock_guard<std::mutex> lock(p.info_mutex_);p.video_info_=message;p.info_pending_=true;
            auto value=[&](const char *field){auto at=p.video_info_.find(field);if(at==std::string::npos)return 0U;at=p.video_info_.find(':',at);if(at==std::string::npos)return 0U;long n=strtol(p.video_info_.c_str()+at+1,nullptr,10);return n>0&&n<=4096?unsigned(n):0U;};
            p.width_=value("\"width\"");p.height_=value("\"height\"");}
        break;
    case PF_EVENT_TYPE_INT_DURATION:p.duration_ms_=value>0?value:0;break;
    case PF_EVENT_TYPE_INT_ERROR:case PF_EVENT_TYPE_STR_ERROR:p.fail(DirectError::NativeError,value);break;
    default:break;}
}
DirectPlayer::~DirectPlayer(){close();}
bool DirectPlayer::open(const std::string& uri,double position,int64_t now,DirectSession *session,bool seconds){
    close();state_=int(DirectState::Loading);session_=session;
    relay_.sender_manifest=session?[session](const std::string& url){return session->sender_manifest(url);}:std::function<std::string(const std::string&)>();
    begin(DirectStage::Relay);auto began=clock_ms();const auto local=relay_.start(uri);result(DirectStage::Relay,!local.empty(),began);
    if(local.empty()){fail(DirectError::RelayStart);cleanup();return false;}
    const auto payload=direct_load_payload(local,"com.github.depius.uxair.dev",true);
    if(payload.empty()){fail(DirectError::Payload);cleanup();return false;}
    start_position_=position;start_seconds_=seconds;opened_ns_=last_progress_ns_=now;last_position_ms_=-1;
    begin(DirectStage::AcbCreate);began=clock_ms();acb_=AcbAPI_create();result(DirectStage::AcbCreate,acb_,began);
    if(!acb_){fail(DirectError::AcbCreate);cleanup();return false;}
    // Native URI playback is VIDEO; MSE is the buffer-stream mirroring role.
    begin(DirectStage::AcbInitialize);began=clock_ms();const bool initialized=AcbAPI_initialize(acb_,PLAYER_TYPE_VIDEO,"com.github.depius.uxair.dev",direct_acb);result(DirectStage::AcbInitialize,initialized,began);
    if(!initialized){fail(DirectError::AcbInitialize);cleanup();return false;}
    begin(DirectStage::Construct);began=clock_ms();
    try{api_=new StarfishMediaAPIs(nullptr);}catch(...){fail(DirectError::Construct);cleanup();return false;}
    result(DirectStage::Construct,api_!=nullptr,began);
    begin(DirectStage::MediaId);began=clock_ms();const bool registered=AcbAPI_setMediaId(acb_,api_->getMediaID());result(DirectStage::MediaId,registered,began);
    if(!registered){fail(DirectError::MediaId);cleanup();return false;}
    context_id_=next_context++;if(!context_id_){fail(DirectError::Construct);cleanup();return false;}
    {std::lock_guard<std::mutex> lock(context_mutex);contexts[context_id_]=this;}
    begin(DirectStage::Load);began=clock_ms();const bool accepted=api_->Load(payload.c_str(),event,reinterpret_cast<void*>(context_id_));result(DirectStage::Load,accepted,began);
    if(!accepted&&state_!=int(DirectState::Error))fail(DirectError::LoadRejected);
    if(state_==int(DirectState::Error)){cleanup();return false;}
    // Match the working native initialization order: bind the window after Load.
    begin(DirectStage::Window);began=clock_ms();auto window=AcbAPI_setDisplayWindow(acb_,0,0,1920,1080,true,&task_);result(DirectStage::Window,window,began);
    if(state_==int(DirectState::Loading))stage_=int(DirectStage::AwaitLoad);
    return true;
}
bool DirectPlayer::control(const DirectCommand& c){
    if(!api_)return false;bool accepted=false;DirectStage stage=DirectStage::Play;
    if(c.kind==DirectCommandKind::Stop){close();return true;}
    if(c.kind==DirectCommandKind::Seek)stage=DirectStage::Seek;
    else if(c.kind==DirectCommandKind::Pause)stage=DirectStage::Paused;
    else if(c.kind!=DirectCommandKind::Resume)return false;
    begin(stage);const auto began=clock_ms();
    if(c.kind==DirectCommandKind::Pause)accepted=api_->Pause();
    else if(c.kind==DirectCommandKind::Resume)accepted=api_->Play();
    else {char value[48];snprintf(value,sizeof(value),"%.0f",c.position*1000);accepted=api_->Seek(value);}
    result(stage,accepted,began);
    if(!accepted&&state_!=int(DirectState::Error))fail(c.kind==DirectCommandKind::Seek?DirectError::Seek:DirectError::ControlRejected);
    return accepted&&state_!=int(DirectState::Error);
}
DirectSnapshot DirectPlayer::snapshot() const {
    DirectSnapshot s;s.state=DirectState(state_.load());s.stage=DirectStage(stage_.load());s.error=DirectError(error_.load());s.native_result=native_result_;
    s.duration=duration_ms_.load()/1000.0;s.width=width_;s.height=height_;s.callbacks=callbacks_;s.last_callback=last_callback_;s.last_callback_value=last_callback_value_;
    s.relay_requests=relay_.incoming_requests;s.upstream_fetches=relay_.requests;s.downloaded_bytes=relay_.bytes;s.relay_failures=relay_.failures;return s;
}
DirectSnapshot DirectPlayer::tick(int64_t now){
    if(state_==int(DirectState::Error))return snapshot();
    std::string info;
    {std::lock_guard<std::mutex> lock(info_mutex_);if(info_pending_){info=video_info_;info_pending_=false;}}
    if(api_&&!info.empty()){
        begin(DirectStage::VideoInfo);auto began=clock_ms();const auto value=direct_acb_video_info(acb_,info.c_str(),&task_);result(DirectStage::VideoInfo,value,began);
        const int width=int(width_.load()),height=int(height_.load());
        if(width>0&&height>0){int w=1920,h=1080;if(1920*height>1080*width)w=1080*width/height;else h=1920*height/width;
            begin(DirectStage::Window);began=clock_ms();const auto window=AcbAPI_setDisplayWindow(acb_,(1920-w)/2,(1080-h)/2,w,h,true,&task_);result(DirectStage::Window,window,began);}
    }
    if(api_&&loaded_&&!started_){
        if(!sink_attached_){
            begin(DirectStage::Sink);auto began=clock_ms();const bool sink=AcbAPI_setSinkType(acb_,SINK_TYPE_MAIN);result(DirectStage::Sink,sink,began);
            if(!sink){fail(DirectError::Sink);return snapshot();}
            begin(DirectStage::LoadedState);began=clock_ms();const auto value=AcbAPI_setState(acb_,APPSTATE_FOREGROUND,PLAYSTATE_LOADED,&task_);result(DirectStage::LoadedState,value,began);sink_attached_=true;
        }
        if(start_position_>0&&!start_seconds_&&duration_ms_<=0){if(now-opened_ns_>15000000000LL)fail(DirectError::DurationTimeout);}
        else {
            if(start_position_>0){DirectCommand seek;seek.kind=DirectCommandKind::Seek;seek.position=start_seconds_?start_position_:start_position_*duration_ms_/1000.0;control(seek);}
            if(state_!=int(DirectState::Error)){
                begin(DirectStage::Play);const auto began=clock_ms();started_=api_->Play();result(DirectStage::Play,started_,began);
                if(!started_&&state_!=int(DirectState::Error))fail(DirectError::PlayRejected);
                else if(state_==int(DirectState::Loading))stage_=int(DirectStage::AwaitPlaying);
            }
        }
    }
    if(state_==int(DirectState::Loading)&&now-opened_ns_>15000000000LL)fail(loaded_?DirectError::PlayingTimeout:DirectError::LoadTimeout);
    if(!reported_wait_&&state_==int(DirectState::Loading)&&now-opened_ns_>5000000000LL){reported_wait_=true;const auto s=snapshot();
        fprintf(stderr,"[direct-player] waiting stage=%s callbacks=%llu native_requests=%llu upstream_fetches=%llu bytes=%llu failures=%llu elapsed_ms=%lld\n",
            direct_stage_name(s.stage),(unsigned long long)s.callbacks,(unsigned long long)s.relay_requests,(unsigned long long)s.upstream_fetches,(unsigned long long)s.downloaded_bytes,(unsigned long long)s.relay_failures,(long long)((now-opened_ns_)/1000000));}
    auto s=snapshot();
    if(api_&&started_&&s.state!=DirectState::Error){const auto position=api_->getCurrentPlaytime();s.position=position/1000.0;
        if(position!=last_position_ms_||s.state!=DirectState::Playing){last_position_ms_=position;last_progress_ns_=now;}
        if(s.state==DirectState::Playing&&now-last_progress_ns_>30000000000LL){fail(DirectError::ProgressTimeout);s=snapshot();}
        if(!acb_playing_&&s.state==DirectState::Playing){const auto began=clock_ms();fprintf(stderr,"[direct-player] acb-playing begin\n");const auto value=AcbAPI_setState(acb_,APPSTATE_FOREGROUND,PLAYSTATE_PLAYING,&task_);
            fprintf(stderr,"[direct-player] acb-playing end result=%d ms=%lld\n",value,(long long)(clock_ms()-began));acb_playing_=true;}}
    return s;
}
void DirectPlayer::cleanup(){
    {std::lock_guard<std::mutex> lock(context_mutex);contexts.erase(context_id_);context_id_=0;}
    if(session_)session_->cancel_fetch();relay_.stop();session_=nullptr;
    if(api_){auto began=clock_ms();fprintf(stderr,"[direct-player] unload begin\n");const bool value=api_->Unload();fprintf(stderr,"[direct-player] unload end result=%d ms=%lld\n",int(value),(long long)(clock_ms()-began));fprintf(stderr,"[direct-player] destroy begin\n");began=clock_ms();delete api_;api_=nullptr;fprintf(stderr,"[direct-player] destroy end ms=%lld\n",(long long)(clock_ms()-began));}
    if(acb_){auto began=clock_ms();fprintf(stderr,"[direct-player] acb-unloaded begin\n");const auto unloaded=AcbAPI_setState(acb_,APPSTATE_FOREGROUND,PLAYSTATE_UNLOADED,&task_);fprintf(stderr,"[direct-player] acb-unloaded end result=%d ms=%lld\n",unloaded,(long long)(clock_ms()-began));began=clock_ms();fprintf(stderr,"[direct-player] acb-finalize begin\n");const auto value=AcbAPI_finalize(acb_);fprintf(stderr,"[direct-player] acb-finalize end result=%d ms=%lld\n",value,(long long)(clock_ms()-began));AcbAPI_destroy(acb_);acb_=0;}
    {std::lock_guard<std::mutex> lock(info_mutex_);video_info_.clear();info_pending_=false;}
    loaded_=false;started_=acb_playing_=sink_attached_=false;
}
void DirectPlayer::close(){
    cleanup();width_=height_=0;duration_ms_=0;state_=int(DirectState::Idle);stage_=int(DirectStage::Idle);error_=int(DirectError::None);native_result_=0;
    callbacks_=0;last_callback_=-1;last_callback_value_=0;event_types_=0;event_records_=phase_records_=0;reported_wait_=false;
}
