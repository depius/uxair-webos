#pragma once
#include <string>
#include <deque>
#include <mutex>
#include <cstdint>
enum class DirectCommandKind { Play, Pause, Resume, Seek, Stop };
struct DirectCommand { DirectCommandKind kind; std::string uri; double position=0; };
enum class DirectState { Idle, Loading, Playing, Paused, Ended, Error };
struct DirectSnapshot { DirectState state=DirectState::Idle; double position=0,duration=0; };
struct DirectReply { int status=400; std::string body,type="text/parameters"; };
bool direct_uri_valid(const std::string& uri);
std::string direct_load_payload(const std::string& uri,const std::string& app);
class DirectSession {
    std::mutex mutex_;std::deque<DirectCommand> commands_;DirectSnapshot snapshot_;std::string owner_;
public:
    DirectReply request(const std::string& owner,const std::string& method,const std::string& target,
                        const std::string& type,const std::string& body,bool busy=false);
    bool take(DirectCommand& command);
    void publish(DirectSnapshot snapshot);
    DirectSnapshot snapshot();
    void reset();
};
