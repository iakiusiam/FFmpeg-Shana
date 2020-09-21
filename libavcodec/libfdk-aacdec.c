/*
 * AAC decoder wrapper
 * Copyright (c) 2012 Martin Storsjo
 *
 * This file is part of FFmpeg.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _WIN32
#include <dlfcn.h>
#define LIBNAME "libfdk-aac.so.0"
#else
#include <windows.h>
#define LIBNAME "libfdk-aac-0.dll"
#define dlopen(fname, f) ((void *) LoadLibraryA(fname))
#define dlclose(handle) FreeLibrary((HMODULE) handle)
#define dlsym(handle, name) GetProcAddress((HMODULE) handle, name)
#endif

#include <fdk-aac/aacdecoder_lib.h>

#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "internal.h"

/* The version macro is introduced the same time as the setting enum was
 * changed, so this check should suffice. */
#ifndef AACDECODER_LIB_VL0
#define AAC_PCM_MAX_OUTPUT_CHANNELS AAC_PCM_OUTPUT_CHANNELS
#endif

typedef LINKSPEC_H HANDLE_AACDECODER (*imp_aacDecoder_Open)(TRANSPORT_TYPE transportFmt, UINT nrOfLayers);
typedef LINKSPEC_H void (*imp_aacDecoder_Close)(HANDLE_AACDECODER self);
typedef LINKSPEC_H AAC_DECODER_ERROR (*imp_aacDecoder_Fill)(HANDLE_AACDECODER self, UCHAR *pBuffer[], const UINT bufferSize[], UINT *bytesValid);
typedef LINKSPEC_H AAC_DECODER_ERROR (*imp_aacDecoder_DecodeFrame)(HANDLE_AACDECODER self, INT_PCM *pTimeData, const INT timeDataSize, const UINT flags);
typedef LINKSPEC_H CStreamInfo* (*imp_aacDecoder_GetStreamInfo)(HANDLE_AACDECODER self);
typedef LINKSPEC_H AAC_DECODER_ERROR (*imp_aacDecoder_ConfigRaw)(HANDLE_AACDECODER self, UCHAR *conf[], const UINT length[]);
typedef LINKSPEC_H AAC_DECODER_ERROR (*imp_aacDecoder_SetParam)(const HANDLE_AACDECODER self, const AACDEC_PARAM param, const INT value);

typedef struct _aacDecLib {
    imp_aacDecoder_Open aacDecoder_Open;
    imp_aacDecoder_Close aacDecoder_Close;
    imp_aacDecoder_Fill aacDecoder_Fill;
    imp_aacDecoder_DecodeFrame aacDecoder_DecodeFrame;
    imp_aacDecoder_ConfigRaw aacDecoder_ConfigRaw;
    imp_aacDecoder_GetStreamInfo aacDecoder_GetStreamInfo;
    imp_aacDecoder_SetParam aacDecoder_SetParam;
} aacDecLib;

