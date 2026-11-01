# Compile the instrumented pinned TEARDOWN excerpt, not a rewrite.
from pathlib import Path
import importlib.util,subprocess
root=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('lifecycle',root/'tools/instrument-lifecycle.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
fixture=(root/'tests/fixtures/raop_teardown.h').read_text(encoding='utf-8')
handler=m.instrument_handler(fixture)
assert handler.count('uxair_sender_video_reset(')==2
try:m.instrument_handler(fixture.replace(m.CALL,''))
except ValueError:pass
else:raise AssertionError('changed upstream accepted')
prefix=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define LOGGER_DEBUG 0
#define RESET_TYPE_RTP_SHUTDOWN 1
#define RESET_TYPE_RTP_TO_HLS_TEARDOWN 5
#define RESET_TYPE_HLS_SHUTDOWN 2
#define CONNECTION_TYPE_HLS 1
#define logger_log(...) ((void)0)
typedef struct node {int type,count;struct node *items[2];} node;
typedef node *plist_t;
static node root_node,array_node,items[2];
static void plist_from_bin(const char *data,int n,plist_t *out){(void)data;(void)n;*out=&root_node;}
static plist_t plist_dict_get_item(plist_t p,const char *key){(void)key;return p==&root_node?&array_node:p;}
#define PLIST_IS_ARRAY(x) ((x)->count>=0)
static int plist_array_get_size(plist_t p){return p->count;}
static plist_t plist_array_get_item(plist_t p,int i){return p->items[i];}
static void plist_get_uint_val(plist_t p,uint64_t *out){*out=p->type;}
static void plist_free(plist_t p){(void)p;}
typedef int http_request_t;typedef int http_response_t;
static const char *http_request_get_data(http_request_t *r,int *n){(void)r;*n=0;return NULL;}
static void http_response_add_header(http_response_t *r,const char *k,const char *v){(void)r;(void)k;(void)v;}
static int generic,sender,last_reason,stopped,destroyed,hls_count;
static void generic_reset(void *p,int reason){(void)p;++generic;last_reason=reason;}
static void uxair_sender_video_reset(void *p,int reason){(void)p;++sender;last_reason=reason;}
typedef struct {struct {void (*video_reset)(void*,int);void (*audio_stop_coverart_rendering)(void*);void *cls;} callbacks;int hls_pending;void *httpd;} raop_t;
typedef struct {raop_t *raop;void *raop_rtp,*raop_rtp_mirror;} raop_conn_t;
static void raop_rtp_stop(void *p){(void)p;++stopped;}
static void raop_rtp_destroy(void *p){(void)p;++destroyed;}
static void raop_rtp_mirror_stop(void *p){(void)p;++stopped;}
static void raop_rtp_mirror_destroy(void *p){(void)p;++destroyed;}
static int httpd_count_connection_type(void *p,int t){(void)p;(void)t;return hls_count;}
'''
suffix=r'''
int main(void){
 raop_t r={0};r.callbacks.video_reset=generic_reset;
 for(int test=0;test<4;++test){
  generic=sender=stopped=destroyed=last_reason=hls_count=0;r.hls_pending=test==3;
  array_node.count=test==2?0:1;array_node.items[0]=&items[0];items[0].type=test==0?96:110;
  raop_conn_t c={&r,(void*)1,(void*)1};http_request_t req=0;http_response_t resp=0;
  raop_handler_teardown(&c,&req,&resp,NULL,NULL);
  if(test==0)assert(!sender&&!generic&&stopped==1);
  if(test==1)assert(sender==1&&!generic&&last_reason==1&&stopped==1);
  if(test==2)assert(sender==1&&!generic&&last_reason==1&&destroyed==2);
  if(test==3)assert(!sender&&generic==1&&last_reason==5);
 }
 puts("PASS: pinned TEARDOWN audio/video/all-stream/HLS paths retain cleanup and exact origin");
}
'''
source=root/'build/tests/teardown-origin-test.c';source.parent.mkdir(parents=True,exist_ok=True)
source.write_text(prefix+handler+suffix,encoding='utf-8')
exe=root/'build/tests/teardown-origin-test'
subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined',str(source),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
