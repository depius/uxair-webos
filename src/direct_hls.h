#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "direct_video.h"

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

DirectError direct_parse_playlist(const std::string& text, const std::string& base, DirectPlaylist& playlist);

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
    int64_t last_video_dts_ns = 0;
    int64_t skipped_ns = 0;
    size_t bytes = 0;
    const char* container = "unknown";
    const char* pixel_format = "unknown";
    const char* timestamp_problem = "none";
    int probe_score = 0, video_codec = -1, audio_codec = -1, h264_profile = 0;
    unsigned video_packets = 0, audio_packets = 0, reordered = 0, tied_dts = 0, inferred_dts = 0;
    size_t first_pcm_bytes = 0;
    bool idr = false, starts_with_idr = false, sps_pps = false;
};

DirectError direct_demux_ts(const std::string& data, const std::atomic<bool>& cancelled,
                            AudioDecoder& audio, int width, int height, DirectDemuxedSegment& segment);
