#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class AudioDecoder;

struct DirectSegment {
    std::string url;
    int64_t start_ns;
    int64_t duration_ns;
};

struct DirectPlaylist {
    std::string variant;
    std::vector<DirectSegment> segments;
    int64_t duration_ns = 0;
};

enum class DirectHlsResult { Ok, Invalid, Unsupported, Limit, Cancelled };
DirectHlsResult direct_parse_playlist(const std::string& text, const std::string& base, DirectPlaylist& playlist);

struct DirectAccessUnit {
    std::vector<uint8_t> data;
    int64_t pts_ns = 0;
    int64_t dts_ns = 0;
    int64_t duration_ns = 0;
    bool video = false;
};

struct DirectDemuxedSegment {
    std::vector<DirectAccessUnit> packets;
    int width = 0;
    int height = 0;
    int aac_sample_rate = 0;
    int aac_channels = 0;
    int64_t first_video_dts_ns = 0;
    size_t bytes = 0;
};

DirectHlsResult direct_demux_ts(const std::string& data, const std::atomic<bool>& cancelled,
                               AudioDecoder& audio, DirectDemuxedSegment& segment);