#define DLSYM(x) \
    do \
    { \
        s->pfn.x = ( imp_##x ) dlsym(s->hLib, AV_STRINGIFY(x)); \
        if (!s->pfn.x ) \
        { \
            av_log(avctx, AV_LOG_ERROR, "Unable to find symbol " AV_STRINGIFY(x) " in dynamic " LIBNAME "\n"); \
            return -1; \
        } \
    } while (0)

enum ConcealMethod {
    CONCEAL_METHOD_SPECTRAL_MUTING      =  0,
    CONCEAL_METHOD_NOISE_SUBSTITUTION   =  1,
    CONCEAL_METHOD_ENERGY_INTERPOLATION =  2,
    CONCEAL_METHOD_NB,
};

typedef struct FDKAACDecContext {
    const AVClass *class;
    HANDLE_AACDECODER handle;
    int initialized;
    uint8_t *decoder_buffer;
    uint8_t *anc_buffer;
    int conceal_method;
    int drc_level;
    int drc_boost;
    int drc_heavy;
    int drc_cut;
    int level_limit;
    void *hLib;
    aacDecLib pfn;
} FDKAACDecContext;


#define DMX_ANC_BUFFSIZE       128
#define DECODER_MAX_CHANNELS     6
#define DECODER_BUFFSIZE      2048 * sizeof(INT_PCM)

#define OFFSET(x) offsetof(FDKAACDecContext, x)
#define AD AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption fdk_aac_dec_options[] = {
    { "conceal", "Error concealment method", OFFSET(conceal_method), AV_OPT_TYPE_INT, { .i64 = CONCEAL_METHOD_NOISE_SUBSTITUTION }, CONCEAL_METHOD_SPECTRAL_MUTING, CONCEAL_METHOD_NB - 1, AD, "conceal" },
    { "spectral", "Spectral muting",      0, AV_OPT_TYPE_CONST, { .i64 = CONCEAL_METHOD_SPECTRAL_MUTING },      INT_MIN, INT_MAX, AD, "conceal" },
    { "noise",    "Noise Substitution",   0, AV_OPT_TYPE_CONST, { .i64 = CONCEAL_METHOD_NOISE_SUBSTITUTION },   INT_MIN, INT_MAX, AD, "conceal" },
    { "energy",   "Energy Interpolation", 0, AV_OPT_TYPE_CONST, { .i64 = CONCEAL_METHOD_ENERGY_INTERPOLATION }, INT_MIN, INT_MAX, AD, "conceal" },
    { "drc_boost", "Dynamic Range Control: boost, where [0] is none and [127] is max boost",
                     OFFSET(drc_boost),      AV_OPT_TYPE_INT,   { .i64 = -1 }, -1, 127, AD, NULL    },
    { "drc_cut",   "Dynamic Range Control: attenuation factor, where [0] is none and [127] is max compression",
                     OFFSET(drc_cut),        AV_OPT_TYPE_INT,   { .i64 = -1 }, -1, 127, AD, NULL    },
    { "drc_level", "Dynamic Range Control: reference level, quantized to 0.25dB steps where [0] is 0dB and [127] is -31.75dB",
                     OFFSET(drc_level),      AV_OPT_TYPE_INT,   { .i64 = -1},  -1, 127, AD, NULL    },
    { "drc_heavy", "Dynamic Range Control: heavy compression, where [1] is on (RF mode) and [0] is off",
                     OFFSET(drc_heavy),      AV_OPT_TYPE_INT,   { .i64 = -1},  -1, 1,   AD, NULL    },
#ifdef AACDECODER_LIB_VL0
    { "level_limit", "Signal level limiting", OFFSET(level_limit), AV_OPT_TYPE_INT, { .i64 = 0 }, -1, 1, AD },
#endif
    { NULL }
};

static const AVClass fdk_aac_dec_class = {
    "libfdk-aac decoder", av_default_item_name, fdk_aac_dec_options, LIBAVUTIL_VERSION_INT
};

