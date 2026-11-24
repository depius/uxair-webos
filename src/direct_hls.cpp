#include "direct_hls.h"
#include "audio.h"
#include "h264.h"
#include "hls_relay.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {
constexpr int64_t second = 1000000000;
constexpr size_t max_segment_bytes = 8 * 1024 * 1024;
constexpr size_t max_packet_bytes = 2 * 1024 * 1024;
constexpr size_t max_segment_packets = 1152;

bool starts(const std::string& value, const char* prefix) {
    return value.compare(0, std::strlen(prefix), prefix) == 0;
}

bool decimal(const std::string& value, double& number) {
    if (value.empty() || value.find_first_not_of("0123456789.") != std::string::npos) return false;
    char* end = nullptr;
    number = std::strtod(value.c_str(), &end);
    return end == value.c_str() + value.size() && std::isfinite(number);
}

std::string attribute(const std::string& line, const char* key) {
    const auto colon = line.find(':');
    size_t pos = colon + 1;
    while (pos < line.size()) {
        const auto equal = line.find('=', pos);
        if (equal == std::string::npos) break;
        const bool quoted = equal + 1 < line.size() && line[equal + 1] == '"';
        const size_t first = equal + (quoted ? 2 : 1);
        const auto end = line.find(quoted ? '"' : ',', first);
        if (quoted && end == std::string::npos) break;
        if (line.compare(pos, equal - pos, key) == 0) return line.substr(first, end - first);
        if (end == std::string::npos) break;
        pos = end + (quoted ? 2 : 1);
    }
    return {};
}

double variant_bandwidth(const std::string& tag) {
    const auto codecs = attribute(tag, "CODECS");
    if (!codecs.empty()) {
        const auto comma = codecs.find(',');
        if (comma == std::string::npos || codecs.find(',', comma + 1) != std::string::npos) return 0;
        const auto first_codec = codecs.substr(0, comma);
        const auto second_codec = codecs.substr(comma + 1);
        if (!((starts(first_codec, "avc1.") && second_codec == "mp4a.40.2") ||
              (first_codec == "mp4a.40.2" && starts(second_codec, "avc1.")))) return 0;
    }
    if (!attribute(tag, "AUDIO").empty() || !attribute(tag, "VIDEO").empty()) return 0;
    const auto resolution = attribute(tag, "RESOLUTION");
    if (!resolution.empty()) {
        const auto divider = resolution.find('x');
        double width = 0, height = 0;
        if (divider == std::string::npos || !decimal(resolution.substr(0, divider), width) ||
            !decimal(resolution.substr(divider + 1), height) || width > 1920 || height > 1080) return 0;
    }
    const auto frame_rate = attribute(tag, "FRAME-RATE");
    double fps = 0;
    if (!frame_rate.empty() && (!decimal(frame_rate, fps) || fps > 60)) return 0;
    double bandwidth = 0;
    return decimal(attribute(tag, "BANDWIDTH"), bandwidth) && bandwidth <= 20000000 ? bandwidth : 0;
}

struct SegmentReader {
    const std::string& data;
    const std::atomic<bool>& cancelled;
    size_t offset = 0;

    SegmentReader(const std::string& bytes, const std::atomic<bool>& cancelled) : data(bytes), cancelled(cancelled) {}

    static int read(void* opaque, uint8_t* dst, int length) {
        auto& reader = *static_cast<SegmentReader*>(opaque);
        if (reader.cancelled) return AVERROR_EXIT;
        const size_t count = std::min(size_t(length), reader.data.size() - reader.offset);
        if (!count) return AVERROR_EOF;
        std::memcpy(dst, reader.data.data() + reader.offset, count);
        reader.offset += count;
        return int(count);
    }

    static int interrupted(void* opaque) { return static_cast<SegmentReader*>(opaque)->cancelled.load(); }
};

struct Demuxer {
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVPacket* packet = nullptr;

    ~Demuxer() {
        av_packet_free(&packet);
        avformat_close_input(&format);
        if (io) av_freep(&io->buffer);
        avio_context_free(&io);
    }

    bool open(SegmentReader& reader) {
        const auto* input = av_find_input_format("mpegts");
        if (!input) return false;
        auto* buffer = static_cast<uint8_t*>(av_malloc(16384));
        if (!buffer) return false;
        io = avio_alloc_context(buffer, 16384, 0, &reader, SegmentReader::read, nullptr, nullptr);
        if (!io) { av_free(buffer); return false; }
        format = avformat_alloc_context();
        if (!format) return false;
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        format->max_streams = 8;
        format->probesize = 128 * 1024;
        format->max_probe_packets = 64;
        format->interrupt_callback = {SegmentReader::interrupted, &reader};
        return avformat_open_input(&format, nullptr, input, nullptr) == 0 && (packet = av_packet_alloc());
    }
};

struct AdtsFormat { int sample_rate; int channels; };

