// deadpool-decomp: the subset of FFmpeg's API that apu/xma.cpp uses, with no decoder behind it.
// Used when DP_XMA=OFF: avcodec_find_decoder returns null, so XMA voices play silence.
// GPL-3.0-or-later (see COPYING).
#pragma once
#include <cstdint>
#include <cstdlib>

#define AV_INPUT_BUFFER_PADDING_SIZE 64
#define AV_CODEC_FLAG2_SKIP_MANUAL (1 << 29)

enum AVCodecID { AV_CODEC_ID_NONE, AV_CODEC_ID_XMA1, AV_CODEC_ID_XMA2 };

struct AVChannelLayout { int nb_channels; };
struct AVCodec { AVCodecID id; };
struct AVCodecContext
{
    int sample_rate;
    AVChannelLayout ch_layout;
    int block_align;
    int flags2;
    uint8_t* extradata;
    int extradata_size;
};
struct AVPacket { uint8_t* data; int size; };
struct AVFrame { int nb_samples; AVChannelLayout ch_layout; uint8_t** extended_data; };

inline const AVCodec* avcodec_find_decoder(AVCodecID) { return nullptr; }
inline AVPacket* av_packet_alloc() { return nullptr; }
inline AVFrame* av_frame_alloc() { return nullptr; }
inline AVCodecContext* avcodec_alloc_context3(const AVCodec*) { return nullptr; }
inline void avcodec_free_context(AVCodecContext** c) { *c = nullptr; }
inline int avcodec_open2(AVCodecContext*, const AVCodec*, void*) { return -1; }
inline void* av_mallocz(size_t n) { return std::calloc(1, n); }
inline void av_packet_unref(AVPacket*) {}
inline void av_frame_unref(AVFrame*) {}
inline int avcodec_send_packet(AVCodecContext*, const AVPacket*) { return -1; }
inline int avcodec_receive_frame(AVCodecContext*, AVFrame*) { return -1; }
inline void avcodec_flush_buffers(AVCodecContext*) {}
