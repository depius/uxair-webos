#pragma once
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Diagnostics contain filesystem metadata only, never identity/key contents.
inline std::string state_log_path(const std::string& path) {
    std::string safe=path.substr(0,512);
    for(char& c:safe)if(static_cast<unsigned char>(c)<32 || c==127)c='?';
    return safe;
}
inline void state_path_info(const std::string& path) {
    struct stat s;
    if(lstat(path.c_str(),&s)) {
        const int error=errno;
        fprintf(stderr,"[state] candidate path=%s lstat errno=%d (%s)\n",
                state_log_path(path).c_str(),error,strerror(error));
        errno=error;return;
    }
    fprintf(stderr,"[state] candidate path=%s uid=%lu gid=%lu mode=%04o type=%s\n",
            state_log_path(path).c_str(),static_cast<unsigned long>(s.st_uid),
            static_cast<unsigned long>(s.st_gid),unsigned(s.st_mode&07777),
            S_ISLNK(s.st_mode)?"symlink":S_ISDIR(s.st_mode)?"directory":S_ISREG(s.st_mode)?"file":"other");
}
inline void state_rejected(const std::string& path,const char *operation,int error,const struct stat *s=nullptr) {
    fprintf(stderr,"[state] rejected path=%s check=%s errno=%d (%s) expected_uid=%lu",
            state_log_path(path).c_str(),operation,error,strerror(error),static_cast<unsigned long>(geteuid()));
    if(s)fprintf(stderr," actual_uid=%lu actual_gid=%lu mode=%04o nlink=%lu",
                 static_cast<unsigned long>(s->st_uid),static_cast<unsigned long>(s->st_gid),
                 unsigned(s->st_mode&07777),static_cast<unsigned long>(s->st_nlink));
    fputc('\n',stderr);errno=error;
}
inline void state_environment(const std::string& app,const std::string& lib,const std::string& selected,bool ndl) {
    fprintf(stderr,"[state] startup pid=%ld uid=%lu euid=%lu gid=%lu egid=%lu backend=%s\n",
            static_cast<long>(getpid()),static_cast<unsigned long>(getuid()),static_cast<unsigned long>(geteuid()),
            static_cast<unsigned long>(getgid()),static_cast<unsigned long>(getegid()),ndl?"ndl":"starfish");
    state_path_info(app);state_path_info(app+"/state");
    state_path_info(lib);state_path_info(lib+"/state");
    fprintf(stderr,"[state] selected path=%s policy=%s\n",state_log_path(selected).c_str(),
            ndl?"NDL persistent var/lib":selected==lib?"reuse existing var/lib state":"Starfish app-local state");
    fflush(stderr);
}
// An inaccessible or unsafe existing identity must not cause a switch to
// newly initialized storage elsewhere.
inline std::string select_state_root(const std::string& app,const std::string& lib,bool ndl) {
    if(ndl) {
        // A backend upgrade must not silently replace a legacy app-local
        // identity with new keys in var/lib. Leave migration explicit.
        struct stat prior;
        const bool lib_missing=lstat((lib+"/state").c_str(),&prior)!=0 && errno==ENOENT;
        const bool local_missing=lstat((app+"/state").c_str(),&prior)!=0 && errno==ENOENT;
        if(lib_missing && !local_missing) {
            state_rejected(app+"/state","legacy-identity-migration-required",EEXIST);
            return "";
        }
        return lib;
    }
    struct stat local_state,lib_state;
    const bool use_lib=lstat((app+"/state").c_str(),&local_state)!=0 && errno==ENOENT &&
                       lstat((lib+"/state").c_str(),&lib_state)==0;
    return use_lib?lib:app;
}
// Platform-owned ancestors must be trusted. Never follow the application's
// data/state leaf or an existing state/log file through a symlink.
// Ownership and permission policy remains unchanged pending jailed B7 evidence.
inline int owned_directory(const std::string& path,bool create,bool private_mode) {
    if(create && mkdir(path.c_str(),0700) && errno!=EEXIST) {
        state_rejected(path,"mkdir",errno);return -1;
    }
    int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0){state_rejected(path,"open-directory-no-follow",errno);return -1;}
    struct stat s;
    const char *reason=nullptr;int error=0;
    if(fstat(fd,&s)){reason="fstat-directory";error=errno;}
    else if(!S_ISDIR(s.st_mode)){reason="directory-type";error=ENOTDIR;}
    else if(s.st_uid!=geteuid()){reason="directory-owner-mismatch";error=EACCES;}
    else if(fchmod(fd,private_mode?0700:(s.st_mode&0755))){reason="fchmod-directory";error=errno;}
    if(reason){close(fd);state_rejected(path,reason,error,strcmp(reason,"fstat-directory")?&s:nullptr);return -1;}
    return fd;
}
inline int owned_file_at(int directory,const char *name,int flags,const std::string& diagnostic_path="") {
    const std::string path=diagnostic_path.empty()?name:diagnostic_path;
    // O_NONBLOCK avoids hanging on an attacker-created FIFO before fstat.
    int fd=openat(directory,name,flags|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if(fd<0){state_rejected(path,"open-file-no-follow",errno);return -1;}
    struct stat s;
    const char *reason=nullptr;int error=0;
    if(fstat(fd,&s)){reason="fstat-file";error=errno;}
    else if(!S_ISREG(s.st_mode)){reason="file-type";error=EINVAL;}
    else if(s.st_uid!=geteuid()){reason="file-owner-mismatch";error=EACCES;}
    else if(s.st_nlink!=1){reason="file-hardlinks";error=EMLINK;}
    else if(fchmod(fd,0600)){reason="fchmod-file";error=errno;}
    if(reason){close(fd);state_rejected(path,reason,error,strcmp(reason,"fstat-file")?&s:nullptr);return -1;}
    return fd;
}
// Starfish alone may trust its executable installation container, as the old
// webOS jail requires. This does NOT make writable ancestors a security boundary.
// State/files remain process-owned; the installation container is never chmodded.
inline bool prepare_state(const std::string& root,bool create_root,bool starfish_app_container=false) {
    int parent=starfish_app_container && !create_root
        ? open(root.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)
        : owned_directory(root,create_root,create_root);
    if(parent<0 && starfish_app_container)state_rejected(root,"open-install-container",errno);
    if(parent<0)return false;
    const std::string path=root+"/state";
    if(mkdirat(parent,"state",0700) && errno!=EEXIST) {
        const int error=errno;close(parent);state_rejected(path,"mkdirat-state",error);return false;
    }
    int fd=openat(parent,"state",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    const int open_error=errno;close(parent);
    if(fd<0){state_rejected(path,"open-state-no-follow",open_error);return false;}
    struct stat s;
    const char *reason=nullptr;int error=0;
    if(fstat(fd,&s)){reason="fstat-state";error=errno;}
    else if(s.st_uid!=geteuid()){reason="state-owner-mismatch";error=EACCES;}
    else if(fchmod(fd,0700)){reason="fchmod-state";error=errno;}
    if(reason){close(fd);state_rejected(path,reason,error,strcmp(reason,"fstat-state")?&s:nullptr);return false;}
    DIR *entries=fdopendir(fd);
    if(!entries){const int error=errno;close(fd);state_rejected(path,"fdopendir-state",error);return false;}
    bool ok=true;int failure=0;
    while(true) {
        errno=0;dirent *entry=readdir(entries);
        if(!entry){if(errno){failure=errno;state_rejected(path,"readdir-state",failure);ok=false;}break;}
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        int file=owned_file_at(fd,entry->d_name,O_RDONLY,path+"/"+entry->d_name);
        if(file<0){failure=errno;ok=false;break;}
        close(file);
    }
    closedir(entries);if(!ok)errno=failure;return ok;
}

// Only call on validated private storage, never the writable install container.
inline int open_rotated_log(const std::string& directory,const char *name) {
    if(!name || !*name || strchr(name,'/') || !strcmp(name,".") || !strcmp(name,"..")) {
        errno=EINVAL;return -1;
    }
    int parent=owned_directory(directory,false,false);
    if(parent<0)return -1;
    const std::string previous=std::string(name)+".previous";
    for(const std::string& leaf:{std::string(name),previous}) {
        struct stat st;
        if(fstatat(parent,leaf.c_str(),&st,AT_SYMLINK_NOFOLLOW)) {
            if(errno==ENOENT)continue;
            const int error=errno;close(parent);state_rejected(directory+"/"+leaf,"stat-log",error);return -1;
        }
        int checked=owned_file_at(parent,leaf.c_str(),O_WRONLY,directory+"/"+leaf);
        if(checked<0){close(parent);return -1;}close(checked);
    }
    if(renameat(parent,name,parent,previous.c_str()) && errno!=ENOENT) {
        const int error=errno;close(parent);state_rejected(directory+"/"+name,"rotate-log",error);return -1;
    }
    int fd=owned_file_at(parent,name,O_WRONLY|O_CREAT,directory+"/"+name);close(parent);
    return fd;
}
