#include "direct_playback.h"

#include <cstdio>

namespace {
constexpr int64_t native_startup_ns = 12000000000LL;
}

bool DirectPlayback::open(const DirectCommand& play, int64_t now, DirectSession& session) {
    if (!close()) return false;
    play_ = play;
    session_ = &session;
    status_ = DirectSnapshot();
    prior_native_ = DirectSnapshot();
    fallback_reason_ = DirectError::None;
    native_proved_ = pause_requested_ = pending_seek_ = false;
    opened_at_ = now;
    strategy_ = direct_strategy();
    std::fprintf(stderr, "[direct-player] accepted-session strategy=%s\n",
                 strategy_ == DirectStrategy::Auto ? "auto" :
                 strategy_ == DirectStrategy::NativeUri ? "native" : "bufferstream");
    if (strategy_ == DirectStrategy::Bufferstream) {
        start_bufferstream(now);
    } else {
        backend_ = DirectBackend::NativeUri;
        const bool accepted = native_.open(play.uri, play.position, now, &session,
                                           play.position_seconds, direct_configured_contract());
        status_ = native_.snapshot();
        if (!accepted && strategy_ == DirectStrategy::Auto) fallback(status_, status_.error, now);
    }
    return status_.state != DirectState::Error;
}

void DirectPlayback::start_bufferstream(int64_t now) {
    backend_ = DirectBackend::Bufferstream;
    const bool opened = bufferstream_.open(play_, now, session_);
    if (opened && pause_requested_) {
        DirectCommand pause;
        pause.kind = DirectCommandKind::Pause;
        bufferstream_.control(pause, now);
    }
    status_ = bufferstream_.tick(now);
}

void DirectPlayback::fallback(DirectSnapshot native, DirectError reason, int64_t now) {
    prior_native_ = native;
    fallback_reason_ = reason;
    std::fprintf(stderr, "[direct-player] native-startup=%s media-gets=%llu callbacks=%llu fallback=BUFFERSTREAM\n",
        direct_error_name(reason), static_cast<unsigned long long>(native.media_gets),
        static_cast<unsigned long long>(native.callbacks));
    released_ = native_.close();
    if (!released_) {
        status_ = native;
        status_.state = DirectState::Error;
        status_.error = DirectError::CleanupFailed;
        return;
    }
    start_bufferstream(now);
}

void DirectPlayback::control(const DirectCommand& command, int64_t now) {
    if (backend_ == DirectBackend::Bufferstream) {
        bufferstream_.control(command, now);
    } else if (backend_ == DirectBackend::NativeUri) {
        if (command.kind == DirectCommandKind::Pause || command.kind == DirectCommandKind::Resume)
            pause_requested_ = command.kind == DirectCommandKind::Pause;
        if (native_proved_) {
            native_.control(command);
        } else if (command.kind == DirectCommandKind::Seek) {
            play_.position = command.position;
            play_.position_seconds = true;
            pending_seek_ = true;
        }
    }
}

DirectSnapshot DirectPlayback::tick(int64_t now) {
    if (backend_ == DirectBackend::NativeUri && released_) {
        status_ = native_.tick(now);
        if (!native_proved_ && status_.state == DirectState::Playing && native_.progress_observed()) {
            native_proved_ = true;
            if (pending_seek_) {
                DirectCommand seek;
                seek.kind = DirectCommandKind::Seek;
                seek.position = play_.position;
                native_.control(seek);
                pending_seek_ = false;
            }
            if (pause_requested_) {
                DirectCommand pause;
                pause.kind = DirectCommandKind::Pause;
                native_.control(pause);
            }
        }
        if (!native_proved_) {
            const auto reason = status_.state == DirectState::Error ? status_.error : DirectError::NoNativeProgress;
            if (status_.state == DirectState::Error || status_.state == DirectState::Ended ||
                now - opened_at_ > native_startup_ns) {
                if (strategy_ == DirectStrategy::Auto) fallback(status_, reason, now);
                else {
                    status_.state = DirectState::Error;
                    status_.error = reason;
                }
            } else {
                status_.state = DirectState::Loading;
                status_.position = 0;
            }
        }
    }
    if (backend_ == DirectBackend::Bufferstream) {
        status_ = bufferstream_.tick(now);
        status_.fallback_reason = fallback_reason_;
        status_.callbacks = prior_native_.callbacks;
        status_.relay_requests = prior_native_.relay_requests;
        status_.media_gets = prior_native_.media_gets;
    }
    return status_;
}

bool DirectPlayback::close() {
    if (backend_ == DirectBackend::NativeUri) released_ = native_.close();
    if (backend_ == DirectBackend::Bufferstream) bufferstream_.close();
    backend_ = DirectBackend::None;
    session_ = nullptr;
    return released_;
}
