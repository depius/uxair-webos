#pragma once
#include <string>
#include <cstdint>
enum {PF_EVENT_TYPE_STR_STATE_UPDATE__LOADCOMPLETED=0x16,PF_EVENT_TYPE_STR_STATE_UPDATE__PLAYING=0x1a,PF_EVENT_TYPE_STR_STATE_UPDATE__ENDOFSTREAM=0x1c,PF_EVENT_TYPE_INT_DURATION=3,PF_EVENT_TYPE_INT_ERROR=0x12,PF_EVENT_TYPE_STR_ERROR=0x13};
class StarfishMediaAPIs {
public:
 static void (*callback)(int,int64_t,const char*,void*);static void *context;static std::string payload;static int unloads;static bool load_success;
 StarfishMediaAPIs(const char*){} const char *getMediaID(){return "stub";}
 bool Load(const char *p,void(*cb)(int,int64_t,const char*,void*),void *ctx){payload=p;callback=cb;context=ctx;return load_success;}
 bool Play(){callback(0x1a,0,nullptr,context);return true;}bool Pause(){callback(0x1b,0,nullptr,context);return true;}
 bool Seek(const char*){return true;}bool Unload(){++unloads;return true;}int64_t getCurrentPlaytime(){return 1000;}
};
