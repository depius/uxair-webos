#include "session_lifecycle.h"
#include "audio_session.h"
#include <cassert>
#include <algorithm>
#include <cstdio>
static unsigned simulate(bool corrected) {
    NowPlayingCadence cadence;int64_t now=1,old_draw=0;unsigned received=0,served=0,peak=0;
    while(now<8000000000LL) {
        const bool due=corrected?cadence.due(now,true,true):!old_draw||now-old_draw>33000000LL;
        if(due) {
            old_draw=now;now+=40000000LL;
            if(corrected)cadence.finish(now);
            assert(!corrected || !cadence.due(now,true,true));
        }
        now+=1000000LL;received=unsigned(now/8000000LL);
        if(served<received)++served;
        peak=std::max(peak,received-served);
    }
    return peak;
}
int main() {
    assert(simulate(false)>512);assert(simulate(true)<32);
    NowPlayingCadence cadence;assert(cadence.due(1,false,false));cadence.finish(40000001);
    assert(!cadence.due(40000002,true,true));assert(cadence.due(74000002,true,true));
    IngressState gate;unsigned transport=0,audio=0,copied_transport=transport,copied_audio=audio;
    ++audio;assert(!current_audio_epoch(copied_audio,audio));assert(gate.accepts(copied_transport,transport));
    bool pending=true,paused=false;int64_t retry=10;
    apply_audio_flush(true,false,pending,retry,paused);assert(!pending&&!retry&&paused);
    paused=false;assert(gate.accepts(transport,transport));
    for(unsigned i=0;i<100;++i) {
        copied_transport=transport;assert(standalone_audio_yield_to_video(true,true));
        assert(gate.accepts(copied_transport,transport));
        gate.close();++transport;assert(!gate.accepts(copied_transport,transport));
        assert(!gate.admit());
        gate.disconnected();assert(!gate.accepts(transport,transport));
        assert(gate.admit());assert(gate.accepts(transport,transport));
        assert(!gate.accepts(copied_transport,transport));
    }
    ResetNotice notice;assert(!notice.pending());notice.add(ResetOrigin::SenderTeardown,1,transport);
    assert(notice.pending()&&!notice.failure()&&notice.video_reason==1);
    notice.add(ResetOrigin::Capacity,0,transport);assert(notice.failure());
    notice=ResetNotice();notice.add(ResetOrigin::TransportCallback,2,transport);
    notice.add(ResetOrigin::SenderTeardown,1,transport);assert(notice.failure()&&notice.transport_reason==2);
    notice=ResetNotice();notice.add(ResetOrigin::VideoCallback,1,transport);assert(notice.failure());
    notice.add(ResetOrigin::SenderTeardown,1,transport+1);assert(!notice.failure()&&notice.epoch==transport+1);
    puts("PASS: slow-render queue starvation, cadence, FLUSH/resume, closing ingress, handoffs and reset classification");
}
