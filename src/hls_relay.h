#pragma once
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <functional>
struct HlsFetch {int status=502;std::string data,url,type;};
class HlsRelay {
    std::atomic<bool> running_{false};std::atomic<int> listener_{-1};std::vector<std::thread> workers_;
    std::mutex clients_mutex_;std::vector<int> clients_;
    std::mutex mutex_;std::vector<std::string> resources_;std::vector<bool> manifests_;std::string token_;unsigned port_=0;std::string allowed_lan_;
    std::string register_url(const std::string&,bool manifest=false);
    std::atomic<unsigned> diagnostic_records_{0};
    std::string resource_path(size_t index,const std::string& uri,bool manifest) const;
    void serve();
public:
    void trace(const char *stage,int code,long http=0);
    std::function<std::string(const std::string&)> sender_manifest;
    std::atomic<uint64_t> bytes{0},requests{0},failures{0},incoming_requests{0};
    ~HlsRelay(){stop();}
    std::string start(const std::string&);
    void stop();
    HlsFetch fetch(const std::string&,size_t limit,bool manifest=false);
    std::string rewrite(const std::string&,const std::string& base);
    static std::string resolve(const std::string&,const std::string&);
    bool address_allowed(uint32_t address) const;
};
