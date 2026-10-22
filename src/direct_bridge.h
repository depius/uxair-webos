#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int uxair_direct_reverse(void*,const char*,int,unsigned long);
void uxair_direct_lost(void*,unsigned long);
int uxair_direct_request(void*,const char*,const char*,const char*,const char*,const char*,int,char**,int*,const char**);
#ifdef __cplusplus
}
#endif
