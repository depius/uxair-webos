#pragma once

#include "direct_bufferstream.h"
#include "direct_player.h"

enum class DirectStrategy { Auto, NativeUri, Bufferstream };
bool direct_enabled();
DirectStrategy direct_strategy();

class DirectPlayback {
public:
    bool open(const DirectCommand& play, int64_t now, DirectSession& session);
    void control(const DirectCommand& command, int64_t now);
    DirectSnapshot tick(int64_t now);
    DirectSnapshot snapshot() const { return status_; }
    bool close();

private:
    void start_bufferstream(int64_t now);
    void fallback(DirectSnapshot native, DirectError reason, int64_t now);

    DirectPlayer native_;
    DirectBufferstream bufferstream_;
    DirectSession* session_ = nullptr;
    DirectCommand play_;
    DirectSnapshot prior_native_, status_;
    DirectStrategy strategy_ = DirectStrategy::Auto;
    DirectBackend backend_ = DirectBackend::None;
    DirectError fallback_reason_ = DirectError::None;
    bool native_proved_ = false, pause_requested_ = false, pending_seek_ = false, released_ = true;
    int64_t opened_at_ = 0;
};
