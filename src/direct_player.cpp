#include "direct_player.h"
#include <StarfishMediaAPIs.h>
#include <AcbAPI.h>
#include <cstdio>
#include <map>
#include <cstring>
#include <cstdlib>
extern "C" int direct_acb_video_info(long,const char*,long*) __asm__("AcbAPI_setMediaVideoData");
static std::mutex context_mutex;static std::map<uintptr_t,DirectPlayer*> contexts;static std::atomic<uintptr_t> next_context{1};
static void direct_acb(long,long,long,long,long,const char*){}
void DirectPlayer::event(int type,int64_t value,const char* message,void *user){
    std::lock_guard<std::mutex> context_lock(context_mutex);
    auto entry=contexts.find(reinterpret_cast<uintptr_t>(user));if(entry==contexts.end())return;
    auto& p=*entry->second;
    // Firmware text may contain signed URLs. Log only numeric transitions.
    switch(type){
    case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED:p.loaded_=true;break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING:p.state_=int(DirectState::Playing);break;
    case 0x1b:p.state_=int(DirectState::Paused);break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM:p.state_=int(DirectState::Ended);break;
    case 4:
        if(message&&strnlen(message,8193)<=8192){std::lock_guard<std::mutex> lock(p.info_mutex_);p.video_info_=message;p.info_pending_=true;
            auto value=[&](const char *field){auto at=p.video_info_.find(field);if(at==std::string::npos)return 0U;at=p.video_info_.find(':',at);if(at==std::string::npos)return 0U;long n=strtol(p.video_info_.c_str()+at+1,nullptr,10);return n>0&&n<=4096?unsigned(n):0U;};
            p.width_=value("\"width\"");p.height_=value("\"height\"");}
        break;
    case PF_EVENT_TYPE_INT_DURATION:p.duration_ms_=value>0?value:0;break;
    case PF_EVENT_TYPE_INT_ERROR:case PF_EVENT_TYPE_STR_ERROR:p.state_=int(DirectState::Error);break;
    default:return;}
    fprintf(stderr,"[direct-player] event=0x%x value=%lld\n",type,(long long)value);
}
DirectPlayer::~DirectPlayer(){close();}
bool DirectPlayer::open(const std::string& uri,double position,int64_t now,DirectSession *session,bool seconds){
    close();session_=session;relay_.sender_manifest=session?[session](const std::string& url){return session->sender_manifest(url);}:std::function<std::string(const std::string&)>();
    const auto local=relay_.start(uri);const auto payload=direct_load_payload(local,"com.github.depius.uxair.dev",true);if(payload.empty())return false;
    start_position_=position;start_seconds_=seconds;opened_ns_=last_progress_ns_=now;last_position_ms_=-1;state_=int(DirectState::Loading);
    acb_=AcbAPI_create();if(!acb_||!AcbAPI_initialize(acb_,PLAYER_TYPE_MSE,"com.github.depius.uxair.dev",direct_acb)){close();return false;}
    api_=new StarfishMediaAPIs(nullptr);
    if(!AcbAPI_setMediaId(acb_,api_->getMediaID())){close();return false;}
    AcbAPI_setDisplayWindow(acb_,0,0,1920,1080,true,&task_);
    context_id_=next_context++;if(!context_id_){close();return false;}
    {std::lock_guard<std::mutex> lock(context_mutex);contexts[context_id_]=this;}
    if(!api_->Load(payload.c_str(),event,reinterpret_cast<void*>(context_id_))){close();return false;}return true;
}
bool DirectPlayer::control(const DirectCommand& c){
    if(!api_)return false;
    switch(c.kind){case DirectCommandKind::Pause:return api_->Pause();case DirectCommandKind::Resume:return api_->Play();
    case DirectCommandKind::Seek:{char value[48];snprintf(value,sizeof(value),"%.0f",c.position*1000);return api_->Seek(value);}
    case DirectCommandKind::Stop:close();return true;default:return false;}
}
DirectSnapshot DirectPlayer::tick(int64_t now){
    std::string info;
    {std::lock_guard<std::mutex> lock(info_mutex_);if(info_pending_){info=video_info_;info_pending_=false;}}
    if(api_&&!info.empty()){
        direct_acb_video_info(acb_,info.c_str(),&task_);
        const int width=int(width_.load()),height=int(height_.load());
        if(width>0&&height>0){int w=1920,h=1080;if(1920*height>1080*width)w=1080*width/height;else h=1920*height/width;AcbAPI_setDisplayWindow(acb_,(1920-w)/2,(1080-h)/2,w,h,true,&task_);}
    }
    if(api_&&loaded_&&!started_){
        AcbAPI_setSinkType(acb_,SINK_TYPE_MAIN);AcbAPI_setState(acb_,APPSTATE_FOREGROUND,PLAYSTATE_LOADED,&task_);
        if(start_position_>0&&!start_seconds_&&duration_ms_<=0){if(now-opened_ns_>15000000000LL)state_=int(DirectState::Error);}
        else {if(start_position_>0){DirectCommand seek;seek.kind=DirectCommandKind::Seek;seek.position=start_seconds_?start_position_:start_position_*duration_ms_/1000.0;if(!control(seek))state_=int(DirectState::Error);}
            if(state_!=int(DirectState::Error)){started_=api_->Play();if(!started_)state_=int(DirectState::Error);}}
    }
    if(state_==int(DirectState::Loading)&&now-opened_ns_>15000000000LL)state_=int(DirectState::Error);
    DirectSnapshot s;s.state=DirectState(state_.load());s.duration=duration_ms_.load()/1000.0;s.width=width_;s.height=height_;
    if(api_&&started_){const auto position=api_->getCurrentPlaytime();s.position=position/1000.0;
        if(position!=last_position_ms_||s.state!=DirectState::Playing){last_position_ms_=position;last_progress_ns_=now;}
        if(s.state==DirectState::Playing&&now-last_progress_ns_>30000000000LL){state_=int(DirectState::Error);s.state=DirectState::Error;}
        if(!acb_playing_&&s.state==DirectState::Playing){AcbAPI_setState(acb_,APPSTATE_FOREGROUND,PLAYSTATE_PLAYING,&task_);acb_playing_=true;}}
    return s;
}
void DirectPlayer::close(){
    {std::lock_guard<std::mutex> lock(context_mutex);contexts.erase(context_id_);context_id_=0;}
    if(session_)session_->cancel_fetch();relay_.stop();session_=nullptr;
    if(api_){api_->Unload();delete api_;api_=nullptr;}
    if(acb_){AcbAPI_setState(acb_,APPSTATE_FOREGROUND,PLAYSTATE_UNLOADED,&task_);AcbAPI_finalize(acb_);AcbAPI_destroy(acb_);acb_=0;}
    relay_.stop();
    {std::lock_guard<std::mutex> lock(info_mutex_);video_info_.clear();info_pending_=false;}width_=height_=0;
    loaded_=false;started_=acb_playing_=false;duration_ms_=0;state_=int(DirectState::Idle);
}
