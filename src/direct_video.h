#pragma once
#include <string>
#include <deque>
#include <mutex>
#include <cstdint>
#include <thread>
#include <condition_variable>
#include <atomic>
enum class DirectCommandKind { Play, Pause, Resume, Seek, Stop };
struct DirectCommand { DirectCommandKind kind; std::string uri; double position=0; bool position_seconds=false; };
enum class DirectState { Idle, Loading, Playing, Paused, Ended, Error };
struct DirectSnapshot { DirectState state=DirectState::Idle; double position=0,duration=0;unsigned width=0,height=0; };
const char *direct_state_name(DirectState state);
struct DirectReply { int status=400; std::string body,type="text/parameters"; };
bool direct_uri_valid(const std::string& uri);
std::string direct_load_payload(const std::string& uri,const std::string& app,bool internal=false);
class DirectSession {
    std::thread reverse_thread_;std::atomic<bool> reverse_running_{false};int reverse_fd_=-1;uintptr_t reverse_token_=0;
    std::condition_variable event_cv_;std::deque<std::string> events_;
    int64_t last_request_ms_=0;
    unsigned request_id_=0,pending_id_=0;std::string pending_url_,pending_data_;bool pending_done_=false;
    void reverse_loop();void close_reverse();
    std::mutex mutex_;std::deque<DirectCommand> commands_;DirectSnapshot snapshot_;std::string owner_;
public:
    ~DirectSession();
    bool bind_reverse(const std::string&,int fd,uintptr_t token);
    void cancel_fetch();
    bool check_timeout(int64_t now_ms);
    void connection_lost(uintptr_t token);
    std::string sender_manifest(const std::string& uri);
    DirectReply request(const std::string& owner,const std::string& method,const std::string& target,
                        const std::string& type,const std::string& body,bool busy=false);
    bool take(DirectCommand& command);
    void publish(DirectSnapshot snapshot);
    DirectSnapshot snapshot();
    void reset(bool keep_reverse=false);
};
