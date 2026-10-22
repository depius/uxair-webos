#include "shell.h"
#include <SDL_ttf.h>
#include <cassert>
#include <cstdio>
#include <algorithm>

struct HistoryRenderTest {
    static void fonts(Shell& shell) {
        const char *path="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
        shell.small_=TTF_OpenFont(path,23);shell.font_=TTF_OpenFont(path,30);shell.large_=TTF_OpenFont(path,76);
        assert(shell.small_&&shell.font_&&shell.large_);
    }
    static void about(Shell& shell,SDL_Surface *surface) {
        int tw=0;TTF_SizeUTF8(shell.font_,"Matej Skuhala (@depius)",&tw,nullptr);assert(tw<=(1920-512)*30/100);
        std::vector<UiRow> rows;
        rows.emplace_back("UxAir","1.1.0 Dev","AirPlay Receiver for webOS",false);
        rows.emplace_back("Developer","Matej Skuhala (@depius)","Based on UxPlay by FDH2; uxplay-webOS by @danlflh",false);
        rows.emplace_back("License","GPL-3.0-or-later","See bundled license and third-party notices",false);
        rows.emplace_back("Enjoying UxAir? Buy me a coffee","","Support development · UxAir is free and open source");
        SDL_Surface *icon=SDL_LoadBMP("assets/bmc-icon.bmp");assert(icon);
        shell.donation_icon_=SDL_CreateTextureFromSurface(shell.renderer,icon);SDL_FreeSurface(icon);assert(shell.donation_icon_);
        shell.page(UiPage::System,"",rows,3,"Left/Right sections");
        assert(SDL_SaveBMP(surface,"build/tests/history-about.bmp")==0);
    }
    static void render(Shell& shell,SDL_Surface *surface,bool empty,uint32_t today=20747) {
        UsageStats stats;stats.daily_from_day=20736; // 2026-10-10 UTC
        if(!empty){DailyUsage d;d.utc_day=20747;d.mirror_ms=100000;d.audio_ms=51000;d.direct_ms=30000;stats.daily.push_back(d);}
        auto days=summarize_usage(stats,UsagePeriod::ThisMonth,today);
        auto months=summarize_usage(stats,UsagePeriod::ThisYear,today);
        assert(days.title=="This month — October 2026");
        UiHistoryData data;data.daily_title=days.title;data.monthly_title=months.title;
        data.daily_bars=days.bars;data.monthly_bars=months.bars;
        data.summary=empty?"0 completed":"5 completed";data.total_time="2 min 31 s";data.average="30 s";
        data.mirror_time="1 min 40 s";data.direct_time="30 s";data.music_time="51 s";data.mirror_percent=empty?0:66;data.audio_percent=empty?0:34;
        data.storage="History storage 1 KB | 5/100 sessions | 365d";
        std::vector<UiRow> rows;
        if(!empty)for(int i=0;i<5;++i)rows.emplace_back("2026-10-21 12:30","30 s","Mirror / HEVC / 3840x2160");
        rows.emplace_back("Reset history","","Reset saved usage history");
        SDL_Rect previous={0,0,1920,1080};SDL_RenderSetClipRect(shell.renderer,&previous);
        shell.page(UiPage::History,"",rows,empty?0:2,"Left/Right sections",nullptr,"",false,&data);
        SDL_Rect restored;SDL_RenderGetClipRect(shell.renderer,&restored);
        assert(SDL_RenderIsClipEnabled(shell.renderer)&&restored.w==1920&&restored.h==1080);
        const auto layout=history_ui_layout(1920,1080);
        assert(layout.list_y+5*layout.row_h<layout.reset_y-4);
        int tw=0;TTF_SizeUTF8(shell.small_,"UTC · Blue label: current · * Partial history · — Unavailable · • Known zero · Blank: future",&tw,nullptr);
        assert(tw<=layout.content_width);
        // Actual SDL pixels: no stacked-bar colors are allowed in either chart header.
        const int plot_top=layout.chart_y+TTF_FontHeight(shell.small_)+26;
        for(int y=layout.chart_y;y<plot_top;++y)for(int x=layout.panel_x;x<1920-48;++x) {
            Uint32 pixel=*(reinterpret_cast<Uint32*>(static_cast<Uint8*>(surface->pixels)+y*surface->pitch)+x);
            Uint8 r,g,b;SDL_GetRGB(pixel,surface->format,&r,&g,&b);
            assert(!(r==101&&g==185&&b==246)&&!(r==104&&g==194&&b==157)&&!(r==187&&g==145&&b==239));
        }
        assert(SDL_SaveBMP(surface,today!=20747?"build/tests/history-month-end.bmp":empty?"build/tests/history-empty.bmp":"build/tests/history-five-sessions.bmp")==0);
    }
};
int main() {
    assert(SDL_Init(0)==0&&TTF_Init()==0);
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,1920,1080,32,SDL_PIXELFORMAT_ARGB8888);assert(surface);
    {Shell shell;shell.renderer=SDL_CreateSoftwareRenderer(surface);assert(shell.renderer);
     HistoryRenderTest::fonts(shell);HistoryRenderTest::render(shell,surface,false);HistoryRenderTest::render(shell,surface,true);HistoryRenderTest::render(shell,surface,false,20756);HistoryRenderTest::about(shell,surface);}
    SDL_FreeSurface(surface);
    puts("PASS: actual SDL History rendering, five-session bounds, clip restoration and header pixels");
}
