#include "audio_session.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
    AirPlayAudioIntent intent;
    assert(!standalone_audio_candidate(intent,false));
    intent.configured=true;
    assert(!standalone_audio_candidate(intent,false)); // no negotiated codec
    for(unsigned codec : {1u,2u,4u,8u}) {
        intent.codec=codec;
        intent.using_screen=false;
        assert(standalone_audio_candidate(intent,false));
        assert(!standalone_audio_candidate(intent,true));
        intent.using_screen=true;
        assert(!standalone_audio_candidate(intent,false)); // preserve mirroring A/V lifecycle
    }
    intent.using_screen=false;
    for(unsigned codec : {3u,5u,6u,7u,9u}) {
        intent.codec=codec;
        assert(!standalone_audio_candidate(intent,false)); // unsupported RAOP packet formats
    }
    assert(!standalone_audio_yield_to_video(false,true));
    assert(!standalone_audio_yield_to_video(true,false));
    assert(standalone_audio_yield_to_video(true,true));
    bool pending=true,paused=false;int64_t stall=100;
    assert(!feed_retry_expired(stall,3000000100LL));
    assert(feed_retry_expired(stall,3000000101LL));
    apply_audio_flush(true,false,pending,stall,paused);
    assert(!pending && !stall && paused);
    // First post-FLUSH packet starts a fresh retry budget, not the old deadline.
    assert(!feed_retry_expired(stall,10000000000LL));
    stall=10000000000LL;assert(!feed_retry_expired(stall,10000000001LL));
    unsigned epoch=1,copied=epoch;++epoch;
    assert(!current_audio_epoch(copied,epoch)); // callback copied before FLUSH/teardown
    assert(current_audio_epoch(epoch,epoch));
    // Mirror FLUSH retains a pending video frame and its independent stall.
    pending=true;paused=false;stall=123;
    apply_audio_flush(false,true,pending,stall,paused);
    assert(pending && stall==123 && !paused);
    // Audio -> video handoff preserves the triggering frame (main finish(...,true,true)).
    assert(standalone_audio_yield_to_video(true,true));
    pending=true;paused=false;stall=123;
    apply_audio_flush(false,false,pending,stall,paused);
    assert(!pending && !stall && !paused);
    std::puts("PASS: standalone RAOP audio is gated by negotiated codec, usingScreen, and mirror state");
}
