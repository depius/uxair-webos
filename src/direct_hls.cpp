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
        const auto trim = [](const std::string& codec) {
            const auto start = codec.find_first_not_of(" \t");
            return start == std::string::npos ? std::string() :
                codec.substr(start, codec.find_last_not_of(" \t") - start + 1);
        };
        const auto first_codec = trim(codecs.substr(0, comma));
        const auto second_codec = trim(codecs.substr(comma + 1));
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
        // Keep explicit PES timestamps. FFmpeg's generic timestamp fill-in
        // can move DTS backwards after a BipBop B frame, even though the
        // original PES decode times are ordered. AAC uses its own sample clock.
        format->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOFILLIN;
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
    // The AAC decoder reports the actual input format to swresample. BipBop
    // uses index 7 (22.05 kHz), and all standard ADTS rates can be resampled.
    static constexpr int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                    22050, 16000, 12000, 11025, 8000, 7350};
    if (data[0] != 0xff || (data[1] & 0xf6) != 0xf0 || (data[2] >> 6) != 1 ||
        rate >= sizeof(rates) / sizeof(*rates) || (channels != 1 && channels != 2) ||
        ((data[1] & 1) == 0 && packet.size < 9) ||
        (data[6] & 3) || frame_bytes != packet.size) return {};
    return {rates[rate], int(channels)};
}

bool prefer_variant(double candidate, double selected) {
    // Prefer rates that fit a maximum-length segment in the download budget.
    // Keep a higher-rate rendition eligible when the master offers no smaller one.
    constexpr double budget = double(max_segment_bytes) * 8 / 12;
    if (candidate <= 0) return false;
    if (selected <= 0) return true;
    if (candidate <= budget) return selected > budget || candidate > selected;
    return selected > budget && candidate < selected;
}
} // namespace

DirectError direct_parse_playlist(const std::string& text, const std::string& base, DirectPlaylist& playlist) {
    playlist = DirectPlaylist();
    if (text.size() > 256 * 1024) return DirectError::StreamLimit;
    if (text.find('\0') != std::string::npos || text.find("{$") != std::string::npos) return DirectError::PlaylistSyntax;
    std::istringstream lines(text);
    std::string line;
    if (!std::getline(lines, line) || (line != "#EXTM3U" && line != "#EXTM3U\r")) return DirectError::PlaylistSyntax;
    bool master = false, end = false, waiting_variant = false;
    double bandwidth = 0, selected_bandwidth = 0;
    int64_t duration = 0;
    unsigned variants = 0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() > 8192) return DirectError::StreamLimit;
        if (line[0] != '#') {
            if (end || (master && !waiting_variant) || (!master && duration == 0)) return DirectError::PlaylistSyntax;
            const auto url = HlsRelay::resolve(base, line);
            if (url.empty()) return DirectError::PlaylistSyntax;
            if (master) {
                if (prefer_variant(bandwidth, selected_bandwidth)) {
                    playlist.variant = url;
                    selected_bandwidth = bandwidth;
                }
                waiting_variant = false;
            } else {
                if (playlist.segments.size() >= 4096 || playlist.duration_ns > 24 * 60 * 60 * second - duration)
                    return DirectError::StreamLimit;
                playlist.segments.push_back({url, playlist.duration_ns, duration});
                playlist.duration_ns += duration;
                duration = 0;
            }
        } else if (starts(line, "#EXT-X-STREAM-INF:")) {
            if (end || duration || waiting_variant || !playlist.segments.empty()) return DirectError::PlaylistSyntax;
            if (++variants > 32) return DirectError::StreamLimit;
            master = waiting_variant = true;
            bandwidth = variant_bandwidth(line);
        } else if (starts(line, "#EXTINF:")) {
            double parsed = 0;
            if (master || end || duration || !decimal(line.substr(8, line.find(',') - 8), parsed) || parsed < 0.01)
                return DirectError::PlaylistSyntax;
            if (parsed > 12) return DirectError::StreamLimit;
            duration = int64_t(std::llround(parsed * second));
        } else if (line == "#EXT-X-ENDLIST") {
            if (master || duration || end) return DirectError::PlaylistSyntax;
            end = true;
        } else if (!starts(line, "#EXT") || line == "#EXT-X-INDEPENDENT-SEGMENTS" ||
                   line == "#EXT-X-KEY:METHOD=NONE" || starts(line, "#EXT-X-VERSION:") ||
                   starts(line, "#EXT-X-TARGETDURATION:") || starts(line, "#EXT-X-MEDIA-SEQUENCE:") ||
                   starts(line, "#EXT-X-PLAYLIST-TYPE:") || starts(line, "#EXT-X-PROGRAM-DATE-TIME:") ||
                   starts(line, "#EXT-X-MEDIA:") || starts(line, "#EXT-X-I-FRAME-STREAM-INF:")) {
            continue;
        } else {
            if (starts(line, "#EXT-X-KEY:") || starts(line, "#EXT-X-SESSION-KEY:")) return DirectError::PlaylistEncryption;
            if (starts(line, "#EXT-X-MAP:")) return DirectError::PlaylistInitSegment;
            if (starts(line, "#EXT-X-BYTERANGE:")) return DirectError::PlaylistByteRange;
            if (line == "#EXT-X-DISCONTINUITY") return DirectError::PlaylistDiscontinuity;
            if (!starts(line, "#EXT-X-")) continue; // HLS clients ignore other extended comments.
            return DirectError::PlaylistTag;
        }
    }
    if (waiting_variant || duration) return DirectError::PlaylistSyntax;
    if (master) return playlist.variant.empty() ? DirectError::PlaylistVariant : DirectError::None;
    if (playlist.segments.empty()) return DirectError::PlaylistSyntax;
    return end ? DirectError::None : DirectError::PlaylistLive;
}