AdtsFormat adts_aac_lc(const AVPacket& packet) {
    if (packet.size < 7) return {};
    const auto* data = packet.data;
    const unsigned rate = (data[2] >> 2) & 15;
    const unsigned channels = ((data[2] & 1) << 2) | (data[3] >> 6);
    const int frame_bytes = ((data[3] & 3) << 11) | (data[4] << 3) | (data[5] >> 5);
    if (data[0] != 0xff || (data[1] & 0xf6) != 0xf0 || (data[2] >> 6) != 1 ||
        (rate != 3 && rate != 4) || (channels != 1 && channels != 2) ||
        (data[6] & 3) || frame_bytes != packet.size) return {};
    return {rate == 3 ? 48000 : 44100, int(channels)};
}

bool has_pps(const std::vector<uint8_t>& configuration) {
    for (size_t i = 0; i + 3 < configuration.size(); ++i) {
        if (!configuration[i] && !configuration[i+1] && configuration[i+2] == 1 &&
            (configuration[i+3] & 31) == 8) return true;
    }
    return false;
}
} // namespace

DirectHlsResult direct_parse_playlist(const std::string& text, const std::string& base, DirectPlaylist& playlist) {
    playlist = DirectPlaylist();
    if (text.size() > 256 * 1024) return DirectHlsResult::Limit;
    if (text.find('\0') != std::string::npos || text.find("{$") != std::string::npos) return DirectHlsResult::Invalid;
    std::istringstream lines(text);
    std::string line;
    if (!std::getline(lines, line) || (line != "#EXTM3U" && line != "#EXTM3U\r")) return DirectHlsResult::Invalid;
    bool master = false, end = false, waiting_variant = false;
    double bandwidth = 0, selected_bandwidth = 0;
    int64_t duration = 0;
    unsigned variants = 0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() > 8192) return DirectHlsResult::Limit;
        if (line[0] != '#') {
            if (end || (master && !waiting_variant) || (!master && duration == 0)) return DirectHlsResult::Invalid;
            const auto url = HlsRelay::resolve(base, line);
            if (url.empty()) return DirectHlsResult::Invalid;
            if (master) {
                if (bandwidth > selected_bandwidth) {
                    playlist.variant = url;
                    selected_bandwidth = bandwidth;
                }
                waiting_variant = false;
            } else {
                if (playlist.segments.size() >= 4096 || playlist.duration_ns > 24 * 60 * 60 * second - duration)
                    return DirectHlsResult::Limit;
                playlist.segments.push_back({url, playlist.duration_ns, duration});
                playlist.duration_ns += duration;
                duration = 0;
            }
        } else if (starts(line, "#EXT-X-STREAM-INF:")) {
            if (end || duration || waiting_variant || !playlist.segments.empty()) return DirectHlsResult::Invalid;
            if (++variants > 32) return DirectHlsResult::Limit;
            master = waiting_variant = true;
            bandwidth = variant_bandwidth(line);
        } else if (starts(line, "#EXTINF:")) {
            double parsed = 0;
            if (master || end || duration || !decimal(line.substr(8, line.find(',') - 8), parsed) || parsed < 0.01)
                return DirectHlsResult::Invalid;
            if (parsed > 12) return DirectHlsResult::Limit;
            duration = int64_t(std::llround(parsed * second));
        } else if (line == "#EXT-X-ENDLIST") {
            if (master || duration || end) return DirectHlsResult::Invalid;
            end = true;
        } else if (!starts(line, "#EXT") || line == "#EXT-X-INDEPENDENT-SEGMENTS" ||
                   line == "#EXT-X-KEY:METHOD=NONE" || starts(line, "#EXT-X-VERSION:") ||
                   starts(line, "#EXT-X-TARGETDURATION:") || starts(line, "#EXT-X-MEDIA-SEQUENCE:") ||
                   starts(line, "#EXT-X-PLAYLIST-TYPE:") || starts(line, "#EXT-X-PROGRAM-DATE-TIME:") ||
                   starts(line, "#EXT-X-MEDIA:") || starts(line, "#EXT-X-I-FRAME-STREAM-INF:")) {
            continue;
        } else {
            return DirectHlsResult::Unsupported;
        }
    }
    if (waiting_variant || duration) return DirectHlsResult::Invalid;
    if (master) return playlist.variant.empty() ? DirectHlsResult::Unsupported : DirectHlsResult::Ok;
    if (playlist.segments.empty()) return DirectHlsResult::Invalid;
    return end ? DirectHlsResult::Ok : DirectHlsResult::Unsupported;
}

