#include "input.h"
#include <cassert>
#include <cstdio>

static SDL_Event key(SDL_Scancode scan, SDL_Keycode code = SDLK_UNKNOWN,
                     Uint8 repeat = 0, Uint32 type = SDL_KEYDOWN) {
    SDL_Event e = {};
    e.type = type;
    e.key.keysym.scancode = scan;
    e.key.keysym.sym = code;
    e.key.repeat = repeat;
    return e;
}
static SDL_Event button(Uint8 which, Uint32 type = SDL_MOUSEBUTTONDOWN) {
    SDL_Event e = {};
    e.type = type;
    e.button.button = which;
    return e;
}

int main() {
    // Integer values from the pinned webOS SDK's SDL_webOS.h.
    const SDL_Scancode back = static_cast<SDL_Scancode>(482);
    const SDL_Scancode exit = static_cast<SDL_Scancode>(505);
    InputMapper mapper(back, exit);
    assert(mapper.map(key(SDL_SCANCODE_UP), 1) == Action::Up);
    assert(mapper.map(key(SDL_SCANCODE_DOWN), 2) == Action::Down);
    assert(mapper.map(key(SDL_SCANCODE_LEFT), 3) == Action::Left);
    assert(mapper.map(key(SDL_SCANCODE_RIGHT), 4) == Action::Right);
    assert(mapper.map(key(back), 5) == Action::Back);
    assert(mapper.map(key(exit), 6) == Action::Exit);
    assert(mapper.map(key(SDL_SCANCODE_ESCAPE), 7) == Action::Back);
    assert(mapper.map(key(SDL_SCANCODE_AC_BACK), 8) == Action::Back);
    assert(mapper.map(key(SDL_SCANCODE_RETURN), 1000) == Action::Select);
    assert(mapper.map(button(SDL_BUTTON_LEFT), 1020) == Action::None); // paired wheel event
    assert(mapper.map(button(SDL_BUTTON_LEFT, SDL_MOUSEBUTTONUP), 1030) == Action::None);
    assert(mapper.map(button(SDL_BUTTON_LEFT), 1040) == Action::None);
    assert(mapper.map(button(SDL_BUTTON_LEFT), 1200) == Action::Select);
    assert(mapper.map(key(SDL_SCANCODE_KP_ENTER), 1220) == Action::None);
    assert(mapper.map(key(SDL_SCANCODE_KP_ENTER), 1400) == Action::Select);
    assert(mapper.map(key(SDL_SCANCODE_SELECT), 1600) == Action::Select);
    assert(mapper.map(key(SDL_SCANCODE_UNKNOWN, SDLK_RETURN), 1800) == Action::Select);
    assert(mapper.map(key(SDL_SCANCODE_UNKNOWN, SDLK_SELECT), 1900) == Action::Select);
    assert(mapper.map(key(SDL_SCANCODE_RETURN, SDLK_RETURN, 1), 2000) == Action::None);
    assert(mapper.map(button(SDL_BUTTON_RIGHT), 2100) == Action::None);
    assert(mapper.map(button(SDL_BUTTON_MIDDLE), 2101) == Action::None);
    SDL_Event motion = {}; motion.type = SDL_MOUSEMOTION;
    assert(mapper.map(motion, 2102) == Action::None);
    assert(mapper.map(key(SDL_SCANCODE_RETURN, SDLK_RETURN, 0, SDL_KEYUP), 2103) == Action::None);
    // Two genuine mouse clicks can be fast; cross-source duplicates cannot.
    assert(mapper.map(button(SDL_BUTTON_LEFT), 2200) == Action::Select);
    assert(mapper.map(button(SDL_BUTTON_LEFT), 2250) == Action::Select);
    SDL_Event quit = {}; quit.type = SDL_QUIT;
    assert(mapper.map(quit, 2300) == Action::Quit);
    std::puts("PASS: SDL input actions, remote click and cross-source deduplication");
}
