#pragma once
// Values and return types from the public native AcbAPI declarations.
#define PLAYER_TYPE_VIDEO 0
#define PLAYER_TYPE_MSE 10
#define SINK_TYPE_MAIN 0
#define APPSTATE_FOREGROUND 1
#define PLAYSTATE_LOADED 1
#define PLAYSTATE_PLAYING 2
#define PLAYSTATE_UNLOADED 0
extern bool acb_initialize_success,acb_sink_success;
extern int acb_player_type,acb_sink_calls;
inline long AcbAPI_create(){return 1;}
inline bool AcbAPI_initialize(long,int type,const char*,void(*)(long,long,long,long,long,const char*)){acb_player_type=type;return acb_initialize_success;}
inline bool AcbAPI_setMediaId(long,const char*){return true;}
inline int AcbAPI_setDisplayWindow(long,long,long,long,long,bool,long*){return 0;}
inline bool AcbAPI_setSinkType(long,int){++acb_sink_calls;return acb_sink_success;}
inline int AcbAPI_setState(long,int,int,long*){return 0;}
inline int AcbAPI_finalize(long){return 0;}
inline void AcbAPI_destroy(long){}