DirectHlsResult direct_demux_ts(const std::string& data, const std::atomic<bool>& cancelled,
                               AudioDecoder& audio, DirectDemuxedSegment& segment) {
    segment = DirectDemuxedSegment();
    if (cancelled) return DirectHlsResult::Cancelled;
    if (data.size() > max_segment_bytes) return DirectHlsResult::Limit;
    SegmentReader reader(data, cancelled);
    Demuxer demux;
    if (!demux.open(reader)) return cancelled ? DirectHlsResult::Cancelled : DirectHlsResult::Invalid;
    int video_index = -1, audio_index = -1, result = 0;
    unsigned packets = 0;
    int64_t audio_pts_anchor = AV_NOPTS_VALUE;
    int audio_samples = 0;
    while (!cancelled && (result = av_read_frame(demux.format, demux.packet)) >= 0) {
        if (++packets > max_segment_packets) return DirectHlsResult::Limit;
        AVPacket& packet = *demux.packet;
        const AVStream& stream = *demux.format->streams[packet.stream_index];
        const bool video = stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        const bool is_audio = stream.codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
        if (!video && !is_audio) { av_packet_unref(&packet); continue; }
        if (packet.size <= 0 || size_t(packet.size) > max_packet_bytes) return DirectHlsResult::Limit;
        if (packet.flags & AV_PKT_FLAG_CORRUPT) return DirectHlsResult::Invalid;
        const AVRational ns = {1, int(second)};
        DirectAccessUnit unit;
        unit.video = video;
        if (video) {
            if (packet.pts == AV_NOPTS_VALUE || packet.dts == AV_NOPTS_VALUE) return DirectHlsResult::Invalid;
            unit.pts_ns = av_rescale_q(packet.pts, stream.time_base, ns);
            unit.dts_ns = av_rescale_q(packet.dts, stream.time_base, ns);
            unit.duration_ns = av_rescale_q(packet.duration, stream.time_base, ns);
            if (stream.codecpar->codec_id != AV_CODEC_ID_H264 ||
                (video_index >= 0 && packet.stream_index != video_index)) return DirectHlsResult::Unsupported;
            const auto info = inspect_h264(packet.data, size_t(packet.size));
            if (!info.valid) return DirectHlsResult::Invalid;
            if (!info.picture) { av_packet_unref(&packet); continue; }
            if (video_index < 0) {
                if (!info.keyframe || !info.width || !has_pps(info.configuration)) return DirectHlsResult::Unsupported;
                if (info.height > 1080 || info.width > 1920) return DirectHlsResult::Unsupported;
                segment.width = info.width;
                segment.height = info.height;
                segment.first_video_dts_ns = unit.dts_ns;
                video_index = packet.stream_index;
            }
            if (info.width && (info.width != segment.width || info.height != segment.height))
                return DirectHlsResult::Unsupported;
            unit.data.assign(packet.data, packet.data + packet.size);
        } else {
            const auto adts = adts_aac_lc(packet);
            if (stream.codecpar->codec_id != AV_CODEC_ID_AAC || !adts.sample_rate ||
                (segment.aac_sample_rate && (segment.aac_sample_rate != adts.sample_rate ||
                                             segment.aac_channels != adts.channels)) ||
                (audio_index >= 0 && packet.stream_index != audio_index)) return DirectHlsResult::Unsupported;
            segment.aac_sample_rate = adts.sample_rate;
            segment.aac_channels = adts.channels;
            const AVRational sample_time = {1, adts.sample_rate};
            // MPEG-TS timestamps the PES. Splitting ADTS access units can leave
            // subsequent frames without a timestamp until the next PES anchor.
            if (packet.pts != AV_NOPTS_VALUE) {
                audio_pts_anchor = av_rescale_q(packet.pts, stream.time_base, ns);
                audio_samples = 0;
            }
            if (audio_pts_anchor == AV_NOPTS_VALUE) return DirectHlsResult::Invalid;
            unit.pts_ns = unit.dts_ns = audio_pts_anchor + av_rescale_q(audio_samples, sample_time, ns);
            audio_samples += 1024;
            unit.duration_ns = av_rescale_q(1024, sample_time, ns);
            if (!audio.decode_adts(packet.data, size_t(packet.size), unit.data) || unit.data.empty())
                return DirectHlsResult::Invalid;
            audio_index = packet.stream_index;
        }
        constexpr int64_t max_timestamp = 2 * 24 * 60 * 60 * second;
        if (unit.pts_ns < -second || unit.pts_ns > max_timestamp || unit.dts_ns < -second ||
            unit.dts_ns > max_timestamp || unit.duration_ns < 0 || unit.duration_ns > second)
            return DirectHlsResult::Invalid;
        if (unit.data.size() > max_segment_bytes - segment.bytes) return DirectHlsResult::Limit;
        segment.bytes += unit.data.size();
        segment.packets.push_back(std::move(unit));
        av_packet_unref(&packet);
    }
    if (cancelled) return DirectHlsResult::Cancelled;
    if (result != AVERROR_EOF) return DirectHlsResult::Invalid;
    if (video_index < 0 || audio_index < 0) return DirectHlsResult::Unsupported;
    std::stable_sort(segment.packets.begin(), segment.packets.end(), [](const DirectAccessUnit& a, const DirectAccessUnit& b) {
        return a.dts_ns < b.dts_ns;
    });
    if (segment.packets.back().dts_ns - segment.packets.front().dts_ns > 13 * second) return DirectHlsResult::Limit;
    return DirectHlsResult::Ok;
}
