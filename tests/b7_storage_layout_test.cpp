// Root-only Linux fixtures model UIDs; they are not a webOS jail test.
#include "secure_data.h"
#include "settings.h"
#include "usage_stats.h"
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

static void persistence(const std::string& app,bool first) {
    pid_t child=fork();assert(child>=0);
    if(!child) {
        assert(setgroups(0,nullptr)==0&&setgid(5000)==0&&setuid(5999)==0);
        umask(0077);
        assert(!prepare_state(app,false)); // Strict/NDL policy still rejects this parent.
        assert(prepare_state(app,false,true));
        const std::string state=app+"/state";
        for(const char *name:{"device.id","server.key","paired.keys"}) {
            const std::string path=state+"/"+name;
            if(first) {
                int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);
                assert(write(fd,"ABCDEF",6)==6);close(fd);
            }
            int fd=open(path.c_str(),O_RDONLY);assert(fd>=0);char bytes[6];
            assert(read(fd,bytes,6)==6&&!memcmp(bytes,"ABCDEF",6));close(fd);
        }
        Settings settings;UsageStats usage;
        if(first) {
            settings.width=1280;settings.height=720;settings.max_fps=60;
            assert(save_settings(state+"/settings-v1",settings));
            usage.playback_ms=12345;assert(save_usage_stats(state+"/usage-v1",usage));
        }
        Settings loaded;UsageStats loaded_usage;
        assert(load_settings(state+"/settings-v1",loaded)&&loaded.width==1280&&loaded.max_fps==60);
        assert(load_usage_stats(state+"/usage-v1",loaded_usage)&&loaded_usage.playback_ms==12345);
        int log=open_rotated_log(state,"uxair.log");assert(log>=0);
        assert(write(log,"log-entry",9)==9);close(log);
        if(!first) {
            int fd=open((state+"/uxair.log.previous").c_str(),O_RDONLY);assert(fd>=0);
            char bytes[9];assert(read(fd,bytes,9)==9&&!memcmp(bytes,"log-entry",9));close(fd);
        }
        const std::string attack=state+"/attack";
        assert(!symlink("device.id",attack.c_str()));assert(!prepare_state(app,false,true));unlink(attack.c_str());
        assert(!link((state+"/device.id").c_str(),attack.c_str()));assert(!prepare_state(app,false,true));unlink(attack.c_str());
        assert(!mkfifo(attack.c_str(),0600));assert(!prepare_state(app,false,true));unlink(attack.c_str());
        assert(!symlink("device.id",(state+"/bad.log").c_str()));
        assert(open_rotated_log(state,"bad.log")<0);unlink((state+"/bad.log").c_str());
        assert(prepare_state(app,false,true));
        fflush(stderr);_exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
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
    assert(rmdir((app+"/state").c_str())==0);
    assert(chmod(app.c_str(),0777)==0); // Reproduce the observed installer metadata.
    persistence(app,true);persistence(app,false);
    assert(stat(app.c_str(),&info)==0&&info.st_uid==501&&info.st_gid==0&&(info.st_mode&07777)==0777);
    assert(stat((app+"/state").c_str(),&info)==0&&info.st_uid==5999&&(info.st_mode&07777)==0700);
    // A root-owned unexpected file is not accepted by the jailed process.
    const std::string bad=app+"/state/foreign";
    int badfd=open(bad.c_str(),O_CREAT|O_WRONLY,0644);assert(badfd>=0);close(badfd);
    pid_t check=fork();assert(check>=0);
    if(!check){assert(setgid(5000)==0&&setuid(5999)==0);assert(!prepare_state(app,false,true));_exit(0);}
    int status;assert(waitpid(check,&status,0)==check&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    unlink(bad.c_str());
    for(const char *leaf:{"device.id","server.key","paired.keys","settings-v1","usage-v1","uxair.log","uxair.log.previous"})
        assert(unlink((app+"/state/"+leaf).c_str())==0);
    assert(rmdir((app+"/state").c_str())==0);assert(rmdir(app.c_str())==0);assert(rmdir(root.c_str())==0);
    assert(state_log_path("a\nb")=="a?b");
    puts("PASS: B7 5999:5000 persistence across launches, identity reuse, settings/history, log rotation, unsafe files and unchanged installation metadata; strict policy preserved");
}