DirectError direct_demux_ts(const std::string& data, const std::atomic<bool>& cancelled,
                            AudioDecoder& audio, int width, int height, DirectDemuxedSegment& segment) {
    segment = DirectDemuxedSegment();
    if (cancelled) return DirectError::FetchCancelled;
    if (data.size() > max_segment_bytes) return DirectError::StreamLimit;
    // Match the container even when it has a misleading URL suffix or MIME
    // type. The ARM build exposes only the in-memory MPEG-TS demuxer.
    const auto probe_size = std::min(data.size(), size_t(64 * 1024));
    std::vector<uint8_t> prefix(probe_size + AVPROBE_PADDING_SIZE, 0);
    std::copy_n(data.begin(), probe_size, prefix.begin());
    AVProbeData probe = {};
    probe.filename = "";
    probe.buf = prefix.data();
    probe.buf_size = int(probe_size);
    const auto* input = av_probe_input_format3(&probe, 1, &segment.probe_score);
    if (!input || std::strcmp(input->name, "mpegts")) {
        if (data.size() >= 8 && (!data.compare(4, 4, "ftyp") || !data.compare(4, 4, "styp") ||
                                 !data.compare(4, 4, "moof"))) segment.container = "iso-bmff";
        return DirectError::SegmentContainer;
    }
    segment.container = "mpegts";
    SegmentReader reader(data, cancelled);
    Demuxer demux;
    if (!demux.open(reader)) return cancelled ? DirectError::FetchCancelled : DirectError::DemuxOpen;
    const bool continuing = width != 0;
    segment.width = width;
    segment.height = height;
    int video_index = -1, audio_index = -1, result = 0;
    unsigned packets = 0;
    // Reuse the active decoder across continuous segments. A new decoder
    // needs in-band parameter sets and an IDR, which may follow leading AUs.
    std::vector<uint8_t> configuration;
    bool have_sps = false, have_pps = false;
    int64_t leading_dts = AV_NOPTS_VALUE, first_video_pts = 0;
    int64_t audio_pts_anchor = AV_NOPTS_VALUE;
    int64_t last_audio_pts = AV_NOPTS_VALUE;
    int audio_samples = 0;
    unsigned aac_without_pcm = 0;
    const auto timestamp_error = [&](bool video, const char* reason) {
        segment.timestamp_problem = reason;
        return video ? DirectError::VideoTimestamp : DirectError::AudioTimestamp;
    };
    while (!cancelled && (result = av_read_frame(demux.format, demux.packet)) >= 0) {
        if (++packets > max_segment_packets) return DirectError::StreamLimit;
        AVPacket& packet = *demux.packet;
        const AVStream& stream = *demux.format->streams[packet.stream_index];
        const bool video = stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        const bool is_audio = stream.codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
        if (!video && !is_audio) { av_packet_unref(&packet); continue; }
        if (video) segment.video_codec = stream.codecpar->codec_id;
        else segment.audio_codec = stream.codecpar->codec_id;
        if (packet.size <= 0 || size_t(packet.size) > max_packet_bytes) return DirectError::StreamLimit;
        if (packet.flags & AV_PKT_FLAG_CORRUPT) return DirectError::PacketCorrupt;
        const AVRational ns = {1, int(second)};
        DirectAccessUnit unit;
        unit.video = video;
        if (video) {
            if (stream.codecpar->codec_id != AV_CODEC_ID_H264) return DirectError::VideoCodec;
            if (video_index >= 0 && packet.stream_index != video_index) return DirectError::MultipleTracks;
            video_index = packet.stream_index;
            const auto info = inspect_h264(packet.data, size_t(packet.size));
            if (!info.valid) return DirectError::H264AnnexB;
            if (info.width) {
                segment.h264_profile = int(info.profile);
                segment.pixel_format = info.chroma == 1 && info.bit_depth == 8 ? "yuv420p" : "other";
                if (info.height > 1080 || info.width > 1920 || info.chroma != 1 || info.bit_depth != 8)
                    return DirectError::H264Format;
                if (segment.width && (info.width != segment.width || info.height != segment.height))
                    return DirectError::TrackChange;
                segment.width = info.width;
                segment.height = info.height;
            }
            if (!info.configuration.empty()) {
                if (info.width && info.pps) configuration.clear();
                if (configuration.size() + info.configuration.size() > 65536) return DirectError::StreamLimit;
                configuration.insert(configuration.end(), info.configuration.begin(), info.configuration.end());
                have_sps |= info.width != 0;
                have_pps |= info.pps;
                segment.sps_pps |= have_sps && have_pps;
            }
            if (!info.picture) { av_packet_unref(&packet); continue; }
            segment.idr |= info.keyframe;
            if (!continuing && !segment.video_packets && (!info.keyframe || !segment.sps_pps)) {
                if (leading_dts == AV_NOPTS_VALUE && packet.dts != AV_NOPTS_VALUE)
                    leading_dts = av_rescale_q(packet.dts, stream.time_base, ns);
                av_packet_unref(&packet);
                continue;
            }
            if (packet.pts == AV_NOPTS_VALUE) return timestamp_error(true, "missing-pts");
            unit.pts_ns = av_rescale_q(packet.pts, stream.time_base, ns);
            if (packet.dts == AV_NOPTS_VALUE) {
                // A random-access IDR can start the decode timeline at its
                // own PTS. Subsequent decode times still have to keep order.
                if (continuing || segment.video_packets || !info.keyframe) return timestamp_error(true, "missing-dts");
                unit.dts_ns = unit.pts_ns;
                ++segment.inferred_dts;
            } else {
                unit.dts_ns = av_rescale_q(packet.dts, stream.time_base, ns);
            }
            unit.duration_ns = av_rescale_q(packet.duration, stream.time_base, ns);
            if (segment.video_packets && unit.dts_ns < segment.last_video_dts_ns)
                return timestamp_error(true, "decode-order");
            if (segment.video_packets && unit.dts_ns == segment.last_video_dts_ns) ++segment.tied_dts;
            if (!segment.video_packets) {
                segment.first_video_dts_ns = unit.dts_ns;
                segment.starts_with_idr = info.keyframe;
                first_video_pts = unit.pts_ns;
            }
            if (unit.pts_ns < unit.dts_ns) ++segment.reordered;
            segment.last_video_dts_ns = unit.dts_ns;
            ++segment.video_packets;
            if (!(info.width && info.pps)) unit.data.swap(configuration);
            configuration.clear();
            have_sps = have_pps = false;
            unit.data.insert(unit.data.end(), packet.data, packet.data + packet.size);
        } else {
            if (stream.codecpar->codec_id != AV_CODEC_ID_AAC) return DirectError::AudioCodec;
            if (audio_index >= 0 && packet.stream_index != audio_index) return DirectError::MultipleTracks;
            audio_index = packet.stream_index;
            const auto adts = adts_aac_lc(packet);
            if (!adts.sample_rate) return DirectError::AacAdts;
            if (segment.aac_sample_rate && (segment.aac_sample_rate != adts.sample_rate ||
                                            segment.aac_channels != adts.channels)) return DirectError::TrackChange;
            segment.aac_sample_rate = adts.sample_rate;
            segment.aac_channels = adts.channels;
            const AVRational sample_time = {1, adts.sample_rate};
            // MPEG-TS timestamps the PES. Splitting ADTS access units can leave
            // subsequent frames without a timestamp until the next PES anchor.
            if (packet.pts != AV_NOPTS_VALUE) {
                audio_pts_anchor = av_rescale_q(packet.pts, stream.time_base, ns);
                audio_samples = 0;
            }
            if (audio_pts_anchor == AV_NOPTS_VALUE) return timestamp_error(false, "missing-anchor");
            unit.pts_ns = unit.dts_ns = audio_pts_anchor + av_rescale_q(audio_samples, sample_time, ns);
            if (unit.pts_ns <= last_audio_pts) return timestamp_error(false, "presentation-order");
            last_audio_pts = unit.pts_ns;
            audio_samples += 1024;
            unit.duration_ns = av_rescale_q(1024, sample_time, ns);
            if (!audio.decode_adts(packet.data, size_t(packet.size), unit.data))
                return DirectError::AacDecode;
            if (unit.data.empty()) {
                // The first Mux ADTS packet carries encoder data and consumes
                // a timestamp slot, while AAC returns success with no frame.
                ++segment.aac_without_pcm;
                if (++aac_without_pcm > 8) return DirectError::AacNoPcm;
                av_packet_unref(&packet);
                continue;
            }
            aac_without_pcm = 0;
            if (!segment.audio_packets++) segment.first_pcm_bytes = unit.data.size();
        }
        constexpr int64_t max_timestamp = 2 * 24 * 60 * 60 * second;
        if (unit.pts_ns < -second || unit.pts_ns > max_timestamp || unit.dts_ns < -second ||
            unit.dts_ns > max_timestamp || unit.duration_ns < 0 || unit.duration_ns > second)
            return timestamp_error(video, "out-of-range");
        if (unit.pts_ns < unit.dts_ns - second || unit.pts_ns > unit.dts_ns + second)
            return timestamp_error(true, "composition-offset");
        if (unit.data.size() > max_segment_bytes - segment.bytes) return DirectError::StreamLimit;
        segment.bytes += unit.data.size();
        segment.packets.push_back(std::move(unit));
        av_packet_unref(&packet);
    }
    if (cancelled) return DirectError::FetchCancelled;
    if (result != AVERROR_EOF) return DirectError::DemuxRead;
    if (video_index < 0) return DirectError::MissingH264;
    if (!segment.video_packets) return segment.idr ? DirectError::H264Configuration : DirectError::H264Keyframe;
    if (audio_index < 0) return DirectError::MissingAac;
    if (!segment.audio_packets) return DirectError::AacNoPcm;
    if (leading_dts != AV_NOPTS_VALUE) {
        segment.skipped_ns = std::max(int64_t(0), segment.first_video_dts_ns - leading_dts);
        const auto keep = std::remove_if(segment.packets.begin(), segment.packets.end(), [&](const DirectAccessUnit& unit) {
            if (unit.video || unit.pts_ns + unit.duration_ns > first_video_pts) return false;
            segment.bytes -= unit.data.size();
            --segment.audio_packets;
            return true;
        });
        segment.packets.erase(keep, segment.packets.end());
        if (!segment.audio_packets) return DirectError::MissingAac;
    }
    std::stable_sort(segment.packets.begin(), segment.packets.end(), [](const DirectAccessUnit& a, const DirectAccessUnit& b) {
        return a.dts_ns < b.dts_ns;
    });
    if (segment.packets.back().dts_ns - segment.packets.front().dts_ns > 13 * second) return DirectError::StreamLimit;
    return DirectError::None;
}
