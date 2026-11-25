#include "audio.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#include <cstdio>
#include <cstring>
namespace { constexpr unsigned adts_aac = 9, mp4_aac = 10; }
AudioDecoder::~AudioDecoder() { reset(); }
void AudioDecoder::reset() {
    swr_free(&resampler_); avcodec_free_context(&context_); codec_ = 0;
}
bool AudioDecoder::configure(unsigned codec, const uint8_t* asc, size_t asc_size, int rate, int channels) {
    reset();
    AVCodecID id = codec == 2 ? AV_CODEC_ID_ALAC : AV_CODEC_ID_AAC;
    if (codec != 2 && codec != 4 && codec != 8 && codec != adts_aac && codec != mp4_aac) return false;
    if (codec==mp4_aac && (!asc||asc_size<2||asc_size>64||rate<=0||(channels!=1&&channels!=2))) return false;
    const AVCodec *decoder = avcodec_find_decoder(id);
    if (!decoder || !(context_ = avcodec_alloc_context3(decoder))) return false;
    const uint8_t eld[] = {0xf8,0xe8,0x50,0x00};
    const uint8_t lc[] = {0x12,0x10};
    const uint8_t alac[] = {0,0,0,0x24,0x61,0x6c,0x61,0x63,0,0,0,0,0,0,1,0x60,0,0x10,0x28,0x0a,0x0e,2,0,0xff,0,0,0,0,0,0,0,0,0,0,0xac,0x44};
    const uint8_t *config = codec==2?alac:codec==8?eld:lc;
    size_t length = codec==2?sizeof(alac):codec==8?sizeof(eld):sizeof(lc);
    if (codec==mp4_aac) { config=asc; length=asc_size; }
    if (codec != adts_aac) {
        context_->extradata = static_cast<uint8_t *>(av_mallocz(length + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!context_->extradata) { reset(); return false; }
        memcpy(context_->extradata,config,length); context_->extradata_size=length;
        context_->sample_rate=codec==mp4_aac?rate:44100;
        av_channel_layout_default(&context_->ch_layout,codec==mp4_aac?channels:2);
    }
    if (avcodec_open2(context_,decoder,nullptr)<0) { reset(); return false; }
    codec_=codec;
    input_rate_=rate;input_channels_=channels;
    fprintf(stderr,"[audio] decoder=%s ct=%u\n",decoder->name,codec);
    return true;
}
bool AudioDecoder::decode_adts(const uint8_t *data, size_t size, std::vector<uint8_t>& pcm) {
    return decode_packet(adts_aac, data, size, pcm);
}
bool AudioDecoder::decode_mp4_aac(const uint8_t *data, size_t size, const uint8_t *config, size_t config_size,
                                  int rate, int channels, std::vector<uint8_t>& pcm) {
    if (codec_!=mp4_aac && !configure(mp4_aac,config,config_size,rate,channels)) return false;
    return decode_packet(mp4_aac,data,size,pcm);
}
bool AudioDecoder::decode(unsigned codec, const uint8_t *data, size_t size, std::vector<uint8_t>& pcm) {
    if (codec == adts_aac||codec == mp4_aac) { pcm.clear(); return false; }
    return decode_packet(codec, data, size, pcm);
}
bool AudioDecoder::decode_packet(unsigned codec, const uint8_t *data, size_t size, std::vector<uint8_t>& pcm) {
    pcm.clear();
    if (!data || !size || size>1048576) return false;
    if (codec==1) {
        if (size%4) return false;
        pcm.assign(data,data+size); return true;
    }
    if (codec_!=codec && !configure(codec)) return false;
    AVPacket *packet=av_packet_alloc(); AVFrame *frame=av_frame_alloc();
    if (!packet || !frame) { av_packet_free(&packet); av_frame_free(&frame); return false; }
    bool ok = av_new_packet(packet,size)>=0;
    if (ok) { memcpy(packet->data,data,size); ok=avcodec_send_packet(context_,packet)>=0; }
    while (ok) {
        int r=avcodec_receive_frame(context_,frame);
        if (r==AVERROR(EAGAIN) || r==AVERROR_EOF) break;
        if (r<0) { ok=false; break; }
        if (codec==mp4_aac && (frame->sample_rate!=input_rate_||frame->ch_layout.nb_channels!=input_channels_)) {
            ok=false;break;
        }
        if (!resampler_) {
            AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&resampler_,&stereo,AV_SAMPLE_FMT_S16,44100,
                &frame->ch_layout,static_cast<AVSampleFormat>(frame->format),frame->sample_rate,0,nullptr)<0 || swr_init(resampler_)<0) {
                ok=false; break;
            }
        }
        int capacity=swr_get_out_samples(resampler_,frame->nb_samples);
        if (capacity<0 || capacity>65536) { ok=false; break; }
        size_t offset=pcm.size(); pcm.resize(offset+capacity*4);
        uint8_t *output=pcm.data()+offset;
        int samples=swr_convert(resampler_,&output,capacity,const_cast<const uint8_t **>(frame->extended_data),frame->nb_samples);
        if (samples<0) { ok=false; break; }
        pcm.resize(offset+samples*4);
        av_frame_unref(frame);
    }
    av_frame_free(&frame); av_packet_free(&packet);
    return ok;
}
