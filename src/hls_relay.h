#pragma once
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <functional>
struct HlsRange {
    uint64_t offset=0,length=0;
    bool operator==(const HlsRange& other) const { return offset==other.offset && length==other.length; }
};
bool hls_decimal_integer(const std::string& value, uint64_t& number);
struct HlsFetch {int status=502,http_status=0;std::string data,url,type;bool cancelled=false,limit_exceeded=false,range_rejected=false;};
class HlsRelay {
    std::atomic<bool> running_{false};std::atomic<int> listener_{-1};std::vector<std::thread> workers_;
    std::mutex clients_mutex_;std::vector<int> clients_;
    std::mutex mutex_;std::vector<std::string> resources_;std::vector<bool> manifests_;std::string token_;unsigned port_=0;std::string allowed_lan_;
    std::string register_url(const std::string&,bool manifest=false);
    std::atomic<unsigned> diagnostic_records_{0};
    std::mutex cookie_mutex_;
    void* cookie_share_=nullptr;
    std::string request_origin_;
    uint64_t generation_=0;
    std::string resource_path(size_t index,const std::string& uri,bool manifest) const;
    void serve();
public:
    void trace(const char *stage,int code,long http=0);
    std::function<std::string(const std::string&)> sender_manifest;
    std::atomic<uint64_t> bytes{0},requests{0},failures{0},incoming_requests{0},media_gets{0};
    ~HlsRelay(){stop();}
    bool begin(bool allow_configured_lan=true);
    void set_request_origin(const std::string& uri,uint64_t generation);
    void cancel() { running_=false; }
    std::string start(const std::string&,bool allow_configured_lan=true);
    void stop();
    HlsFetch fetch(const std::string&,size_t limit,bool manifest=false,HlsRange range={},const char* role=nullptr);
    std::string rewrite(const std::string&,const std::string& base);
    static std::string resolve(const std::string&,const std::string&);
    bool address_allowed(uint32_t address) const;
};
