#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
struct AVCodecContext;
struct SwrContext;
class AudioDecoder {
public:
    ~AudioDecoder();
    bool decode(unsigned codec, const uint8_t *data, size_t size, std::vector<uint8_t>& pcm);
    bool decode_adts(const uint8_t *data, size_t size, std::vector<uint8_t>& pcm);
    bool decode_mp4_aac(const uint8_t *data, size_t size, const uint8_t *config, size_t config_size,
                        int rate, int channels, std::vector<uint8_t>& pcm);
    void reset();
private:
    bool decode_packet(unsigned codec, const uint8_t *data, size_t size, std::vector<uint8_t>& pcm);
    bool configure(unsigned codec, const uint8_t* asc=nullptr, size_t asc_size=0, int rate=0, int channels=0);
    unsigned codec_ = 0;
    int input_rate_=0, input_channels_=0;
    AVCodecContext *context_ = nullptr;
    SwrContext *resampler_ = nullptr;
};

#ifdef UXAIR_DIRECT_BUILD
struct AVPacket;
struct AVStream;
struct HlsPcm {
    std::vector<uint8_t> data;
    int64_t time_ns=0;
};

// Owns a single HLS audio track and its sample clock on the download worker.
// AAC and RAOP continue to use AudioDecoder.
class HlsAudioDecoder {
public:
    ~HlsAudioDecoder();
    HlsAudioDecoder()=default;
    HlsAudioDecoder(const HlsAudioDecoder&)=delete;
    HlsAudioDecoder& operator=(const HlsAudioDecoder&)=delete;
    const char* decode(const AVStream&,const AVPacket*,std::vector<HlsPcm>&);
    void reset();
    int rate=0, channels=0;
    unsigned frames=0;
private:
    AVCodecContext* context_=nullptr;
    SwrContext* resampler_=nullptr;
    int format_=-1;
    uint64_t channel_mask_=0;
    int64_t origin_=0, input_samples_=0, output_samples_=0;
    bool clock_started_=false;
};
#endif
