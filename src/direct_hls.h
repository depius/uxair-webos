#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "direct_video.h"
#include "hls_relay.h"

class AudioDecoder;

struct DirectSegment {
    std::string url;
    int64_t start_ns;
    int64_t duration_ns;
    HlsRange range;
    unsigned init = 0, period = 0;
};
struct DirectInitSegment { std::string url; HlsRange range; };

struct DirectPlaylist {
    std::string variant;
    std::string alternate;
    std::string diagnostic_tag;
    std::vector<DirectSegment> segments;
    std::vector<DirectInitSegment> maps;
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
    const char* video_codec_name = "unknown";
    bool hevc = false;
    int hevc_rap = -1;
    const char* timestamp_problem = "none";
    int probe_score = 0, video_codec = -1, audio_codec = -1, h264_profile = 0, h264_level = 0;
    unsigned video_packets = 0, audio_packets = 0, reordered = 0, tied_dts = 0, inferred_dts = 0;
    unsigned aac_without_pcm = 0;
    unsigned ffmpeg_timestamp_warnings = 0;
    unsigned tracks = 0, fragments = 0;
    size_t first_pcm_bytes = 0;
    bool idr = false, starts_with_idr = false, sps_pps = false;
};

DirectError direct_demux_segment(const std::string& data, const std::string& init, const std::atomic<bool>& cancelled,
                            AudioDecoder& audio, int width, int height, DirectDemuxedSegment& segment);
// Install before starting playback threads. Unrelated FFmpeg logs retain the default callback.
void direct_hls_log_setup();
