#include "shell.h"
#include <cassert>
#include <csignal>
#include <cstdlib>
int main(int argc,char **argv) {
    // No TV/font required. open() installs handlers before SDL initialization.
    setenv("SDL_VIDEODRIVER","dummy",1);
    Shell shell;shell.open();
    raise(argc>1 && argv[1][0]=='i'?SIGINT:SIGTERM);
    assert(shell.action()==Action::Quit);
    assert(!shell.pump());
}
