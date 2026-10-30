#pragma once
#include <atomic>
#include <string>

// UxAir's own connection to the TV's system bus (luna), on a thread of its own.
//
// Screen saver: the TV asks registered clients before it starts its screen
// saver (tvpower registerScreenSaverRequest, after about 90 s without picture
// changes or keys - a Now Playing screen counts as still); while a session
// plays, UxAir answers ack:false to postpone it, as video apps do. The Now
// Playing screen dims for the OLED instead. Paused playback and settings do
// not inhibit the system screen saver.
//
// Foreground: UxAir keeps running in the background so it stays discoverable
// (webOS does not freeze it). A connecting device brings it to the front; when
// the device leaves, the app that was in front before comes back - as LG's own
// AirPlay behaves.
class SystemBus {
public:
    ~SystemBus();
    bool start(const char *app_id);
    void block_screen_saver(bool on) { block_ = on; }
    void bring_to_front();      // remembers the app in front, then launches UxAir
    void return_to_previous();  // the app that was in front before bring_to_front
    void to_background(const char *app_id = nullptr);   // that app, else the previous one, else Home
    struct Impl;   // (luna callbacks live outside the class)
private:
    std::atomic<bool> block_{false};
    Impl *impl_ = nullptr;
};
