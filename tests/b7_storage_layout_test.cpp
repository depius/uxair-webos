// Root-only Linux fixtures model UIDs; they are not a webOS jail test.
#include "secure_data.h"
#include <cassert>
#include <cstdlib>
#include <grp.h>
#include <sys/wait.h>

static std::string run_as(const std::string& app,uid_t uid,gid_t gid,bool expected) {
    int output[2];assert(pipe(output)==0);
    pid_t child=fork();assert(child>=0);
    if(!child) {
        close(output[0]);assert(dup2(output[1],STDERR_FILENO)>=0);close(output[1]);
        assert(setgroups(0,nullptr)==0);assert(setgid(gid)==0);assert(setuid(uid)==0);
        const std::string lib=app+"-absent-var-lib";
        state_environment(app,lib,select_state_root(app,lib,false),false);
        assert(prepare_state(app,false)==expected);
        fflush(stderr);_exit(0);
    }
    close(output[1]);std::string log;char buf[1024];ssize_t n;
    while((n=read(output[0],buf,sizeof(buf)))>0)log.append(buf,size_t(n));
    assert(n==0);close(output[0]);int status=0;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);return log;
}
int main() {
    assert(geteuid()==0); // Run explicitly under sudo in CI; never silently skip.
    char pattern[]="/tmp/uxair-b7-layout-XXXXXX";
    char *created=mkdtemp(pattern);assert(created);std::string root=created,app=root+"/app";
    assert(chmod(root.c_str(),0755)==0);assert(mkdir(app.c_str(),0700)==0);
    assert(chown(app.c_str(),501,0)==0);assert(chmod(app.c_str(),0777)==0);
    std::string log=run_as(app,0,0,false);
    assert(log.find("uid=0 euid=0 gid=0 egid=0")!=std::string::npos);
    assert(log.find("check=directory-owner-mismatch errno=")!=std::string::npos);
    assert(log.find("expected_uid=0 actual_uid=501 actual_gid=0 mode=0777")!=std::string::npos);
    struct stat info;assert(stat(app.c_str(),&info)==0&&(info.st_mode&07777)==0777&&info.st_uid==501);
    assert(lstat((app+"/state").c_str(),&info)<0&&errno==ENOENT);
    // A hypothetical jail UID distinct from 501 fails at the same parent check,
    // even when the existing state and identity belong to that process UID.
    assert(mkdir((app+"/state").c_str(),0700)==0);
    const std::string identity=app+"/state/device.id";
    int fd=open(identity.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);assert(fd>=0);
    assert(write(fd,"ABCDEF",6)==6);close(fd);
    assert(chown(identity.c_str(),65001,5000)==0);assert(chown((app+"/state").c_str(),65001,5000)==0);
    log=run_as(app,65001,5000,false);
    assert(log.find("uid=65001 euid=65001 gid=5000 egid=5000")!=std::string::npos);
    assert(log.find("expected_uid=65001 actual_uid=501")!=std::string::npos);
    fd=open(identity.c_str(),O_RDONLY);assert(fd>=0);char bytes[6];assert(read(fd,bytes,6)==6);close(fd);
    assert(memcmp(bytes,"ABCDEF",6)==0);
    assert(stat(identity.c_str(),&info)==0&&info.st_uid==65001&&(info.st_mode&07777)==0600);
    assert(stat(app.c_str(),&info)==0&&(info.st_mode&07777)==0777);
    assert(unlink(identity.c_str())==0);assert(rmdir((app+"/state").c_str())==0);
    // UID 501 is only a counterexample, not an assumption about the TV jail.
    log=run_as(app,501,0,true);
    assert(log.find("rejected")==std::string::npos);
    assert(stat(app.c_str(),&info)==0&&(info.st_mode&07777)==0755);
    assert(stat((app+"/state").c_str(),&info)==0&&info.st_uid==501&&(info.st_mode&07777)==0700);
    assert(rmdir((app+"/state").c_str())==0);assert(rmdir(app.c_str())==0);assert(rmdir(root.c_str())==0);
    assert(state_log_path("a\nb")=="a?b");
    puts("PASS: observed 501:0/0777 layout rejects root and distinct simulated jail UID without replacing identity; owner-UID counterexample and diagnostics verified");
}