static int get_stream_info(AVCodecContext *avctx)
{
    FDKAACDecContext *s   = avctx->priv_data;
    CStreamInfo *info     = s->pfn.aacDecoder_GetStreamInfo(s->handle);
    int channel_counts[0x24] = { 0 };
    int i, ch_error       = 0;
    uint64_t ch_layout    = 0;

    if (!info) {
        av_log(avctx, AV_LOG_ERROR, "Unable to get stream info\n");
        return AVERROR_UNKNOWN;
    }

    if (info->sampleRate <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Stream info not initialized\n");
        return AVERROR_UNKNOWN;
    }
    avctx->sample_rate = info->sampleRate;
    avctx->frame_size  = info->frameSize;

    for (i = 0; i < info->numChannels; i++) {
        AUDIO_CHANNEL_TYPE ctype = info->pChannelType[i];
        if (ctype <= ACT_NONE || ctype >= FF_ARRAY_ELEMS(channel_counts)) {
            av_log(avctx, AV_LOG_WARNING, "unknown channel type\n");
            break;
        }
        channel_counts[ctype]++;
    }
    av_log(avctx, AV_LOG_DEBUG,
           "%d channels - front:%d side:%d back:%d lfe:%d top:%d\n",
           info->numChannels,
           channel_counts[ACT_FRONT], channel_counts[ACT_SIDE],
           channel_counts[ACT_BACK],  channel_counts[ACT_LFE],
           channel_counts[ACT_FRONT_TOP] + channel_counts[ACT_SIDE_TOP] +
           channel_counts[ACT_BACK_TOP]  + channel_counts[ACT_TOP]);

    switch (channel_counts[ACT_FRONT]) {
    case 4:
        ch_layout |= AV_CH_LAYOUT_STEREO | AV_CH_FRONT_LEFT_OF_CENTER |
                     AV_CH_FRONT_RIGHT_OF_CENTER;
        break;
    case 3:
        ch_layout |= AV_CH_LAYOUT_STEREO | AV_CH_FRONT_CENTER;
        break;
    case 2:
        ch_layout |= AV_CH_LAYOUT_STEREO;
        break;
    case 1:
        ch_layout |= AV_CH_FRONT_CENTER;
        break;
    default:
        av_log(avctx, AV_LOG_WARNING,
               "unsupported number of front channels: %d\n",
               channel_counts[ACT_FRONT]);
        ch_error = 1;
        break;
    }
    if (channel_counts[ACT_SIDE] > 0) {
        if (channel_counts[ACT_SIDE] == 2) {
            ch_layout |= AV_CH_SIDE_LEFT | AV_CH_SIDE_RIGHT;
        } else {
            av_log(avctx, AV_LOG_WARNING,
                   "unsupported number of side channels: %d\n",
                   channel_counts[ACT_SIDE]);
            ch_error = 1;
        }
    }
    if (channel_counts[ACT_BACK] > 0) {
        switch (channel_counts[ACT_BACK]) {
        case 3:
            ch_layout |= AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT | AV_CH_BACK_CENTER;
            break;
        case 2:
            ch_layout |= AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT;
            break;
        case 1:
            ch_layout |= AV_CH_BACK_CENTER;
            break;
        default:
            av_log(avctx, AV_LOG_WARNING,
                   "unsupported number of back channels: %d\n",
                   channel_counts[ACT_BACK]);
            ch_error = 1;
            break;
        }
    }
    if (channel_counts[ACT_LFE] > 0) {
        if (channel_counts[ACT_LFE] == 1) {
            ch_layout |= AV_CH_LOW_FREQUENCY;
        } else {
            av_log(avctx, AV_LOG_WARNING,
                   "unsupported number of LFE channels: %d\n",
                   channel_counts[ACT_LFE]);
            ch_error = 1;
        }
    }
    if (!ch_error &&
        av_get_channel_layout_nb_channels(ch_layout) != info->numChannels) {
        av_log(avctx, AV_LOG_WARNING, "unsupported channel configuration\n");
        ch_error = 1;
    }
    if (ch_error)
        avctx->channel_layout = 0;
    else
        avctx->channel_layout = ch_layout;

    avctx->channels = info->numChannels;

    return 0;
}

static av_cold int fdk_aac_decode_close(AVCodecContext *avctx)
{
    FDKAACDecContext *s = avctx->priv_data;

    if (s->hLib && s->handle) {
        s->pfn.aacDecoder_Close(s->handle);
        dlclose(s->hLib);
    }
    av_freep(&s->decoder_buffer);
    av_freep(&s->anc_buffer);

    return 0;
}

