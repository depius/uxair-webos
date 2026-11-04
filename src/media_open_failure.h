#pragma once

enum class MediaOpenFailure {
    None,
    InvalidConfiguration,
    AcbCreate,
    AcbInitialize,
    MediaIdRegistration,
    StarfishLoad
};

inline const char *media_open_failure_message(MediaOpenFailure failure) {
    switch (failure) {
    case MediaOpenFailure::InvalidConfiguration: return "Video playback could not start (invalid media configuration).";
#if defined(UXAIR_MEDIA_NDL)
    case MediaOpenFailure::AcbCreate: return "Video playback could not start (video window creation failed).";
#else
    case MediaOpenFailure::AcbCreate: return "Video playback could not start (ACB creation failed).";
#endif
    case MediaOpenFailure::AcbInitialize: return "Video playback could not start (ACB initialization failed).";
    case MediaOpenFailure::MediaIdRegistration: return "Video playback could not start (ACB media registration failed).";
#if defined(UXAIR_MEDIA_NDL)
    case MediaOpenFailure::StarfishLoad: return "Video playback could not start (NDL Load failed).";
#else
    case MediaOpenFailure::StarfishLoad: return "Video playback could not start (Starfish Load failed).";
#endif
    case MediaOpenFailure::None: break;
    }
    return "Video playback could not start.";
}
