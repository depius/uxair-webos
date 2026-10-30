#pragma once
#include <SDL.h>
#include "input.h"
#include "ui_navigation.h"
#include "usage_stats.h"
#include <cstdint>
#include <string>
#include <vector>
struct _TTF_Font;
struct UiRow {
    std::string label, value, note;
    bool enabled;
    UiRow(const std::string& l,const std::string& v,const std::string& n,bool e=true)
        : label(l),value(v),note(n),enabled(e) {}
};
using UiHistoryBar = UsageBar;
struct UiHistoryData {
    std::string daily_title,monthly_title;
    std::string summary,total_time,average,mirror_time,music_time,storage;
    std::vector<UiHistoryBar> daily_bars,monthly_bars;
    unsigned mirror_percent=0,audio_percent=0;
    size_t session_count=0; bool empty=true;
};
struct UiDialog {
    std::string title, description;
    std::vector<std::string> choices;
    int selected = 0;
    int current = -1;
};
// The Now Playing screen during audio-only AirPlay.
struct NowPlayingView {
    std::string title, artist, album, source;
    const std::vector<uint8_t> *cover = nullptr;   // JPEG/PNG, may be empty
    uint64_t cover_generation = 0;
    double elapsed_s = 0, duration_s = 0;           // duration 0: no progress bar
    float brightness = 1;                           // 1 full, lower when idle (OLED)
};
enum class UiHitKind { None, Category, Row, DialogChoice, Cancel };
struct UiHit {
    UiHitKind kind; int index;
    UiHit(UiHitKind k=UiHitKind::None,int i=-1):kind(k),index(i) {}
};
class Shell {
public:
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    bool open();
    bool pump();
    Action action();
    bool pointer_select(int& x, int& y) const;
    bool take_pointer_motion();
    UiHit hit_test(int x, int y, int row_count, const UiDialog *dialog = nullptr, int selected = 0, UiPage page = UiPage::Mirroring) const;
    void screen(const std::string& status, const std::string& pin, int audio_delay_ms=100);
    void page(UiPage page, const std::string& subtitle, const std::vector<UiRow>& rows, int selected,
              const std::string& footer, const UiDialog *dialog = nullptr,
              const std::string& pin = std::string(), bool donation_open = false, const UiHistoryData *history = nullptr);
    void overlay(const std::vector<std::string>& lines, const std::string& pin = std::string());
    bool now_playing(const NowPlayingView& view);   // true while the title scrolls (redraw often)
    void transparent();
    ~Shell();
private:
    friend struct HistoryRenderTest;
    InputMapper input_{SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN};
    bool pointer_select_ = false;
    int pointer_x_ = 0, pointer_y_ = 0;
    bool pointer_valid_=false,pointer_moved_=false;
    _TTF_Font *font_=nullptr, *large_=nullptr, *small_=nullptr, *pin_font_=nullptr, *pin_font_small_=nullptr;
    _TTF_Font *np_title_=nullptr, *np_artist_=nullptr, *np_album_=nullptr;
    SDL_Texture *donation_qr_=nullptr,*donation_icon_=nullptr;
    SDL_Texture *cover_=nullptr;          // decoded cover of cover_generation_
    uint64_t cover_generation_=~uint64_t(0);
    SDL_Color cover_tint_={29,45,71,255}; // the cover's average colour
    SDL_Texture *title_=nullptr;          // the rendered title, scrolled when too wide
    std::string title_text_;
    int title_w_=0,title_h_=0;
    Uint32 title_since_=0;
    void text(const std::string&,int y,_TTF_Font*,SDL_Color);
    void text_at(const std::string&,int x,int y,int max_width,_TTF_Font*,SDL_Color);
    void draw_history(const UiHistoryData&,const std::vector<UiRow>&,int,int,int,int);
    void draw_donation_modal(int w, int h);
    void draw_pairing_modal(const std::string& pin, int w, int h);
};
int64_t now_ns();
std::string app_directory();
std::string data_directory();
bool start_log(const char *name);
void rotate_log();