static av_cold int fdk_aac_decode_init(AVCodecContext *avctx)
{
    FDKAACDecContext *s = avctx->priv_data;
    AAC_DECODER_ERROR err;
    int ret;

    if (!(s->hLib = dlopen(LIBNAME, RTLD_NOW))) {
        av_log(avctx, AV_LOG_ERROR, "Unable to load " LIBNAME "\n");
        return -1;
    }

    DLSYM(aacDecoder_Open);
#define aacDecoder_Open s->pfn.aacDecoder_Open
    DLSYM(aacDecoder_Close);
    DLSYM(aacDecoder_Fill);
#define aacDecoder_Fill s->pfn.aacDecoder_Fill
    DLSYM(aacDecoder_DecodeFrame);
#define aacDecoder_DecodeFrame s->pfn.aacDecoder_DecodeFrame
    DLSYM(aacDecoder_GetStreamInfo);
#define aacDecoder_GetStreamInfo s->pfn.aacDecoder_GetStreamInfo
    DLSYM(aacDecoder_ConfigRaw);
#define aacDecoder_ConfigRaw s->pfn.aacDecoder_ConfigRaw
    DLSYM(aacDecoder_SetParam);
#define aacDecoder_SetParam s->pfn.aacDecoder_SetParam

    s->handle = aacDecoder_Open(avctx->extradata_size ? TT_MP4_RAW : TT_MP4_ADTS, 1);
    if (!s->handle) {
        av_log(avctx, AV_LOG_ERROR, "Error opening decoder\n");
        return AVERROR_UNKNOWN;
    }

    if (avctx->extradata_size) {
        if ((err = aacDecoder_ConfigRaw(s->handle, &avctx->extradata,
                                        &avctx->extradata_size)) != AAC_DEC_OK) {
            av_log(avctx, AV_LOG_ERROR, "Unable to set extradata\n");
            return AVERROR_INVALIDDATA;
        }
    }

    if ((err = aacDecoder_SetParam(s->handle, AAC_CONCEAL_METHOD,
                                   s->conceal_method)) != AAC_DEC_OK) {
        av_log(avctx, AV_LOG_ERROR, "Unable to set error concealment method\n");
        return AVERROR_UNKNOWN;
    }

    if (avctx->request_channel_layout > 0 &&
        avctx->request_channel_layout != AV_CH_LAYOUT_NATIVE) {
        int downmix_channels = -1;

        switch (avctx->request_channel_layout) {
        case AV_CH_LAYOUT_STEREO:
        case AV_CH_LAYOUT_STEREO_DOWNMIX:
            downmix_channels = 2;
            break;
        case AV_CH_LAYOUT_MONO:
            downmix_channels = 1;
            break;
        default:
            av_log(avctx, AV_LOG_WARNING, "Invalid request_channel_layout\n");
            break;
        }

        if (downmix_channels != -1) {
            if (aacDecoder_SetParam(s->handle, AAC_PCM_MAX_OUTPUT_CHANNELS,
                                    downmix_channels) != AAC_DEC_OK) {
               av_log(avctx, AV_LOG_WARNING, "Unable to set output channels in the decoder\n");
            } else {
               s->anc_buffer = av_malloc(DMX_ANC_BUFFSIZE);
               if (!s->anc_buffer) {
                   av_log(avctx, AV_LOG_ERROR, "Unable to allocate ancillary buffer for the decoder\n");
                   ret = AVERROR(ENOMEM);
                   goto fail;
               }
               if (aacDecoder_AncDataInit(s->handle, s->anc_buffer, DMX_ANC_BUFFSIZE)) {
                   av_log(avctx, AV_LOG_ERROR, "Unable to register downmix ancillary buffer in the decoder\n");
                   ret = AVERROR_UNKNOWN;
                   goto fail;
               }
            }
        }
    }

    if (s->drc_boost != -1) {
        if (aacDecoder_SetParam(s->handle, AAC_DRC_BOOST_FACTOR, s->drc_boost) != AAC_DEC_OK) {
            av_log(avctx, AV_LOG_ERROR, "Unable to set DRC boost factor in the decoder\n");
            return AVERROR_UNKNOWN;
        }
    }

    if (s->drc_cut != -1) {
        if (aacDecoder_SetParam(s->handle, AAC_DRC_ATTENUATION_FACTOR, s->drc_cut) != AAC_DEC_OK) {
            av_log(avctx, AV_LOG_ERROR, "Unable to set DRC attenuation factor in the decoder\n");
            return AVERROR_UNKNOWN;
        }
    }

    if (s->drc_level != -1) {
        if (aacDecoder_SetParam(s->handle, AAC_DRC_REFERENCE_LEVEL, s->drc_level) != AAC_DEC_OK) {
            av_log(avctx, AV_LOG_ERROR, "Unable to set DRC reference level in the decoder\n");
            return AVERROR_UNKNOWN;
        }
    }

    if (s->drc_heavy != -1) {
        if (aacDecoder_SetParam(s->handle, AAC_DRC_HEAVY_COMPRESSION, s->drc_heavy) != AAC_DEC_OK) {
            av_log(avctx, AV_LOG_ERROR, "Unable to set DRC heavy compression in the decoder\n");
            return AVERROR_UNKNOWN;
        }
    }

#ifdef AACDECODER_LIB_VL0
    if (aacDecoder_SetParam(s->handle, AAC_PCM_LIMITER_ENABLE, s->level_limit) != AAC_DEC_OK) {
        av_log(avctx, AV_LOG_ERROR, "Unable to set in signal level limiting in the decoder\n");
        return AVERROR_UNKNOWN;
    }
#endif

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;

    return 0;
fail:
    fdk_aac_decode_close(avctx);
    return ret;
}

