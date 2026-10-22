#pragma once
#define PLAYER_TYPE_MSE 1
#define SINK_TYPE_MAIN 1
#define APPSTATE_FOREGROUND 1
#define PLAYSTATE_LOADED 1
#define PLAYSTATE_PLAYING 2
#define PLAYSTATE_UNLOADED 3
inline long AcbAPI_create(){return 1;}
inline bool AcbAPI_initialize(long,int,const char*,void(*)(long,long,long,long,long,const char*)){return true;}
inline bool AcbAPI_setMediaId(long,const char*){return true;}
inline void AcbAPI_setDisplayWindow(long,int,int,int,int,bool,long*){}
inline void AcbAPI_setSinkType(long,int){}
inline void AcbAPI_setState(long,int,int,long*){}
inline void AcbAPI_finalize(long){}
inline void AcbAPI_destroy(long){}
