#include "shell.h"
#include "identity.h"
#include "usage_stats.h"
#include "now_playing.h"
#include "secure_data.h"
#include <SDL_webOS.h>
#include <SDL_ttf.h>
#include <dlfcn.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <algorithm>
#include <cmath>
static std::string log_path;
static volatile sig_atomic_t interrupted = 0;
static void signal_stop(int) { interrupted = 1; }
static const SDL_Color uxair_navy={29,45,71,255};
static const SDL_Color uxair_background={16,28,46,255};
static const SDL_Color uxair_surface={41,62,93,255};
static const SDL_Color uxair_accent={101,185,246,255};
static const SDL_Color uxair_text={245,248,252,255};
static void fill_round_rect(SDL_Renderer *renderer,const SDL_Rect& rect,int radius,SDL_Color color) {
    SDL_SetRenderDrawColor(renderer,color.r,color.g,color.b,color.a);
    radius=std::max(0,std::min(radius,std::min(rect.w,rect.h)/2));
    for(int y=0;y<rect.h;++y) {
        int inset=0;
        if(y<radius) {
            double dy=radius-y-0.5;
            inset=radius-int(std::sqrt(std::max(0.0,double(radius*radius)-dy*dy)));
        } else if(y>=rect.h-radius) {
            double dy=y-(rect.h-radius)+0.5;
            inset=radius-int(std::sqrt(std::max(0.0,double(radius*radius)-dy*dy)));
        }
        SDL_RenderDrawLine(renderer,rect.x+inset,rect.y+y,rect.x+rect.w-inset-1,rect.y+y);
    }
}
int64_t now_ns() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return int64_t(t.tv_sec)*1000000000 + t.tv_nsec; }
std::string app_directory() {
    char p[4096]; ssize_t n = readlink("/proc/self/exe", p, sizeof(p)-1);
    if (n <= 0) return ".";
    p[n] = 0; std::string s(p); return s.substr(0, s.rfind('/'));
}
// Where the app may write (log, pairing keys, settings). webOS 3.9: its own
// folder. webOS 5+ (NDL build): a folder of its own on the developer
// partition, which the jail mounts writable and keeps across restarts and
// reinstalls. The app folder is no choice there: an install replaces it, and
// after a restart the TV leaves it writable for everyone, so testing it for
// write access would switch to an empty state (new identity, default
// settings). Starfish keeps its historical app directory and existing state.
std::string data_directory() {
    static std::string dir;
    if (!dir.empty()) return dir;
    std::string app = app_directory();
    std::string id = app.substr(app.rfind('/') + 1);
    std::string lib = "/media/developer/apps/var/lib/" + id;
#if defined(UXAIR_MEDIA_NDL)
    const bool ndl=true;
#else
    const bool ndl=false;
#endif
    const std::string selected=select_state_root(app,lib,ndl);
    const bool use_lib=selected==lib;
    state_environment(app,lib,selected,ndl);
    if(prepare_state(selected,use_lib))return dir=selected;
    fprintf(stderr,"[state] unsafe or inaccessible persistent directory; refusing temporary identity\n");
    return "";
}
bool start_log(const char *name) {
    const std::string directory=data_directory();
    if(directory.empty())return false;
    std::string p = directory + "/" + name;
    log_path=p;
    if(rename(p.c_str(), (p+".previous").c_str()) && errno!=ENOENT)
        state_rejected(p,"rotate-log",errno);
    int parent=owned_directory(directory,false,false);
    if(parent<0)return false;
    int fd=owned_file_at(parent,name,O_WRONLY|O_CREAT,p);close(parent);
    if(fd<0)return false;
    if(ftruncate(fd,0)) {
        const int error=errno;close(fd);state_rejected(p,"truncate-log",error);return false;
    }
    if(dup2(fd,STDERR_FILENO)<0) {
        const int error=errno;close(fd);state_rejected(p,"redirect-stderr",error);return false;
    }
    close(fd);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    // Proprietary drivers emit high-volume frame diagnostics to stdout.
    // Keep application/errors on stderr, without filling app storage with them.
    int nullfd=open("/dev/null",O_WRONLY);
    if(nullfd>=0){dup2(nullfd,STDOUT_FILENO);close(nullfd);}
    return true;
}
void rotate_log() {
    struct stat s;
    if(!log_path.empty() && fstat(fileno(stderr),&s)==0 && s.st_size>1024*1024) {
        start_log(log_path.substr(log_path.rfind('/')+1).c_str());
    }
}
bool Shell::open() {
    signal(SIGTERM, signal_stop); signal(SIGINT, signal_stop);
    input_ = InputMapper(static_cast<SDL_Scancode>(SDL_WEBOS_SCANCODE_BACK),
                         static_cast<SDL_Scancode>(SDL_WEBOS_SCANCODE_EXIT));
    setenv("EGL_PLATFORM", "wayland", 0); setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_EXIT, "true");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "[SDL] %s\n",SDL_GetError()); return false; }
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(0,&mode) != 0) return false;
    window = SDL_CreateWindow(UXAIR_DISPLAY_NAME,0,0,mode.w,mode.h,SDL_WINDOW_FULLSCREEN);
    if (window) renderer = SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED);
    if (!renderer) { fprintf(stderr,"[SDL] %s\n",SDL_GetError()); return false; }
    fprintf(stderr,"[SDL] graphics-window=%dx%d (logical app coordinates)\n",mode.w,mode.h);
    if(TTF_Init()!=0) return false;
    font_=TTF_OpenFont("/usr/share/fonts/LG_Display-Regular.ttf",30);
    large_=TTF_OpenFont("/usr/share/fonts/LG_Display-Regular.ttf",76);
    small_=TTF_OpenFont("/usr/share/fonts/LG_Display-Regular.ttf",23);
    pin_font_=TTF_OpenFont("/usr/share/fonts/LG_Display-Regular.ttf",112);
    pin_font_small_=TTF_OpenFont("/usr/share/fonts/LG_Display-Regular.ttf",88);
    if(pin_font_)TTF_SetFontStyle(pin_font_,TTF_STYLE_BOLD);
    if(pin_font_small_)TTF_SetFontStyle(pin_font_small_,TTF_STYLE_BOLD);
    if(!font_||!large_||!small_||!pin_font_||!pin_font_small_) {fprintf(stderr,"[font] %s\n",TTF_GetError());return false;}
    np_title_=TTF_OpenFont("/usr/share/fonts/LG_Display-Regular.ttf",66);
    np_artist_=TTF_OpenFont("/usr/share/fonts/LG_Display-Regular.ttf",44);
    np_album_=TTF_OpenFont("/usr/share/fonts/LG_Display-Regular.ttf",34);
    if(np_title_)TTF_SetFontStyle(np_title_,TTF_STYLE_BOLD);
    if(!np_title_||!np_artist_||!np_album_)fprintf(stderr,"[font] now playing: %s\n",TTF_GetError());
    SDL_Surface *qr=SDL_LoadBMP((app_directory()+"/bmc-qr.bmp").c_str());
    if(qr) {donation_qr_=SDL_CreateTextureFromSurface(renderer,qr);SDL_FreeSurface(qr);}
    if(!donation_qr_)fprintf(stderr,"[donation] bundled QR image unavailable: %s\n",SDL_GetError());
    SDL_Surface *coffee=SDL_LoadBMP((app_directory()+"/bmc-icon.bmp").c_str());
    if(coffee) {donation_icon_=SDL_CreateTextureFromSurface(renderer,coffee);SDL_FreeSurface(coffee);}
    if(!donation_icon_)fprintf(stderr,"[donation] bundled coffee icon unavailable: %s\n",SDL_GetError());
    return true;
}
void Shell::transparent() {
    SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer,0,0,0,0); SDL_RenderClear(renderer); SDL_RenderPresent(renderer);
}
// SDL2_image, loaded at run time: a TV without it shows no cover art.
typedef SDL_Surface *(*load_image_fn)(SDL_RWops *, int);
static load_image_fn load_image() {
    static load_image_fn fn=nullptr;
    static bool tried=false;
    if(!tried) {
        tried=true;
        void *lib=dlopen("libSDL2_image-2.0.so.0",RTLD_NOW|RTLD_GLOBAL);
        fn=lib?reinterpret_cast<load_image_fn>(dlsym(lib,"IMG_Load_RW")):nullptr;
        if(!fn)fprintf(stderr,"[now-playing] SDL2_image unavailable, no cover art: %s\n",dlerror());
    }
    return fn;
}
static std::string clock_text(double seconds) {
    const int t=int(std::max(0.0,seconds)+0.5);
    char text[16];snprintf(text,sizeof text,"%d:%02d",t/60,t%60);return text;
}
// LG's UI font has no typographic quotes, dashes or ellipsis (drawn as boxes):
// track names often use them, so they become their plain forms.
static std::string plain_punctuation(std::string s) {
    static const char *const map[][2]={
        {"\xE2\x80\x98","'"},{"\xE2\x80\x99","'"},{"\xE2\x80\x9A","'"},{"\xE2\x80\x9B","'"},
        {"\xE2\x80\x9C","\""},{"\xE2\x80\x9D","\""},{"\xE2\x80\x9E","\""},
        {"\xE2\x80\x93","-"},{"\xE2\x80\x94","-"},{"\xE2\x80\xA6","..."},{"\xC2\xA0"," "}};
    for(const auto& m:map)
        for(size_t p;(p=s.find(m[0]))!=std::string::npos;)s.replace(p,strlen(m[0]),m[1]);
    return s;
}
// s, shortened with an ellipsis to max_width (whole UTF-8 characters)
static std::string fit_text(TTF_Font *font,const std::string& s,int max_width) {
    int w=0;
    if(!font||TTF_SizeUTF8(font,s.c_str(),&w,nullptr)||w<=max_width)return s;
    const std::string dots="...";
    std::string t=s;
    while(!t.empty()) {
        while(!t.empty()&&(static_cast<unsigned char>(t.back())&0xC0)==0x80)t.pop_back();
        if(!t.empty())t.pop_back();
        const std::string c=t+dots;
        if(!TTF_SizeUTF8(font,c.c_str(),&w,nullptr)&&w<=max_width)return c;
    }
    return dots;
}
bool Shell::now_playing(const NowPlayingView& v) {
    int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
    bool render_failed=false;
    TTF_Font *title_font=np_title_?np_title_:large_;
    TTF_Font *artist_font=np_artist_?np_artist_:font_;
    TTF_Font *album_font=np_album_?np_album_:small_;
    if(v.cover_generation!=cover_generation_) {
        if(cover_){SDL_DestroyTexture(cover_);cover_=nullptr;}
        cover_tint_=uxair_navy;
        if(v.cover && artwork_supported(v.cover->data(),v.cover->size())) {
            SDL_RWops *rw=SDL_RWFromConstMem(v.cover->data(),int(v.cover->size()));
            const load_image_fn load=load_image();
            SDL_Surface *image=rw&&load?load(rw,1):nullptr;
            if(rw&&!load)SDL_RWclose(rw);
            if(image && !artwork_dimensions(image->w,image->h)) {
                SDL_FreeSurface(image);image=nullptr;
                fprintf(stderr,"[now-playing] decoded cover exceeds pixel budget\n");
            }
            if(image) {
                // the cover's average colour tints the background
                SDL_Surface *rgba=SDL_ConvertSurfaceFormat(image,SDL_PIXELFORMAT_RGBA32,0);
                if(rgba&&!SDL_LockSurface(rgba)) {
                    uint64_t r=0,g=0,b=0,n=0;
                    for(int y=0;y<rgba->h;y+=std::max(1,rgba->h/32))
                        for(int x=0;x<rgba->w;x+=std::max(1,rgba->w/32)) {
                            const Uint8 *px=static_cast<const Uint8 *>(rgba->pixels)+y*rgba->pitch+x*4;
                            r+=px[0];g+=px[1];b+=px[2];++n;
                        }
                    SDL_UnlockSurface(rgba);
                    if(n)cover_tint_={Uint8(r/n),Uint8(g/n),Uint8(b/n),255};
                }
                if(rgba)SDL_FreeSurface(rgba);
                cover_=SDL_CreateTextureFromSurface(renderer,image);
                if(!cover_)fprintf(stderr,"[now-playing] cover texture: %s\n",SDL_GetError());
                SDL_FreeSurface(image);
            } else if(load) fprintf(stderr,"[now-playing] cover not decoded: %s\n",SDL_GetError());
        }
        cover_generation_=v.cover_generation;
    }
    const float b=std::max(0.f,std::min(1.f,v.brightness));
    auto dim=[&](SDL_Color c,float f){return SDL_Color{Uint8(c.r*b*f),Uint8(c.g*b*f),Uint8(c.b*b*f),c.a};};
    // Diagnostic B7 path: opaque solid colour, with no stretched gradient.
    if(SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE))render_failed=true;
    const SDL_Color background=dim(cover_tint_,0.22f);
    SDL_SetRenderDrawColor(renderer,background.r,background.g,background.b,255);
    if(SDL_RenderClear(renderer)) {
        static bool reported=false;
        if(!reported)fprintf(stderr,"[now-playing] clear: %s\n",SDL_GetError());
        reported=true;
    }
    if(SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND))render_failed=true;
    const int size=h*50/100, cx=w*12/100, cy=(h-size)/2;
    fill_round_rect(renderer,{cx+12,cy+18,size,size},18,{0,0,0,Uint8(120*b)});
    SDL_Rect art={cx,cy,size,size};
    if(cover_) {
        if(SDL_SetTextureColorMod(cover_,Uint8(255*b),Uint8(255*b),Uint8(255*b)))render_failed=true;
        if(SDL_RenderCopy(renderer,cover_,nullptr,&art))render_failed=true;
    } else {
        fill_round_rect(renderer,art,18,dim(uxair_surface,1));
        int lw=0,lh=0;TTF_SizeUTF8(title_font,"AirPlay",&lw,&lh);
        text_at("AirPlay",cx+(size-lw)/2,cy+(size-lh)/2,size,title_font,dim(uxair_accent,1));
    }
    const int tx=cx+size+90, tw=w-tx-w*8/100;
    int y=cy+size*12/100;
    const std::string source=plain_punctuation(v.source.empty()?"AirPlay on UxAir":"AirPlay from "+v.source);
    text_at(fit_text(small_,source,tw),tx,y,tw,small_,dim({150,175,190,255},1));
    y+=TTF_FontLineSkip(small_)+18;
    // The title scrolls when too wide: 3 s at the start, glides left until its
    // end shows, 5 s there, glides back, again.
    const std::string title=plain_punctuation(v.title.empty()?"Unknown title":v.title);
    if(title!=title_text_) {
        if(title_){SDL_DestroyTexture(title_);title_=nullptr;}
        SDL_Surface *s=TTF_RenderUTF8_Blended(title_font,title.c_str(),uxair_text);
        if(s){title_=SDL_CreateTextureFromSurface(renderer,s);title_w_=s->w;title_h_=s->h;SDL_FreeSurface(s);}
        if(!title_)fprintf(stderr,"[now-playing] title texture: %s\n",SDL_GetError());
        title_text_=title;title_since_=SDL_GetTicks();
    }
    bool animating=false;
    if(title_) {
        int offset=0;
        const int overflow=title_w_-tw;
        if(overflow>0) {
            animating=true;
            const double hold_start=3.0,out=std::max(1.0,overflow/90.0),hold_end=5.0,back=std::max(0.6,overflow/240.0);
            double t=std::fmod((SDL_GetTicks()-title_since_)/1000.0,hold_start+out+hold_end+back);
            auto ease=[](double x){return x*x*(3-2*x);};
            if(t<hold_start) offset=0;
            else if((t-=hold_start)<out) offset=int(overflow*ease(t/out));
            else if((t-=out)<hold_end) offset=overflow;
            else offset=int(overflow*(1-ease((t-hold_end)/back)));
        }
        if(SDL_SetTextureColorMod(title_,Uint8(255*b),Uint8(255*b),Uint8(255*b)))render_failed=true;
        SDL_Rect src={offset,0,std::min(title_w_,tw),title_h_}, dst={tx,y,src.w,title_h_};
        if(SDL_RenderCopy(renderer,title_,&src,&dst))render_failed=true;
    }
    y+=TTF_FontLineSkip(title_font)+8;
    if(!v.artist.empty()) {text_at(fit_text(artist_font,plain_punctuation(v.artist),tw),tx,y,tw,artist_font,dim({222,232,242,255},1));y+=TTF_FontLineSkip(artist_font)+6;}
    if(!v.album.empty()) text_at(fit_text(album_font,plain_punctuation(v.album),tw),tx,y,tw,album_font,dim({160,182,198,255},1));
    if(v.duration_s>0) {
        const int py=cy+size*82/100;
        const double fraction=std::max(0.0,std::min(1.0,v.elapsed_s/v.duration_s));
        fill_round_rect(renderer,{tx,py,tw,8},4,{255,255,255,Uint8(60*b)});
        if(fraction>0)fill_round_rect(renderer,{tx,py,std::max(8,int(tw*fraction)),8},4,dim(uxair_text,1));
        const std::string remaining="-"+clock_text(v.duration_s-v.elapsed_s);
        int rw=0;TTF_SizeUTF8(small_,remaining.c_str(),&rw,nullptr);
        text_at(clock_text(v.elapsed_s),tx,py+22,tw/2,small_,dim({170,190,205,255},1));
        text_at(remaining,tx+tw-rw,py+22,rw,small_,dim({170,190,205,255},1));
    }
    if(render_failed) {
        static bool reported=false;
        if(!reported)fprintf(stderr,"[now-playing] texture/blend rendering: %s\n",SDL_GetError());
        reported=true;
    }
    SDL_RenderPresent(renderer);
    return animating;
}
Action Shell::action() {
    if(interrupted)return Action::Quit;
    SDL_Event e;
    static Uint32 last_motion_log_ms = 0;
    pointer_select_ = false;
    auto update_pointer=[&](int x,int y) {if(!pointer_valid_||x!=pointer_x_||y!=pointer_y_)pointer_moved_=true;pointer_x_=x;pointer_y_=y;pointer_valid_=true;};
    while (SDL_PollEvent(&e)) {
        const Uint32 now = SDL_GetTicks();
        if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            if (!e.key.repeat)
                fprintf(stderr,"[input] key %s keycode=%ld scancode=%d repeat=%u\n",
                        e.type == SDL_KEYDOWN ? "down" : "up",
                        static_cast<long>(e.key.keysym.sym),
                        static_cast<int>(e.key.keysym.scancode), e.key.repeat);
        } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
            update_pointer(e.button.x,e.button.y);
            fprintf(stderr,"[input] mouse %s button=%u clicks=%u x=%d y=%d\n",
                    e.type == SDL_MOUSEBUTTONDOWN ? "down" : "up",
                    e.button.button, e.button.clicks, e.button.x, e.button.y);
        } else if (e.type == SDL_MOUSEMOTION &&
                   (last_motion_log_ms == 0 || Uint32(now - last_motion_log_ms) >= 5000)) {
            update_pointer(e.motion.x,e.motion.y);
            fprintf(stderr,"[input] pointer motion x=%d y=%d buttons=%u\n",
                    e.motion.x, e.motion.y, e.motion.state);
            last_motion_log_ms = now;
        } else if(e.type==SDL_MOUSEMOTION) update_pointer(e.motion.x,e.motion.y);
        const Action mapped = input_.map(e, now);
        if (mapped != Action::None) {
            if (mapped==Action::Select && e.type==SDL_MOUSEBUTTONDOWN) {
                pointer_select_=true;update_pointer(e.button.x,e.button.y);
            }
            return mapped;
        }
    }
    return interrupted?Action::Quit:Action::None;
}
bool Shell::pointer_select(int& x, int& y) const {
    if (!pointer_select_) return false;
    x=pointer_x_;y=pointer_y_;return true;
}
bool Shell::take_pointer_motion() {bool moved=pointer_moved_;pointer_moved_=false;return moved;}
static int dialog_first(const UiDialog& dialog) {
    int count=int(dialog.choices.size()),visible=std::min(5,count);
    return std::max(0,std::min(dialog.selected-2,count-visible));
}
UiHit Shell::hit_test(int x, int y, int row_count, const UiDialog *dialog, int selected, UiPage page_id) const {
    int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
    if(dialog) {
        int visible=std::min(5,int(dialog->choices.size()));
        int box_w=std::min(w*68/100,1050),box_h=150+visible*72;
        int left=(w-box_w)/2,top=(h-box_h)/2;
        if(x<left || x>=left+box_w || y<top+124 || y>=top+124+visible*72)
            return {UiHitKind::Cancel,-1};
        return {UiHitKind::DialogChoice,dialog_first(*dialog)+(y-top-124)/72};
    }
    int top=(page_id==UiPage::System?h/5:h/4),row_height=std::max(66,h/11),sidebar=w/4;
    if(x>=0 && x<sidebar && y>=top && y<top+5*row_height)
        return {UiHitKind::Category,(y-top)/row_height};
    if(x>=sidebar+24 && x<w-24) {
        if(page_id==UiPage::History) {
            const HistoryUiLayout layout=history_ui_layout(w,h);
            const int count=std::max(0,row_count-1);
            const int session=history_session_hit(layout,x,y,count,selected);
            if(session>=0)return {UiHitKind::Row,session};
            if(history_reset_hit(layout,x,y))return {UiHitKind::Row,row_count-1};
            return {};
        }
        int visible=std::min(row_count,7),first=visible_row_first(selected,row_count,visible);
        if(y>=top && y<top+visible*row_height) return {UiHitKind::Row,first+(y-top)/row_height};
    }
    return {};
}
bool Shell::pump() {Action a=action();return a!=Action::Back&&a!=Action::Exit&&a!=Action::Quit;}
void Shell::text(const std::string& s,int y,TTF_Font *font,SDL_Color color) {
    SDL_Surface *surface=TTF_RenderUTF8_Blended(font,s.c_str(),color);if(!surface)return;
    SDL_Texture *texture=SDL_CreateTextureFromSurface(renderer,surface);
    int w,h;SDL_GetRendererOutputSize(renderer,&w,&h);
    int width=std::min(surface->w,w-80),height=surface->h;
    SDL_Rect rect={(w-width)/2,y,width,height};
    if(texture) {SDL_RenderCopy(renderer,texture,nullptr,&rect);SDL_DestroyTexture(texture);}
    SDL_FreeSurface(surface);
}
void Shell::text_at(const std::string& s,int x,int y,int max_width,TTF_Font *font,SDL_Color color) {
    SDL_Surface *surface=TTF_RenderUTF8_Blended(font,s.c_str(),color);if(!surface)return;
    SDL_Texture *texture=SDL_CreateTextureFromSurface(renderer,surface);
    int width=std::min(surface->w,max_width);
    SDL_Rect source={0,0,width,surface->h};
    SDL_Rect rect={x,y,width,surface->h};
    if(texture) { SDL_RenderCopy(renderer,texture,&source,&rect); SDL_DestroyTexture(texture); }
    SDL_FreeSurface(surface);
}
void Shell::draw_history(const UiHistoryData& data,const std::vector<UiRow>& rows,int selected,int panel,int w,int h) {
    const HistoryUiLayout layout=history_ui_layout(w,h);
    const int content=layout.content_width,top=layout.card_top,card_gap=16,card_h=74;
    auto card=[&](int x,int y,int cw,const char *label,const std::string& value) {
        SDL_Rect r={x,y,cw,card_h};fill_round_rect(renderer,r,12,uxair_navy);
        text_at(label,x+16,y+5,cw-32,small_,{157,184,201,255});
        text_at(value,x+16,y+36,cw-32,font_,uxair_text);
    };
    const int third=(content-card_gap*2)/3;
    card(panel,top,third,"LIFETIME SESSIONS",data.summary);
    card(panel+third+card_gap,top,third,"LIFETIME PLAYBACK",data.total_time);
    card(panel+2*(third+card_gap),top,third,"AVERAGE SESSION",data.average);
    const int second_y=top+86;
    card(panel,second_y,content/2-card_gap/2,"LAST 30 DAYS - SCREEN MIRRORING",data.mirror_time);
    card(panel+content/2+card_gap/2,second_y,content/2-card_gap/2,"LAST 30 DAYS - AIRPLAY MUSIC",data.music_time);
    auto draw_chart=[&](int x,int y,int cw,int ch,const std::vector<UiHistoryBar>& bars,const std::string& title,bool monthly) {
        fill_round_rect(renderer,{x,y,cw,ch},12,{25,39,61,255});
        uint64_t maximum=0;
        for(const auto& b:bars)if(b.available) {
            const uint64_t total=b.audio_ms>UINT64_MAX-b.mirror_ms?UINT64_MAX:b.mirror_ms+b.audio_ms;
            maximum=std::max(maximum,total);
        }
        const uint64_t axis=usage_chart_axis(maximum);
        const uint64_t unit=maximum>=3600000?3600000:maximum>=60000?60000:1000;
        const int text_h=TTF_FontHeight(small_),padding=16;
        text_at(title,x+padding,y+8,cw-2*padding,small_,uxair_text);
        // Reserve two label rows; one shared legend below the cards explains both charts.
        int axis_width=0;
        for(int tick=0;tick<=2;++tick) {
            int tw=0;TTF_SizeUTF8(small_,usage_axis_label(tick==2?axis:tick?axis/2:0,unit).c_str(),&tw,nullptr);
            axis_width=std::max(axis_width,tw);
        }
        const int px=x+padding+axis_width+10,py=y+text_h+26,pw=cw-(px-x)-padding;
        const int base=y+ch-2*text_h-12,plot_h=std::max(1,base-py);
        for(int tick=0;tick<=2;++tick) {
            const int gy=base-plot_h*tick/2;
            SDL_SetRenderDrawColor(renderer,54,70,88,255);SDL_RenderDrawLine(renderer,px,gy,px+pw-1,gy);
            text_at(usage_axis_label(tick==2?axis:tick?axis/2:0,unit),x+padding,gy-text_h/2,axis_width,small_,{157,178,194,255});
        }
        const int n=int(bars.size());
        SDL_Rect previous_clip;SDL_RenderGetClipRect(renderer,&previous_clip);
        const bool had_clip=SDL_RenderIsClipEnabled(renderer)==SDL_TRUE;
        const SDL_Rect plot={px,py,pw,plot_h};SDL_RenderSetClipRect(renderer,&plot);
        for(int i=0;i<n;++i) {
            const auto& b=bars[size_t(i)];
            const int left=px+pw*i/n,right=px+pw*(i+1)/n;
            const int bw=std::max(1,std::min(right-left-3,monthly?22:14)),bx=(left+right-bw)/2;
            if(b.future)continue;
            if(!b.available) {SDL_SetRenderDrawColor(renderer,130,145,159,255);SDL_RenderDrawLine(renderer,bx,base-2,bx+bw-1,base-2);continue;}
            const UsageBarHeights heights=usage_bar_heights(b.mirror_ms,b.audio_ms,axis,plot_h);
            const int mh=heights.mirror,ah=heights.audio;
            SDL_SetRenderDrawColor(renderer,101,185,246,255);SDL_Rect mr={bx,base-mh,bw,mh};if(mh)SDL_RenderFillRect(renderer,&mr);
            SDL_SetRenderDrawColor(renderer,104,194,157,255);SDL_Rect ar={bx,base-mh-ah,bw,ah};if(ah)SDL_RenderFillRect(renderer,&ar);
            if(b.partial)text_at("*",(left+right)/2-6,base-text_h,bw+12,small_,uxair_text);
            if(!mh&&!ah) {SDL_SetRenderDrawColor(renderer,174,194,207,255);SDL_Rect zero={(left+right)/2-1,base-2,3,2};SDL_RenderFillRect(renderer,&zero);}
        }
        SDL_RenderSetClipRect(renderer,had_clip?&previous_clip:nullptr);
        // Labels are independent of bar availability, so Jan/Dec and day 1/end never disappear.
        for(int i=0;i<n;++i) {
            const auto& b=bars[size_t(i)];
            if(!monthly&&!b.current&&i!=0&&(i+1)%5!=0&&i!=n-1)continue;
            if(!monthly&&!b.current&&i!=0&&i!=n-1&&
               ((i>0&&bars[size_t(i-1)].current)||(i+1<n&&bars[size_t(i+1)].current)))continue;
            // Avoid adjacent day 30/31 labels. The final calendar day takes priority.
            if(!monthly&&!b.current&&i==n-2&&i!=0)continue;
            const std::string label=b.label;
            int tw=0;TTF_SizeUTF8(small_,label.c_str(),&tw,nullptr);
            const int center=px+pw*(2*i+1)/(2*n);
            const int tx=std::max(px,std::min(center-tw/2,x+cw-padding-tw));
            const bool second_label_row=monthly?i%2!=0:b.current&&(i==1||i==n-2);
            const int label_y=base+7+(second_label_row?text_h+1:0);
            text_at(label,tx,label_y,x+cw-padding-tx,small_,b.current?uxair_accent:b.future?SDL_Color{116,133,148,255}:SDL_Color{171,190,204,255});
        }
    };
    const int chart_y=layout.chart_y,chart_h=layout.chart_h;
    draw_chart(panel,chart_y,layout.daily_width,chart_h,data.daily_bars,data.daily_title,false);
    const int month_x=panel+layout.daily_width+layout.chart_gap;
    draw_chart(month_x,chart_y,content-layout.daily_width-layout.chart_gap,chart_h,data.monthly_bars,data.monthly_title,true);
    text_at("UTC · Blue label: current · * Partial history · — Unavailable · • Known zero · Blank: future",panel,chart_y+chart_h+7,content,small_,{157,178,194,255});
    if(data.mirror_percent+data.audio_percent) {
        text_at("Last 30 days - playback distribution",panel,layout.distribution_y,content/2,small_,uxair_text);
        text_at("Screen mirroring  "+std::to_string(data.mirror_percent)+"%",panel+content/2,layout.distribution_y,content/4,small_,uxair_accent);
        text_at("AirPlay music  "+std::to_string(data.audio_percent)+"%",panel+content*3/4,layout.distribution_y,content/4,small_,{145,211,181,255});
    } else text_at("No playback yet",panel,layout.distribution_y,content,small_,{174,194,207,255});
    const int dist_h=20;
    SDL_Rect track={panel,layout.distribution_bar_y,content,dist_h};SDL_SetRenderDrawColor(renderer,47,65,85,255);SDL_RenderFillRect(renderer,&track);
    const unsigned total_pct=data.mirror_percent+data.audio_percent;
    const int mw=int(content*data.mirror_percent/100);
    SDL_SetRenderDrawColor(renderer,101,185,246,255);SDL_Rect m={panel,layout.distribution_bar_y,mw,dist_h};if(mw)SDL_RenderFillRect(renderer,&m);
    SDL_SetRenderDrawColor(renderer,104,194,157,255);SDL_Rect a={panel+mw,layout.distribution_bar_y,content-mw,dist_h};if(total_pct&&a.w)SDL_RenderFillRect(renderer,&a);
    const int list_y=layout.list_y,row_h=layout.row_h;
    text_at("Recent sessions",panel,layout.heading_y,content/2,font_,uxair_text);
    text_at("STARTED (UTC)",panel+10,layout.heading_y+36,content*27/100,small_,{157,184,201,255});
    text_at("DURATION",panel+content*30/100,layout.heading_y+36,content*15/100,small_,{157,184,201,255});
    text_at("SESSION",panel+content*48/100,layout.heading_y+36,content*50/100,small_,{157,184,201,255});
    const int count=std::max(0,int(rows.size())-1),visible=std::min(5,count),first=visible_row_first(std::max(0,selected),count,visible);
    for(int i=0;i<visible;++i){int idx=first+i,y=list_y+i*row_h;
        if(selected==idx){SDL_Rect f={panel-5,y-3,content,row_h-1};SDL_SetRenderDrawColor(renderer,uxair_surface.r,uxair_surface.g,uxair_surface.b,255);SDL_RenderFillRect(renderer,&f);}
        const auto& r=rows[size_t(idx)];text_at(r.label,panel+10,y+4,content*27/100,small_,uxair_text);text_at(r.value,panel+content*30/100,y+4,content*15/100,small_,uxair_accent);text_at(r.note,panel+content*48/100,y+4,content*50/100,small_,{174,194,207,255});
    }
    if(count==0)text_at("No playback history yet.",panel+10,list_y+5,content-20,font_,{174,194,207,255});
    int reset_index=int(rows.size())-1,reset_y=layout.reset_y;
    SDL_Rect reset={layout.reset_x,reset_y-4,layout.reset_w,layout.reset_h};if(selected==reset_index|| (pointer_valid_&&history_reset_hit(layout,pointer_x_,pointer_y_))){fill_round_rect(renderer,reset,8,{48,78,110,255});}
    const SDL_Color reset_border=selected==reset_index?uxair_accent:SDL_Color{90,112,132,255};
    SDL_SetRenderDrawColor(renderer,reset_border.r,reset_border.g,reset_border.b,255);SDL_RenderDrawRect(renderer,&reset);
    text_at("Reset history",panel+10,reset_y,160,small_,selected==reset_index?uxair_accent:uxair_text);
    text_at(data.storage,panel+200,reset_y,w-panel-250,small_,{174,194,207,255});
}
void Shell::page(UiPage page_id,const std::string& subtitle,const std::vector<UiRow>& rows,int selected,const std::string& footer,const UiDialog *dialog,const std::string& pin,bool donation_open,const UiHistoryData *history) {
    int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
    SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer,uxair_background.r,uxair_background.g,uxair_background.b,255);SDL_RenderClear(renderer);
    const int sidebar=w/4,panel=sidebar+32;
    SDL_Rect side={0,0,sidebar,h};
    SDL_SetRenderDrawColor(renderer,uxair_navy.r,uxair_navy.g,uxair_navy.b,255);SDL_RenderFillRect(renderer,&side);
    SDL_Rect accent={0,0,w,7};
    SDL_SetRenderDrawColor(renderer,uxair_accent.r,uxair_accent.g,uxair_accent.b,255);SDL_RenderFillRect(renderer,&accent);
    const char *categories[]={"Video","Audio","History","Diagnostics","About"};
    const char *titles[]={"Video","Audio","History","Diagnostics","About"};
    int index=int(page_id);if(index<0||index>4)index=0;
    int category=index;
    text_at("UxAir",w/28,h/18,sidebar-40,large_,uxair_accent);
    text_at("AirPlay Receiver",w/28,h/8,sidebar-40,small_,{154,185,202,255});
    text_at(titles[index],panel,h/20,w-panel-35,large_,{239,247,249,255});
    if(page_id!=UiPage::History&&page_id!=UiPage::System)
        text_at(subtitle,panel,h/7,w-panel-35,font_,{154,185,202,255});
    const int top=h/4;
    const int row_height=std::max(66,h/11);
    for(int i=0;i<5;++i) {
        int y=top+i*row_height;
        if(i==category) {
            SDL_Rect tab={14,y-5,sidebar-28,row_height-6};
            SDL_SetRenderDrawColor(renderer,uxair_surface.r,uxair_surface.g,uxair_surface.b,255);SDL_RenderFillRect(renderer,&tab);
            SDL_Rect marker={14,y-5,6,row_height-6};
            SDL_SetRenderDrawColor(renderer,uxair_accent.r,uxair_accent.g,uxair_accent.b,255);SDL_RenderFillRect(renderer,&marker);
        }
        text_at(categories[i],30,y+12,sidebar-52,font_,i==category?SDL_Color{238,249,247,255}:SDL_Color{150,175,190,255});
    }
    if(page_id==UiPage::History && history) {
        draw_history(*history,rows,selected,panel,w,h);
    } else {
        const size_t visible_rows=std::min<size_t>(rows.size(),7);
        const int row_top=page_id==UiPage::System?h/5:top;
        const int first_row=visible_row_first(selected,int(rows.size()),int(visible_rows));
        for(size_t slot=0;slot<visible_rows;++slot) {
        size_t i=size_t(first_row)+slot;int y=row_top+int(slot)*row_height;
        if(int(i)==selected) {
            SDL_Rect focus={panel-8,y-6,w-panel-24,row_height-5};
            SDL_SetRenderDrawColor(renderer,uxair_surface.r,uxair_surface.g,uxair_surface.b,255);SDL_RenderFillRect(renderer,&focus);
            SDL_Rect marker={panel-8,y-6,6,row_height-5};
            SDL_SetRenderDrawColor(renderer,uxair_accent.r,uxair_accent.g,uxair_accent.b,255);SDL_RenderFillRect(renderer,&marker);
        }
        const SDL_Color primary=rows[i].enabled?uxair_text:SDL_Color{159,174,194,255};
        const bool donation_row=page_id==UiPage::System&&i==3&&donation_icon_;
        int icon_size=48,icon_y=y+(row_height-48)/2;
        if(donation_row) {
            int title_h=0,note_h=0;
            TTF_SizeUTF8(font_,rows[i].label.c_str(),nullptr,&title_h);
            TTF_SizeUTF8(small_,rows[i].note.c_str(),nullptr,&note_h);
            const int text_top=y+4,text_bottom=std::max(y+4+title_h,y+43+note_h),text_height=text_bottom-text_top;
            icon_size=std::max(48,std::min(68,text_height));
            icon_y=text_top+(text_height-icon_size)/2;
        }
        const int label_x=donation_row?panel+18+icon_size+16:panel+18;
        text_at(rows[i].label,label_x,y+4,w-label_x-35,font_,primary);
        if(!rows[i].value.empty())text_at(rows[i].value,panel+(w-panel)*65/100,y+4,(w-panel)*30/100,font_,int(i)==selected?uxair_accent:primary);
        if(!rows[i].note.empty())text_at(rows[i].note,label_x,y+43,w-label_x-48,small_,{151,178,194,255});
        if(donation_row) {
            SDL_Rect icon={panel+18,icon_y,icon_size,icon_size};SDL_RenderCopy(renderer,donation_icon_,nullptr,&icon);
        }
        }
    }
    if(page_id!=UiPage::History)text_at(footer,panel,h-54,w-panel-35,small_,{152,180,194,255});
    if(dialog) {
        SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer,0,5,10,190);
        SDL_Rect dim={0,0,w,h};SDL_RenderFillRect(renderer,&dim);
        int visible=std::min(5,int(dialog->choices.size()));
        int box_w=std::min(w*68/100,1050),box_h=150+visible*72;
        int left=(w-box_w)/2,dialog_top=(h-box_h)/2;
        SDL_SetRenderDrawColor(renderer,uxair_navy.r,uxair_navy.g,uxair_navy.b,255);
        SDL_Rect box={left,dialog_top,box_w,box_h};SDL_RenderFillRect(renderer,&box);
        text_at(dialog->title,left+32,dialog_top+18,box_w-64,font_,uxair_text);
        text_at(dialog->description,left+32,dialog_top+68,box_w-64,small_,{164,191,207,255});
        int first=dialog_first(*dialog);
        for(int i=0;i<visible;++i) {
            int option=first+i,y=dialog_top+124+i*72;
            if(option==dialog->selected) {
                SDL_Rect selected_row={left+20,y-3,box_w-40,67};
                SDL_SetRenderDrawColor(renderer,uxair_surface.r,uxair_surface.g,uxair_surface.b,255);SDL_RenderFillRect(renderer,&selected_row);
            }
            text_at(dialog->choices[option],left+42,y+12,box_w-120,font_,option==dialog->selected?uxair_text:SDL_Color{184,205,216,255});
            if(option==dialog->current)text_at("✓",left+box_w-94,y+12,60,small_,uxair_accent);
        }
    }
    if(!pin.empty())draw_pairing_modal(pin,w,h);
    else if(donation_open)draw_donation_modal(w,h);
    SDL_RenderPresent(renderer);
}
void Shell::overlay(const std::vector<std::string>& lines,const std::string& pin) {
    SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer,0,0,0,0);SDL_RenderClear(renderer);
    int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
    if(!lines.empty()) {
        int x=w/30,y=h/25,box_w=std::min(w*3/5,950),box_h=34+int(lines.size())*38;
        SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer,8,18,28,208);
        SDL_Rect box={x,y,box_w,box_h};SDL_RenderFillRect(renderer,&box);
        for(size_t i=0;i<lines.size();++i)
            text_at(lines[i],x+20,y+15+int(i)*38,box_w-40,small_,{239,249,246,255});
    }
    if(!pin.empty())draw_pairing_modal(pin,w,h);
    SDL_RenderPresent(renderer);
}
void Shell::draw_donation_modal(int w,int h) {
    SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer,0,4,10,184);SDL_Rect backdrop={0,0,w,h};SDL_RenderFillRect(renderer,&backdrop);
    int qr_size=std::min(500,std::min(w*45/100,h*65/100));
    int top=std::max(20,(h-(qr_size+178))/2);
    int panel_w=std::min(w-40,std::max(560,qr_size+120));
    SDL_Rect panel={(w-panel_w)/2,top,panel_w,qr_size+150};
    fill_round_rect(renderer,panel,26,{29,45,71,255});
    const std::string heading="Enjoying the app? Buy me a coffee.";
    int heading_width=0;TTF_SizeUTF8(font_,heading.c_str(),&heading_width,nullptr);
    text_at(heading,(w-heading_width)/2,top+24,w-80,font_,uxair_text);
    if(donation_qr_) {
        SDL_Rect qr={(w-qr_size)/2,top+70,qr_size,qr_size};
        SDL_RenderCopy(renderer,donation_qr_,nullptr,&qr);
    } else {
        text("QR image unavailable",top+qr_size/2,large_,{255,255,255,255});
    }
    const std::string url="buymeacoffee.com/depius";
    int url_width=0;TTF_SizeUTF8(font_,url.c_str(),&url_width,nullptr);
    text_at(url,(w-url_width)/2,top+qr_size+82,w-60,font_,uxair_accent);
    const std::string hint="Back to return";
    int hint_width=0;TTF_SizeUTF8(small_,hint.c_str(),&hint_width,nullptr);
    text_at(hint,(w-hint_width)/2,h-48,w-40,small_,{160,184,198,255});
}
void Shell::draw_pairing_modal(const std::string& pin,int w,int h) {
    SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer,0,3,8,190);SDL_Rect backdrop={0,0,w,h};SDL_RenderFillRect(renderer,&backdrop);
    int box_w=std::min(w*82/100,1120),box_h=std::min(h*62/100,500);
    SDL_Rect panel={(w-box_w)/2,(h-box_h)/2,box_w,box_h};
    fill_round_rect(renderer,panel,28,{29,45,71,255});
    const std::string heading="AirPlay Pairing";
    int heading_width=0;TTF_SizeUTF8(large_,heading.c_str(),&heading_width,nullptr);
    text_at(heading,panel.x+(box_w-heading_width)/2,panel.y+box_h/8,box_w-64,large_,uxair_accent);
    const std::string instruction="Enter this code on your Apple device";
    int instruction_width=0;TTF_SizeUTF8(font_,instruction.c_str(),&instruction_width,nullptr);
    text_at(instruction,panel.x+(box_w-instruction_width)/2,panel.y+box_h*37/100,box_w-64,font_,{225,237,243,255});
    std::string spaced;
    for(size_t i=0;i<pin.size();++i) {if(i)spaced+="  ";spaced+=pin[i];}
    TTF_Font *display_pin_font=pin_font_;
    int pin_width=0;TTF_SizeUTF8(display_pin_font,spaced.c_str(),&pin_width,nullptr);
    if(pin_width>box_w*84/100) {display_pin_font=pin_font_small_;TTF_SizeUTF8(display_pin_font,spaced.c_str(),&pin_width,nullptr);}
    const int pin_x=panel.x+(box_w-pin_width)/2;
    text_at(spaced,pin_x,panel.y+box_h*52/100,box_w-64,display_pin_font,{255,255,255,255});
    const std::string hint="Back to cancel pairing";
    int hint_width=0;TTF_SizeUTF8(small_,hint.c_str(),&hint_width,nullptr);
    text_at(hint,panel.x+(box_w-hint_width)/2,panel.y+box_h-50,box_w-64,small_,{190,208,225,255});
}
void Shell::screen(const std::string& status,const std::string& pin,int delay) {
    SDL_SetRenderDrawColor(renderer,uxair_background.r,uxair_background.g,uxair_background.b,255);SDL_RenderClear(renderer);
    int w,h;SDL_GetRendererOutputSize(renderer,&w,&h);
    text(UXAIR_DISPLAY_NAME,h/6,large_,uxair_accent);
    text(status,h/2-60,font_,uxair_text);
    if(!pin.empty()) text(pin,h/2+10,large_,uxair_text);
    else text(std::string("Open Screen Mirroring and select ")+UXAIR_DISPLAY_NAME,h/2+20,font_,{178,194,207,255});
    char sync[160];snprintf(sync,sizeof(sync),"◀ ▶  Audio delay: %+d ms  ·  Adjust before connecting",delay);
    text(sync,h-135,font_,{145,167,185,255});
    text("Same Wi-Fi network  ·  Quality in Settings  ·  Back to exit",h-80,font_,{145,167,185,255});
    SDL_RenderPresent(renderer);
}
Shell::~Shell() {if(cover_)SDL_DestroyTexture(cover_);if(title_)SDL_DestroyTexture(title_);if(np_title_)TTF_CloseFont(np_title_);if(np_artist_)TTF_CloseFont(np_artist_);if(np_album_)TTF_CloseFont(np_album_);if(donation_icon_)SDL_DestroyTexture(donation_icon_);if(donation_qr_)SDL_DestroyTexture(donation_qr_);if(font_)TTF_CloseFont(font_);if(large_)TTF_CloseFont(large_);if(small_)TTF_CloseFont(small_);if(pin_font_)TTF_CloseFont(pin_font_);if(pin_font_small_)TTF_CloseFont(pin_font_small_);TTF_Quit();if(renderer) SDL_DestroyRenderer(renderer); if(window) SDL_DestroyWindow(window); SDL_Quit(); }
