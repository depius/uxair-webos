#pragma once
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Platform-owned ancestors must be trusted. Never follow the application's
// data/state leaf or an existing state/log file through a symlink.
inline int owned_directory(const std::string& path,bool create,bool private_mode) {
    if(create && mkdir(path.c_str(),0700) && errno!=EEXIST)return -1;
    int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat s;
    if(fstat(fd,&s) || !S_ISDIR(s.st_mode) || s.st_uid!=geteuid() ||
       fchmod(fd,private_mode?0700:(s.st_mode&0755))) {close(fd);return -1;}
    return fd;
}
inline int owned_file_at(int directory,const char *name,int flags) {
    // O_NONBLOCK avoids hanging on an attacker-created FIFO before fstat.
    int fd=openat(directory,name,flags|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if(fd<0)return -1;
    struct stat s;
    if(fstat(fd,&s) || !S_ISREG(s.st_mode) || s.st_uid!=geteuid() || s.st_nlink!=1 || fchmod(fd,0600)) {
        close(fd);return -1;
    }
    return fd;
}
inline bool prepare_state(const std::string& root,bool create_root) {
    int parent=owned_directory(root,create_root,create_root);
    if(parent<0)return false;
    if(mkdirat(parent,"state",0700) && errno!=EEXIST){close(parent);return false;}
    int fd=openat(parent,"state",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    close(parent);
    if(fd<0)return false;
    struct stat s;
    if(fstat(fd,&s)||s.st_uid!=geteuid()||fchmod(fd,0700)){close(fd);return false;}
    DIR *entries=fdopendir(fd);
    if(!entries){close(fd);return false;}
    bool ok=true;
    while(true) {
        errno=0;dirent *entry=readdir(entries);
        if(!entry){if(errno)ok=false;break;}
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        int file=owned_file_at(fd,entry->d_name,O_RDONLY);
        if(file<0){ok=false;break;}
        close(file);
    }
    closedir(entries);return ok;
}
