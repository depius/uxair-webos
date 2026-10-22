#include "direct_player.h"
#include <StarfishMediaAPIs.h>
#include <AcbAPI.h>
#include <cstdio>
static void direct_acb(long,long,long,long,long,const char*){}
void DirectPlayer::event(int type,int64_t value,const char*,void *user){
    auto& p=*static_cast<DirectPlayer*>(user);
    // Firmware text may contain signed URLs. Log only numeric transitions.
    switch(type){
    case PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED:p.loaded_=true;break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING:p.state_=int(DirectState::Playing);break;
    case 0x1b:p.state_=int(DirectState::Paused);break;
    case PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM:p.state_=int(DirectState::Ended);break;
    case PF_EVENT_TYPE_INT_DURATION:p.duration_ms_=value>0?value:0;break;
    case PF_EVENT_TYPE_INT_ERROR:case PF_EVENT_TYPE_STR_ERROR:p.state_=int(DirectState::Error);break;
    default:return;}
    fprintf(stderr,"[direct-player] event=0x%x value=%lld\n",type,(long long)value);
}
DirectPlayer::~DirectPlayer(){close();}
bool DirectPlayer::open(const std::string& uri,double fraction,int64_t now,DirectSession *session){
    close();relay_.sender_manifest=session?[session](const std::string& url){return session->sender_manifest(url);}:std::function<std::string(const std::string&)>();
    const auto local=relay_.start(uri);const auto payload=direct_load_payload(local,"com.github.depius.uxair.dev",true);if(payload.empty())return false;
    start_fraction_=fraction;opened_ns_=now;state_=int(DirectState::Loading);
    acb_=AcbAPI_create();if(!acb_||!AcbAPI_initialize(acb_,PLAYER_TYPE_MSE,"com.github.depius.uxair.dev",direct_acb)){close();return false;}
    api_=new StarfishMediaAPIs(nullptr);
    if(!AcbAPI_setMediaId(acb_,api_->getMediaID())){close();return false;}
    AcbAPI_setDisplayWindow(acb_,0,0,1920,1080,true,&task_);
    if(!api_->Load(payload.c_str(),event,this)){close();return false;}return true;
}
bool DirectPlayer::control(const DirectCommand& c){
    if(!api_)return false;
    switch(c.kind){case DirectCommandKind::Pause:return api_->Pause();case DirectCommandKind::Resume:return api_->Play();
    case DirectCommandKind::Seek:{char value[48];snprintf(value,sizeof(value),"%.0f",c.position*1000);return api_->Seek(value);}
    case DirectCommandKind::Stop:close();return true;default:return false;}
}
DirectSnapshot DirectPlayer::tick(int64_t now){
    if(api_&&loaded_&&!started_){
        AcbAPI_setSinkType(acb_,SINK_TYPE_MAIN);AcbAPI_setState(acb_,APPSTATE_FOREGROUND,PLAYSTATE_LOADED,&task_);
        if(start_fraction_>0&&duration_ms_<=0){if(now-opened_ns_>15000000000LL)state_=int(DirectState::Error);}
        else {if(start_fraction_>0){DirectCommand seek;seek.kind=DirectCommandKind::Seek;seek.position=start_fraction_*duration_ms_/1000.0;if(!control(seek))state_=int(DirectState::Error);}
            if(state_!=int(DirectState::Error)){started_=api_->Play();if(!started_)state_=int(DirectState::Error);}}
    }
    if(state_==int(DirectState::Loading)&&now-opened_ns_>15000000000LL)state_=int(DirectState::Error);
    DirectSnapshot s;s.state=DirectState(state_.load());s.duration=duration_ms_.load()/1000.0;
    if(api_&&started_){s.position=api_->getCurrentPlaytime()/1000.0;if(!acb_playing_&&s.state==DirectState::Playing){AcbAPI_setState(acb_,APPSTATE_FOREGROUND,PLAYSTATE_PLAYING,&task_);acb_playing_=true;}}
    return s;
}
void DirectPlayer::close(){
    if(api_){api_->Unload();delete api_;api_=nullptr;}
    if(acb_){AcbAPI_setState(acb_,APPSTATE_FOREGROUND,PLAYSTATE_UNLOADED,&task_);AcbAPI_finalize(acb_);AcbAPI_destroy(acb_);acb_=0;}
    relay_.stop();
    loaded_=false;started_=acb_playing_=false;duration_ms_=0;state_=int(DirectState::Idle);
}