static int fdk_aac_decode_frame(AVCodecContext *avctx, void *data,
                                int *got_frame_ptr, AVPacket *avpkt)
{
    FDKAACDecContext *s = avctx->priv_data;
    AVFrame *frame = data;
    int ret;
    AAC_DECODER_ERROR err;
    UINT valid = avpkt->size;
    uint8_t *buf, *tmpptr = NULL;
    int buf_size;

    err = aacDecoder_Fill(s->handle, &avpkt->data, &avpkt->size, &valid);
    if (err != AAC_DEC_OK) {
        av_log(avctx, AV_LOG_ERROR, "aacDecoder_Fill() failed: %x\n", err);
        return AVERROR_INVALIDDATA;
    }

    if (s->initialized) {
        frame->nb_samples = avctx->frame_size;
        if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
            return ret;

        if (s->anc_buffer) {
            buf_size = DECODER_BUFFSIZE * DECODER_MAX_CHANNELS;
            buf = s->decoder_buffer;
        } else {
            buf = frame->extended_data[0];
            buf_size = avctx->channels * frame->nb_samples *
                       av_get_bytes_per_sample(avctx->sample_fmt);
        }
    } else {
        buf_size = DECODER_BUFFSIZE * DECODER_MAX_CHANNELS;

        if (!s->decoder_buffer)
            s->decoder_buffer = av_malloc(buf_size);
        if (!s->decoder_buffer)
            return AVERROR(ENOMEM);

        buf = tmpptr = s->decoder_buffer;
    }

    err = aacDecoder_DecodeFrame(s->handle, (INT_PCM *) buf, buf_size, 0);
    if (err == AAC_DEC_NOT_ENOUGH_BITS) {
        ret = avpkt->size - valid;
        goto end;
    }
    if (err != AAC_DEC_OK) {
        av_log(avctx, AV_LOG_ERROR,
               "aacDecoder_DecodeFrame() failed: %x\n", err);
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    if (!s->initialized) {
        if ((ret = get_stream_info(avctx)) < 0)
            goto end;
        s->initialized = 1;
        frame->nb_samples = avctx->frame_size;
    }

    if (tmpptr) {
        frame->nb_samples = avctx->frame_size;
        if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
            goto end;
    }
    if (s->decoder_buffer) {
        memcpy(frame->extended_data[0], buf,
               avctx->channels * avctx->frame_size *
               av_get_bytes_per_sample(avctx->sample_fmt));

        if (!s->anc_buffer)
            av_freep(&s->decoder_buffer);
    }

    *got_frame_ptr = 1;
    ret = avpkt->size - valid;

end:
    return ret;
}

static av_cold void fdk_aac_decode_flush(AVCodecContext *avctx)
{
    FDKAACDecContext *s = avctx->priv_data;
    AAC_DECODER_ERROR err;

    if (!s->handle)
        return;

    if ((err = aacDecoder_SetParam(s->handle,
                                   AAC_TPDEC_CLEAR_BUFFER, 1)) != AAC_DEC_OK)
        av_log(avctx, AV_LOG_WARNING, "failed to clear buffer when flushing\n");
}

AVCodec ff_libfdk_aac_decoder = {
    .name           = "libfdk_aac",
    .long_name      = NULL_IF_CONFIG_SMALL("Fraunhofer FDK AAC"),
    .type           = AVMEDIA_TYPE_AUDIO,
    .id             = AV_CODEC_ID_AAC,
    .priv_data_size = sizeof(FDKAACDecContext),
    .init           = fdk_aac_decode_init,
    .decode         = fdk_aac_decode_frame,
    .close          = fdk_aac_decode_close,
    .flush          = fdk_aac_decode_flush,
    .capabilities   = CODEC_CAP_DR1 | CODEC_CAP_CHANNEL_CONF,
    .priv_class     = &fdk_aac_dec_class,
};
