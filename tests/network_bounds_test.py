"""Compile actual generated allocation guards under ASan/UBSan; no media parser changes."""
import importlib.util
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('network_bounds', ROOT/'tools/harden-network.py')
bounds = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bounds)
FIXTURE = (ROOT/'tests/fixtures/pinned_mirror_allocations.c').read_text(encoding='utf-8')


def section(text, name):
    return text.split('/* SECTION '+name+' */\n', 1)[1].split('/* SECTION ', 1)[0]


class NetworkBoundsTest(unittest.TestCase):
    def test_changed_input_fails_closed(self):
        with self.assertRaises(ValueError):
            bounds.bound_mirror(FIXTURE.replace('payload = malloc(payload_size);', 'changed();'))
        with self.assertRaises(ValueError):
            bounds.bound_http_request('changed parser')
        with tempfile.TemporaryDirectory() as d:
            source = pathlib.Path(d)/'lib'; source.mkdir()
            for name in bounds.PINS:
                (source/name).write_text('untrusted')
            out = pathlib.Path(d)/'out'
            result = subprocess.run(['python3' if shutil.which('python3') else 'python', str(ROOT/'tools/harden-network.py'), str(source), str(out)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(out.exists())

    @unittest.skipUnless(shutil.which('cc'), 'C sanitizer compiler unavailable on this host; required in Linux CI')
    def test_generated_allocation_guards(self):
        patched = bounds.bound_mirror(FIXTURE)
        head = section(patched, 'header')
        prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static int alloc_calls, fail_alloc, raw_size;
static size_t allocated;
static void *tracked_malloc(size_t n) { ++alloc_calls; allocated=n; return fail_alloc?NULL:malloc(n); }
static void *tracked_realloc(void *p,size_t n) { ++alloc_calls; allocated=n; return fail_alloc?NULL:realloc(p,n); }
#define malloc tracked_malloc
#define realloc tracked_realloc
#define logger_log(...) ((void)0)
static int byteutils_get_int(void *p,int offset) {(void)p;(void)offset;return raw_size;}
static void reset(void) {alloc_calls=fail_alloc=0;allocated=0;}
'''
        raw = 'static int raw_packet(int n) { raw_size=n;bool conn_reset=false;void *packet=NULL,*payload=NULL;do {\n'+head+section(patched,'raw')+'}while(0);free(payload);return conn_reset;}\n'
        prepend = 'static int prepend_packet(int n,int sps_pps_len) { raw_size=n;bool conn_reset=false;void *packet=NULL;unsigned char *payload_out=NULL;do {\n'+head+section(patched,'prepend')+'}while(0);free(payload_out);return conn_reset;}\n'
        plain = 'static int plain_packet(int n) { raw_size=n;bool conn_reset=false;void *packet=NULL;unsigned char *payload_out=NULL;do {\n'+head+section(patched,'plain')+'}while(0);free(payload_out);return conn_reset;}\n'
        parameter = 'static int parameter_packet(int sps_pps_len) {bool conn_reset=false;unsigned char *sps_pps=NULL;do {\n'+section(patched,'parameter_a')+'}while(0);free(sps_pps);return conn_reset;}\n'
        self.assertEqual(section(patched,'parameter_a'),section(patched,'parameter_b'))
        http = 'struct request {int datalen;void *data;};\nstatic int http_body(struct request *request,size_t length) {\n'+bounds.bound_http_request('    request->data = realloc(request->data, request->datalen + length);\n')+'return 0;}\n'
        main = r'''
int main(void) {
 const int limit=2097152;
 reset();assert(!raw_packet(limit)&&alloc_calls==1&&allocated==(size_t)limit);
 reset();assert(raw_packet(limit+1)&&!alloc_calls);
 reset();assert(raw_packet(-1)&&!alloc_calls);
 reset();assert(raw_packet(INT_MAX)&&!alloc_calls);
 reset();assert(!raw_packet(0)&&allocated==1);
 reset();fail_alloc=1;assert(raw_packet(4)&&alloc_calls==1);
 reset();assert(!prepend_packet(limit-12,12)&&allocated==(size_t)limit);
 reset();assert(prepend_packet(limit-11,12)&&!alloc_calls);
 reset();assert(prepend_packet(4,INT_MAX)&&!alloc_calls);
 reset();assert(prepend_packet(4,-1)&&!alloc_calls);
 reset();fail_alloc=1;assert(prepend_packet(4,12)&&alloc_calls==1);
 reset();assert(!plain_packet(limit)&&allocated==(size_t)limit);
 reset();fail_alloc=1;assert(plain_packet(4));
 reset();assert(!parameter_packet(limit)&&allocated==(size_t)limit);
 reset();assert(parameter_packet(limit+1)&&!alloc_calls);
 reset();assert(parameter_packet(-1)&&!alloc_calls);
 reset();assert(parameter_packet(0)&&!alloc_calls);
 reset();fail_alloc=1;assert(parameter_packet(4));
 struct request r={0,NULL};
 reset();assert(!http_body(&r,33554432U)&&allocated==33554432U);free(r.data);r.data=NULL;
 reset();assert(http_body(&r,33554433U)==-1&&!alloc_calls);
 reset();r.datalen=33554432;assert(http_body(&r,1)==-1&&!alloc_calls);
 reset();r.datalen=-1;assert(http_body(&r,1)==-1&&!alloc_calls);
 reset();r.datalen=0;assert(http_body(&r,SIZE_MAX)==-1&&!alloc_calls);
 reset();r.data=tracked_malloc(4);void *old=r.data;r.datalen=4;fail_alloc=1;
 assert(http_body(&r,4)==-1&&r.data==old);free(r.data);
 puts("PASS: exact wire/body bounds, signed/overflow lengths and allocation failures before use");
}
'''
        scratch = ROOT/'build/tests'; scratch.mkdir(parents=True, exist_ok=True)
        source=scratch/'network-bounds-test.c'; exe=scratch/'network-bounds-test'
        source.write_text(prefix+raw+prepend+plain+parameter+http+main,encoding='utf-8')
        subprocess.run(['cc','-std=c11','-g','-Wall','-Wextra','-fsanitize=address,undefined',str(source),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)


if __name__=='__main__':
    unittest.main()
