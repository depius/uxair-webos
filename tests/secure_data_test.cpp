#include "secure_data.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
int main() {
    char pattern[]="build/tests/secure-state-XXXXXX";
    char *created=mkdtemp(pattern);assert(created);
    std::string root=created,state=root+"/state",file=state+"/settings-v1",link=root+"/alias";
    const std::string lib=root+"/var-lib";
    assert(select_state_root(root,lib,false)==root);
    assert(select_state_root(root,lib,true)==lib);
    assert(!mkdir(lib.c_str(),0700));assert(!mkdir((lib+"/state").c_str(),0700));
    assert(select_state_root(root,lib,false)==lib);
    assert(prepare_state(root,false));
    assert(select_state_root(root,lib,false)==root); // Existing local identity wins.
    rmdir((lib+"/state").c_str());rmdir(lib.c_str());
    int fd=open(file.c_str(),O_CREAT|O_WRONLY,0666);assert(fd>=0);close(fd);
    chmod(state.c_str(),0777);assert(prepare_state(root,false));
    struct stat s;assert(!stat(state.c_str(),&s)&&(s.st_mode&0777)==0700);
    assert(!stat(file.c_str(),&s)&&(s.st_mode&0777)==0600);
    assert(!symlink("state",link.c_str()));assert(owned_directory(link,false,true)<0);unlink(link.c_str());
    unlink(file.c_str());assert(!symlink("../victim",file.c_str()));assert(!prepare_state(root,false));unlink(file.c_str());
    assert(!mkfifo(file.c_str(),0600));assert(!prepare_state(root,false));unlink(file.c_str());
    fd=open(file.c_str(),O_CREAT|O_WRONLY,0600);assert(fd>=0);close(fd);
    assert(!::link(file.c_str(),link.c_str()));assert(!prepare_state(root,false));unlink(link.c_str());unlink(file.c_str());
    rmdir(state.c_str());assert(!symlink("missing",state.c_str()));assert(select_state_root(root,lib,false)==root);assert(!prepare_state(root,false));unlink(state.c_str());
    assert(!rmdir(root.c_str()));puts("PASS: state permissions, symlinks, hardlinks and FIFO rejection");
}
