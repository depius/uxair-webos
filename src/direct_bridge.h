#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void uxair_direct_lost(void*,unsigned long);
int uxair_direct_request(void*,const char*,const char*,const char*,const char*,const char*,int,char**,int*,const char**,const char**,const unsigned char*,unsigned long);
int uxair_direct_encrypted_reverse(void*,const char*,const unsigned char*,unsigned long);
int uxair_direct_next_event(void*,unsigned long,char*,int,uint64_t*);
#ifdef __cplusplus
}
#endif
