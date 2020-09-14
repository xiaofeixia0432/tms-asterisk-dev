/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Sergio Garcia Murillo <sergio.garcia@fontventa.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief MP4 application -- save and play mp4 files
 *
 * \ingroup applications
 */

#include <asterisk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "asterisk/app.h"
#include "asterisk/ast_version.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/file.h"
#include "asterisk/format_cache.h"         // added by yy
#include "asterisk/format_compatibility.h" // added by yy
#include "asterisk/lock.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/options.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>

#include "app_tms.h"

static const char *app_play = "TMSMp4Play";
static const char *syn_play = "MP4 file playblack";
static const char *des_play = "  TMSMp4Play(filename,[options]):  Play mp4 file to user. \n";

#define AST_FRAME_GET_BUFFER(fr) ((uint8_t *)((fr)->data.ptr))

#define PKT_PAYLOAD 1460
#define PKT_SIZE (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET + PKT_PAYLOAD)
#define PKT_OFFSET (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET)

#define MAX_SAMPLES_IN_AST_FRAME 320 // 每个帧里最多放多少个采样

#define FF_RTP_FLAG_H264_MODE0 8

#define ALAW_BIT_RATE 64000   // alaw比特率
#define ALAW_SAMPLE_RATE 8000 // alaw采样率
#define BYTES_PER_SAMPLE 1    // 每个采样的字节数
#define MAX_PKT_SAMPLES 320   // 每个rtp帧中包含的采样数

/* 记录输入媒体流相关数据 */
typedef struct TmsInputStream
{
  int stream_index;
  AVStream *st;
  AVCodecContext *dec_ctx;
  AVCodec *codec;
  int bytes_per_sample;

  int saw_first_ts;

  int64_t start; /* time when read started */
  /* predicted dts of the next packet read for this stream or (when there are several frames in a packet) of the next frame in current packet (in AV_TIME_BASE units) */
  int64_t next_dts;
  int64_t dts; ///< dts of the last packet read for this stream (in AV_TIME_BASE units)
} TmsInputStream;
/**
 * 记录播放过程全局数据 
 */
typedef struct TmsPlayerContext
{
  struct ast_channel *chan;
  char *src;
  /* 计数器 */
  int64_t start_time;
  int64_t end_time;
  int nb_packets;
  int nb_video_packets;
  int nb_video_rtps;
  int nb_audio_packets;
  int nb_audio_frames;
  int nb_pcma_frames;
  int nb_audio_rtp_samples;
  int nb_audio_rtps;
} TmsPlayerContext;
/**
 * 重采样 
 */
typedef struct Resampler
{
  SwrContext *swrctx;
  int max_nb_samples; // 重采样缓冲区最大采样数
  int linesize;       // 声道平面尺寸
  uint8_t **data;     // 重采样缓冲区
} Resampler;
/**
 * PCMA编码器 
 */
typedef struct PCMAEnc
{
  AVCodec *codec;
  AVCodecContext *cctx;
  int nb_samples;
  AVFrame *frame;
  AVPacket packet;
} PCMAEnc;

/**
 * 记录视频RTP发送相关数据 
 */
typedef struct TmsVideoRtpContext
{
  const AVClass *av_class;
  AVFormatContext *ic;
  AVStream *st;
  int payload_type;
  uint32_t ssrc;
  const char *cname;
  int seq;
  uint32_t timestamp;
  uint32_t base_timestamp;
  uint32_t cur_timestamp;
  int max_payload_size;
  int num_frames;

  /* rtcp sender statistics */
  int64_t last_rtcp_ntp_time;
  int64_t first_rtcp_ntp_time;
  unsigned int packet_count;
  unsigned int octet_count;
  unsigned int last_octet_count;
  int first_packet;
  /* buffer for output */
  uint8_t *buf;
  uint8_t *buf_ptr;

  int max_frames_per_packet;

  /**
    * Number of bytes used for H.264 NAL length, if the MP4 syntax is used
    * (1, 2 or 4)
    */
  // int nal_length_size;
  int buffered_nals;

  int flags;
} TmsVideoRtpContext;

static void tms_rtp_send_video(TmsVideoRtpContext *s, const uint8_t *buf1, int len, int m, TmsPlayerContext *player)
{
  ast_debug(1, "进入ff_rtp_send_data len=%d M=%d\n", len, m);

  uint8_t *data;

  uint8_t buffer[PKT_SIZE];
  struct ast_frame *f = (struct ast_frame *)buffer;
  s->timestamp = s->cur_timestamp;

  /* Unset */
  memset(f, 0, PKT_SIZE);

  AST_FRAME_SET_BUFFER(f, f, PKT_OFFSET, PKT_PAYLOAD);

  f->src = strdup(player->src);
  /* Set type */
  f->frametype = AST_FRAME_VIDEO;
  /* Set number of samples */
  f->samples = 1;
  f->delivery.tv_usec = 0;
  f->delivery.tv_sec = 0;
  // 告知asterisk使用指定的时间戳
  ast_set_flag(f, AST_FRFLAG_HAS_TIMING_INFO);
  f->ts = s->timestamp;
  /* Don't free the frame outside */
  f->mallocd = 0;
  //f->subclass.format = s->format;
  f->subclass.format = ast_format_h264;
  f->subclass.frame_ending = m;
  f->offset = AST_FRIENDLY_OFFSET;

  /* Get data pointer */
  data = AST_FRAME_GET_BUFFER(f);

  f->datalen = len;

  memcpy(data, buf1, len);

  /* Write frame */
  ast_write(player->chan, f);

  ast_frfree(f);

  player->nb_video_rtps++;

  ast_debug(2, "[chan %p] 完成第 %d 个视频RTP帧发送 \n", player->chan, player->nb_video_rtps);
}
/* 将多个nal缓存起来一起发送 */
static void flush_nal_buffered(TmsVideoRtpContext *s, int last, TmsPlayerContext *player)
{
  if (s->buf_ptr != s->buf)
  {
    // If we're only sending one single NAL unit, send it as such, skip
    // the STAP-A/AP framing
    if (s->buffered_nals == 1)
    {
      tms_rtp_send_video(s, s->buf + 3, s->buf_ptr - s->buf - 3, last, player);
    }
    else
    {
      tms_rtp_send_video(s, s->buf, s->buf_ptr - s->buf, last, player);
    }
  }
  s->buf_ptr = s->buf;
  s->buffered_nals = 0;
}
/* 发送nal */
static void h264_nal_send(TmsVideoRtpContext *s, const uint8_t *buf, int size, int last, TmsPlayerContext *player)
{
  int nalu_type = buf[0] & 0x1F;
  ast_debug(1, "Sending NAL %x of len %d M=%d\n", nalu_type, size, last);

  if (size <= s->max_payload_size)
  {
    int buffered_size = s->buf_ptr - s->buf;
    int header_size;
    int skip_aggregate = 1;

    header_size = 1;
    //skip_aggregate = s->flags & FF_RTP_FLAG_H264_MODE0;

    // Flush buffered NAL units if the current unit doesn't fit
    if (buffered_size + 2 + size > s->max_payload_size)
    {
      flush_nal_buffered(s, 0, player);
      buffered_size = 0;
    }
    // If we aren't using mode 0, and the NAL unit fits including the
    // framing (2 bytes length, plus 1/2 bytes for the STAP-A/AP marker),
    // write the unit to the buffer as a STAP-A/AP packet, otherwise flush
    // and send as single NAL.
    if (buffered_size + 2 + header_size + size <= s->max_payload_size &&
        !skip_aggregate)
    {
      if (buffered_size == 0)
      {
        *s->buf_ptr++ = 24;
      }
      AV_WB16(s->buf_ptr, size);
      s->buf_ptr += 2;
      memcpy(s->buf_ptr, buf, size);
      s->buf_ptr += size;
      s->buffered_nals++;
    }
    else
    {
      flush_nal_buffered(s, 0, player);
      tms_rtp_send_video(s, buf, size, last, player);
    }
  }
  else
  {
    int flag_byte, header_size;
    flush_nal_buffered(s, 0, player);
    if (s->flags & FF_RTP_FLAG_H264_MODE0)
    {
      ast_debug(1, "NAL size %d > %d, try -slice-max-size %d\n", size, s->max_payload_size, s->max_payload_size);
      return;
    }
    ast_debug(1, "NAL size %d > %d\n", size, s->max_payload_size);

    uint8_t type = buf[0] & 0x1F;
    uint8_t nri = buf[0] & 0x60;

    s->buf[0] = 28; /* FU Indicator; Type = 28 ---> FU-A */
    s->buf[0] |= nri;
    s->buf[1] = type;
    s->buf[1] |= 1 << 7;
    buf += 1;
    size -= 1;

    flag_byte = 1;
    header_size = 2;

    while (size + header_size > s->max_payload_size)
    {
      memcpy(&s->buf[header_size], buf, s->max_payload_size - header_size);
      tms_rtp_send_video(s, s->buf, s->max_payload_size, 0, player);
      buf += s->max_payload_size - header_size;
      size -= s->max_payload_size - header_size;
      s->buf[flag_byte] &= ~(1 << 7);
    }
    s->buf[flag_byte] |= 1 << 6;
    memcpy(&s->buf[header_size], buf, size);
    tms_rtp_send_video(s, s->buf, size + header_size, last, player);
  }
}

static const uint8_t *ff_avc_find_startcode_internal(const uint8_t *p, const uint8_t *end)
{
  const uint8_t *a = p + 4 - ((intptr_t)p & 3);

  for (end -= 3; p < a && p < end; p++)
  {
    if (p[0] == 0 && p[1] == 0 && p[2] == 1)
      return p;
  }

  for (end -= 3; p < end; p += 4)
  {
    uint32_t x = *(const uint32_t *)p;
    //      if ((x - 0x01000100) & (~x) & 0x80008000) // little endian
    //      if ((x - 0x00010001) & (~x) & 0x00800080) // big endian
    if ((x - 0x01010101) & (~x) & 0x80808080)
    { // generic
      if (p[1] == 0)
      {
        if (p[0] == 0 && p[2] == 1)
          return p;
        if (p[2] == 0 && p[3] == 1)
          return p + 1;
      }
      if (p[3] == 0)
      {
        if (p[2] == 0 && p[4] == 1)
          return p + 2;
        if (p[4] == 0 && p[5] == 1)
          return p + 3;
      }
    }
  }

  for (end += 3; p < end; p++)
  {
    if (p[0] == 0 && p[1] == 0 && p[2] == 1)
      return p;
  }

  return end + 3;
}

static const uint8_t *ff_avc_find_startcode(const uint8_t *p, const uint8_t *end)
{
  const uint8_t *out = ff_avc_find_startcode_internal(p, end);
  if (p < out && out < end && !out[-1])
    out--;
  return out;
}

static void ff_rtp_send_h264(TmsVideoRtpContext *s, const uint8_t *buf1, int size, TmsPlayerContext *player)
{
  const uint8_t *r, *end = buf1 + size;

  s->buf_ptr = s->buf;

  r = ff_avc_find_startcode(buf1, end);
  while (r < end)
  {
    const uint8_t *r1;

    while (!*(r++))
      ;
    r1 = ff_avc_find_startcode(r, end);
    h264_nal_send(s, r, r1 - r, r1 == end, player);
    ast_debug(1, "ff_rtp_send_h264.h264_nal_send r = %p r1 = %p end = %p\n", r, r1, end);
    r = r1;
  }

  flush_nal_buffered(s, 1, player);
}
/* 输出媒体流格式信息 */
static void tms_dump_stream_format(TmsInputStream *ist)
{
  AVStream *st = ist->st;
  AVCodecContext *cctx = ist->dec_ctx;
  AVCodec *codec = ist->codec;

  ast_debug(1, "媒体流 #%d is %s\n", st->index, codec->type == AVMEDIA_TYPE_VIDEO ? "video" : "audio");
  ast_debug(1, "-- codec.name %s\n", codec->name);

  /* 音频采样信息 */
  if (codec->type == AVMEDIA_TYPE_AUDIO)
  {
    ast_debug(1, "-- ccxt.sample_fmt = %s\n", av_get_sample_fmt_name(cctx->sample_fmt));
    ast_debug(1, "-- ccxt.sample_rate = %d\n", cctx->sample_rate);
    ast_debug(1, "-- ccxt.bytes_per_sample = %d\n", av_get_bytes_per_sample(cctx->sample_fmt));
    ast_debug(1, "-- ccxt.channels = %d\n", cctx->channels);
    char buf[64];
    av_get_channel_layout_string(buf, sizeof(buf), cctx->channels, cctx->channel_layout);
    ast_debug(1, "-- ccxt.channel_layout = %s\n", buf);
  }

  ast_debug(1, "-- stream.avg_frame_rate(%d, %d)\n", st->avg_frame_rate.num, st->avg_frame_rate.den);
  ast_debug(1, "-- stream.tbn(%d, %d)\n", st->time_base.num, st->time_base.den);
  ast_debug(1, "-- stream.duration = %s\n", av_ts2str(st->duration));
}
/* 初始化音频编码器（转换为pcma格式） */
static int tms_init_pcma_encoder(PCMAEnc *encoder)
{
  AVCodec *c;
  c = avcodec_find_encoder(AV_CODEC_ID_PCM_ALAW);
  if (!c)
  {
    ast_log(LOG_ERROR, "没有找到alaw编码器\n");
    return -1;
  }
  int i = 0;
  while (c->sample_fmts[i] != -1)
  {
    ast_debug(1, "encoder.sample_fmts[0] %s\n", av_get_sample_fmt_name(c->sample_fmts[i]));
    i++;
  }

  AVCodecContext *cctx;
  cctx = avcodec_alloc_context3(c);
  if (!cctx)
  {
    ast_log(LOG_ERROR, "分配alaw编码器上下文失败\n");
    return -1;
  }
  /* put sample parameters */
  cctx->bit_rate = ALAW_BIT_RATE;
  cctx->sample_fmt = c->sample_fmts[0];
  cctx->sample_rate = ALAW_SAMPLE_RATE;
  cctx->channel_layout = AV_CH_LAYOUT_MONO;
  cctx->channels = av_get_channel_layout_nb_channels(cctx->channel_layout);

  /* open it */
  if (avcodec_open2(cctx, c, NULL) < 0)
  {
    ast_log(LOG_ERROR, "打开编码器失败\n");
    return -1;
  }

  encoder->codec = c;
  encoder->cctx = cctx;

  return 0;
}
/**
 * Initialize the audio resampler based on the input and encoder codec settings.
 * If the input and encoder sample formats differ, a conversion is required
 * libswresample takes care of this, but requires initialization.
 * @param      input_codec_context  Codec context of the input file
 * @param      output_codec_context Codec context of the encoder file
 * @param[out] resample_context     Resample context for the required conversion
 * @return Error code (0 if successful)
 */
static int tms_init_audio_resampler(AVCodecContext *input_codec_context,
                                    AVCodecContext *output_codec_context,
                                    Resampler *resampler)
{
  int error;

  SwrContext **resample_context = &resampler->swrctx;

  /*
  * Create a resampler context for the conversion.
  * Set the conversion parameters.
  * Default channel layouts based on the number of channels
  * are assumed for simplicity (they are sometimes not detected
  * properly by the demuxer and/or decoder).
  */
  *resample_context = swr_alloc_set_opts(NULL,
                                         av_get_default_channel_layout(output_codec_context->channels),
                                         output_codec_context->sample_fmt,
                                         output_codec_context->sample_rate,
                                         av_get_default_channel_layout(input_codec_context->channels),
                                         input_codec_context->sample_fmt,
                                         input_codec_context->sample_rate,
                                         0, NULL);
  if (!*resample_context)
  {
    ast_log(LOG_ERROR, "Could not allocate resample context\n");
    return AVERROR(ENOMEM);
  }
  /* Open the resampler with the specified parameters. */
  if ((error = swr_init(*resample_context)) < 0)
  {
    ast_log(LOG_ERROR, "Could not open resample context\n");
    swr_free(resample_context);
    return error;
  }

  resampler->data = av_calloc(1, sizeof(**resampler->data));

  return 0;
}
/**
 * 执行音频重采样
 */
static int tms_audio_resample(Resampler *resampler, AVFrame *frame, PCMAEnc *encoder)
{
  int ret = 0;

  int nb_resample_samples = av_rescale_rnd(swr_get_delay(resampler->swrctx, frame->sample_rate) + frame->nb_samples, encoder->cctx->sample_rate, frame->sample_rate, AV_ROUND_UP);

  /* 分配缓冲区 */
  if (nb_resample_samples > resampler->max_nb_samples)
  {
    if (resampler->max_nb_samples > 0)
      av_freep(&resampler->data[0]);

    ret = av_samples_alloc(resampler->data, &resampler->linesize, 1, nb_resample_samples, encoder->cctx->sample_fmt, 0);
    if (ret < 0)
    {
      ast_log(LOG_ERROR, "Could not allocate destination samples\n");
      goto end;
    }
    resampler->max_nb_samples = nb_resample_samples;
  }

  ret = swr_convert(resampler->swrctx, resampler->data, nb_resample_samples, (const uint8_t **)frame->data, frame->nb_samples);
  if (ret < 0)
  {
    ast_log(LOG_ERROR, "Could not allocate destination samples\n");
    goto end;
  }

  encoder->nb_samples = nb_resample_samples;

end:
  return ret;
}
/* 生成自己使用的输入媒体流对象。只支持音频流和视频流。 */
static int tms_init_input_stream(AVFormatContext *fctx, int index, TmsInputStream *ist)
{
  int ret;
  AVStream *st;
  AVCodec *codec;
  AVCodecContext *cctx;

  if (!ist)
  {
    ast_log(LOG_ERROR, "没有传递有效参数");
    return -1;
  }

  st = fctx->streams[index];
  codec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!codec)
  {
    ast_log(LOG_WARNING, "stream #%d codec_id = %d 没有找到解码器\n", index, st->codecpar->codec_id);
    return -1;
  }

  if (codec->type != AVMEDIA_TYPE_VIDEO && codec->type != AVMEDIA_TYPE_AUDIO)
  {
    ast_log(LOG_WARNING, "stream #%d codec.type = %d 不支持处理，只支持视频流或音频流\n", index, codec->type);
    return -1;
  }
  if (codec->type == AVMEDIA_TYPE_VIDEO && 0 != strcmp(codec->name, "h264"))
  {
    ast_log(LOG_WARNING, "stream #%d 视频流不是h264格式\n", index);
    return -1;
  }
  // if (codec->type == AVMEDIA_TYPE_AUDIO && 0 != strcmp(codec->name, "aac"))
  // {
  //   ast_log(LOG_WARNING, "stream #%d 音频流不是aac格式\n", index);
  //   return -1;
  // }

  cctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(cctx, st->codecpar);
  if ((ret = avcodec_open2(cctx, codec, NULL)) < 0)
  {
    ast_log(LOG_WARNING, "stream #%d 读取媒体流基本信息势失败 %s\n", index, av_err2str(ret));
    return -1;
  }

  memset(ist, 0, sizeof(TmsInputStream));
  ist->stream_index = index;
  ist->st = st;
  ist->dec_ctx = cctx;
  ist->codec = codec;
  ist->bytes_per_sample = av_get_bytes_per_sample(cctx->sample_fmt);
  ist->start = AV_NOPTS_VALUE;
  ist->next_dts = AV_NOPTS_VALUE;
  ist->dts = AV_NOPTS_VALUE;

  return 0;
}

/* 输出音频帧调试信息 */
static void tms_dump_audio_frame(AVFrame *frame, TmsPlayerContext *player)
{
  // 	uint8_t *frame_dat = frame->extended_data[0];
  // 	ast_debug(2, "frame前8个字节 %02x %02x %02x %02x %02x %02x %02x %02x \n", frame_dat[0], frame_dat[1], frame_dat[2], frame_dat[3], frame_dat[4], frame_dat[5], frame_dat[6], frame_dat[7]);

  const char *frame_fmt = av_get_sample_fmt_name(frame->format);

  ast_debug(2, "从音频包 #%d 中读取音频帧 #%d, format = %s , sample_rate = %d , channels = %d , nb_samples = %d, pts = %ld, best_effort_timestamp = %ld\n", player->nb_audio_packets, player->nb_audio_frames, frame_fmt, frame->sample_rate, frame->channels, frame->nb_samples, frame->pts, frame->best_effort_timestamp);
}

/* 添加音频帧发送延时 */
static void tms_add_audio_frame_send_delay(AVFrame *frame, TmsPlayerContext *player)
{
  int64_t pts = av_rescale(frame->pts, AV_TIME_BASE, frame->sample_rate);
  int64_t now = av_gettime_relative() - player->start_time;
  ast_debug(1, "计算音频帧 #%d 发送延时 now = %ld pts = %ld delay = %ld\n", player->nb_audio_frames, now, pts, pts - now);
  if (pts > now)
  {
    usleep(pts - now);
  }
}

/**
 * Initialize one data packet for reading or writing.
 * @param packet Packet to be initialized
 */
static void tms_init_pcma_packet(AVPacket *packet)
{
  av_init_packet(packet);
  /* Set the packet data and size so that it is recognized as being empty. */
  packet->data = NULL;
  packet->size = 0;
}
/**
 * @return Error code (0 if successful)
 */
static int tms_init_pcma_frame(PCMAEnc *encoder, Resampler *resampler)
{
  int error;

  AVFrame *frame;
  AVCodecContext *cctx = encoder->cctx;
  int nb_samples = encoder->nb_samples;

  /* Create a new frame to store the audio samples. */
  if (!(frame = av_frame_alloc()))
  {
    ast_log(LOG_ERROR, "Could not allocate encoder frame\n");
    return AVERROR_EXIT;
  }
  /* Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity. */
  frame->nb_samples = nb_samples;
  frame->channel_layout = cctx->channel_layout;
  frame->format = cctx->sample_fmt;
  frame->sample_rate = cctx->sample_rate;
  /* Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified. */
  if ((error = av_frame_get_buffer(frame, 0)) < 0)
  {
    ast_log(LOG_ERROR, "Could not allocate encoder frame samples (error '%s')\n",
            av_err2str(error));
    av_frame_free(&frame);
    return error;
  }

  memcpy(frame->data[0], *(resampler->data), nb_samples * 2);

  encoder->frame = frame;

  return 0;
}
/**
 * 发送RTP包
 * 
 * 应该处理采样数超过限制进行分包的情况 
 */
static int tms_rtp_send_audio(PCMAEnc *encoder, TmsPlayerContext *player)
{
  struct ast_channel *chan = player->chan;
  char *src = player->src;

  uint8_t *output_data = encoder->packet.data;
  int nb_samples = encoder->nb_samples;

  unsigned char buffer[PKT_SIZE];
  struct ast_frame *f = (struct ast_frame *)buffer;

  /* Unset */
  memset(f, 0, PKT_SIZE);

  AST_FRAME_SET_BUFFER(f, f, PKT_OFFSET, PKT_PAYLOAD);
  f->src = strdup(src);
  /* 设置帧类型和编码格式 */
  f->frametype = AST_FRAME_VOICE;
  f->subclass.format = ast_format_alaw;
  /* 时间戳 */
  f->delivery.tv_usec = 0;
  f->delivery.tv_sec = 0;
  /* Don't free the frame outside */
  f->mallocd = 0;
  f->offset = AST_FRIENDLY_OFFSET;
  /* 设置包含的采样数 */
  f->samples = nb_samples;
  /* 设置采样 */
  uint8_t *data;
  data = AST_FRAME_GET_BUFFER(f);
  memcpy(data, output_data, nb_samples);
  f->datalen = nb_samples;

  /* Write frame */
  ast_write(chan, f);
  ast_frfree(f);

  player->nb_audio_rtps++;

  ast_debug(2, "完成 #%d 个音频RTP包发送 \n", player->nb_audio_rtps);

  return 0;
}

/* 输出视频packet信息 */
static void tms_dump_video_packet(AVPacket *pkt, TmsPlayerContext *player)
{
  int64_t dts = pkt->dts == INT64_MIN ? -1 : pkt->dts;
  int64_t pts = pkt->pts == INT64_MIN ? -1 : pkt->pts;
  uint8_t *pkt_data = pkt->data;

  ast_debug(1, "读取媒体包 #%d 所属媒体流 #%d size= %d dts = %" PRId64 " pts = %" PRId64 "\n", player->nb_video_packets, pkt->stream_index, pkt->size, dts, pts);
  if (player->nb_video_packets < 8)
  {
    ast_debug(2, "av_read_frame.packet 前12个字节 %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", pkt_data[0], pkt_data[1], pkt_data[2], pkt_data[3], pkt_data[4], pkt_data[5], pkt_data[6], pkt_data[7], pkt_data[8], pkt_data[9], pkt_data[10], pkt_data[11]);
  }

  int nal_unit_type = pkt_data[4] & 0x1f; // 5 bit
  ast_debug(1, "媒体包 #%d 视频包 #%d nal_unit_type = %d\n", player->nb_packets, player->nb_video_packets, nal_unit_type);
}
/**
 * 播放mp4文件应用主程序
 */
static int mp4_play(struct ast_channel *chan, const char *data)
{
  struct ast_module_user *u = NULL;

  char *filename; // 要打开的文件
  int ret = 0;    // 返回结果
  char *parse;

  AST_DECLARE_APP_ARGS(args, AST_APP_ARG(filename); AST_APP_ARG(options););

  ast_debug(1, "TMSMp4Play %s\n", (char *)data);

  /* Lock module */
  u = ast_module_user_add(chan);

  /* Duplicate input */
  parse = ast_strdup(data);

  /* Get input data */
  AST_STANDARD_APP_ARGS(args, parse);

  // 记录媒体流信息
  TmsInputStream *ists[2];

  uint8_t video_buf[1470];
  TmsVideoRtpContext video_rtp_ctx = {
      .buf = video_buf,
      .max_payload_size = 1400,
      .buffered_nals = 0,
      .flags = 0};

  AVFormatContext *ictx = NULL;

  filename = (char *)args.filename;

  /* 打开指定的媒体文件 */
  if ((ret = avformat_open_input(&ictx, filename, NULL, NULL)) < 0)
  {
    ast_log(LOG_WARNING, "无法打开媒体文件 %s\n", filename);
    goto clean;
  }

  /* 获得指定的视频文件的信息 */
  if ((ret = avformat_find_stream_info(ictx, NULL)) < 0)
  {
    ast_log(LOG_WARNING, "无法获取媒体文件信息 %s\n", filename);
    goto clean;
  }

  int nb_streams = ictx->nb_streams;

  ast_debug(1, "媒体文件 %s nb_streams = %d , duration = %s\n", filename, nb_streams, av_ts2str(ictx->duration));

  /* Set random src */
  char src[128]; // rtp.src
  snprintf(src, 128, "TMSMp4Play%08lx", ast_random());

  /**
   * mp4文件中h264用的是avcc格式进行分割，报中并不包含sps和pps，所以，如果直接读取mp4文件的packet，读取不到sps和pps，也就无法通过rtp发送这两个nalu。
   * 通过bitstream filter将avcc格式转换位annexb格式。
   */
  AVBSFContext *h264bsfc;
  /* 音频重采样 */
  Resampler resampler = {.max_nb_samples = 0};
  PCMAEnc pcma_enc = {.nb_samples = 0};

  int i = 0;
  for (; i < nb_streams; i++)
  {
    TmsInputStream *ist = malloc(sizeof(TmsInputStream));
    tms_init_input_stream(ictx, i, ist);
    tms_dump_stream_format(ist);

    ists[i] = ist;

    if (ist->codec->type == AVMEDIA_TYPE_VIDEO)
    {
      const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
      ret = av_bsf_alloc(filter, &h264bsfc);
      avcodec_parameters_copy(h264bsfc->par_in, ist->st->codecpar);
      av_bsf_init(h264bsfc);
    }
    else if (ist->codec->type == AVMEDIA_TYPE_AUDIO)
    {
      if ((ret = tms_init_pcma_encoder(&pcma_enc)) < 0)
      {
        goto clean;
      }
      /* 设置重采样，将解码出的fltp采样格式，转换为s16采样格式 */
      if ((ret = tms_init_audio_resampler(ist->dec_ctx, pcma_enc.cctx, &resampler)) < 0)
      {
        goto clean;
      }
    }
  }

  // 应用有可能在一个dialplan中多次调用，所以应该用当前时间保证多个应用间的时间戳是连续增长的
  struct timeval now = ast_tvnow();
  video_rtp_ctx.base_timestamp = now.tv_sec * AV_TIME_BASE + now.tv_usec;
  video_rtp_ctx.timestamp = video_rtp_ctx.base_timestamp;
  video_rtp_ctx.cur_timestamp = 0;

  TmsPlayerContext player = {
      .chan = chan,
      .src = src,
      .start_time = av_gettime_relative(),
      .nb_packets = 0,
      .nb_video_packets = 0,
      .nb_video_rtps = 0,
      .nb_audio_packets = 0,
      .nb_audio_frames = 0,
      .nb_audio_rtp_samples = 0,
      .nb_audio_rtps = 0};

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();

  while (1)
  {
    player.nb_packets++;
    if ((ret = av_read_frame(ictx, pkt)) == AVERROR_EOF)
    {
      player.end_time = av_gettime_relative();
      break;
    }
    else if (ret < 0)
    {
      ast_log(LOG_WARNING, "读取媒体包 #%d 失败 %s\n", player.nb_packets, av_err2str(ret));
      goto clean;
    }

    TmsInputStream *ist = ists[pkt->stream_index];
    if (ist->codec->type == AVMEDIA_TYPE_VIDEO)
    {
      player.nb_video_packets++;
      /*将avcc格式转为annexb格式*/
      if ((ret = av_bsf_send_packet(h264bsfc, pkt)) < 0)
      {
        ast_log(LOG_ERROR, "av_bsf_send_packet error");
        goto clean;
      }
      while ((ret = av_bsf_receive_packet(h264bsfc, pkt)) == 0)
        ;

      tms_dump_video_packet(pkt, &player);

      /* 处理视频包 */
      if (!ist->saw_first_ts)
      {
        ist->dts = ist->st->avg_frame_rate.num ? -ist->dec_ctx->has_b_frames * AV_TIME_BASE / av_q2d(ist->st->avg_frame_rate) : 0;
        ist->next_dts = ist->dts;
        ist->saw_first_ts = 1;
      }
      else
      {
        ist->dts = ist->next_dts;
      }

      /* 添加发送间隔 */
      int64_t dts = ist->dts;
      int64_t elapse = av_gettime_relative() - player.start_time;
      if (dts > elapse)
        usleep(dts - elapse);

      ist->next_dts += av_rescale_q(pkt->duration, ist->st->time_base, AV_TIME_BASE_Q);

      // 从ffmpeg的时间转为rtp的时间
      int64_t video_ts = video_rtp_ctx.base_timestamp + dts;
      int64_t rtp_ts = av_rescale(video_ts, RTP_H264_TIME_BASE, AV_TIME_BASE);
      video_rtp_ctx.cur_timestamp = rtp_ts;

      ast_debug(2, "elapse = %ld dts = %ld base_timestamp = %d video_ts = %ld rtp_ts = %ld\n", elapse, dts, video_rtp_ctx.base_timestamp, video_ts, rtp_ts);

      /* 发送RTP包 */
      ff_rtp_send_h264(&video_rtp_ctx, pkt->data, pkt->size, &player);
    }
    else if (ist->codec->type == AVMEDIA_TYPE_AUDIO)
    {
      player.nb_audio_packets++;
      /* 将媒体包发送给解码器 */
      if ((ret = avcodec_send_packet(ist->dec_ctx, pkt)) < 0)
      {
        ast_log(LOG_WARNING, "读取音频包 #%d 失败 %s\n", player.nb_audio_packets, av_err2str(ret));
        goto clean;
      }

      ast_debug(1, "读取音频包 #%d size= %d \n", player.nb_audio_packets, pkt->size);

      while (1)
      {
        /* 从解码器获取音频帧 */
        ret = avcodec_receive_frame(ist->dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
          break;
        }
        else if (ret < 0)
        {
          ast_log(LOG_WARNING, "读取音频帧 #%d 错误 %s\n", player.nb_audio_frames + 1, av_err2str(ret));
          goto clean;
        }

        player.nb_audio_frames++;
        tms_dump_audio_frame(frame, &player);

        /* 添加发送间隔 */
        tms_add_audio_frame_send_delay(frame, &player);

        /* 对获得的音频帧执行重采样 */
        ret = tms_audio_resample(&resampler, frame, &pcma_enc);
        if (ret < 0)
        {
          goto clean;
        }
        /* 重采样后的媒体帧 */
        ret = tms_init_pcma_frame(&pcma_enc, &resampler);
        if (ret < 0)
        {
          goto clean;
        }
        player.nb_pcma_frames++;

        /* 音频帧送编码器准备编码 */
        if ((ret = avcodec_send_frame(pcma_enc.cctx, pcma_enc.frame)) < 0)
        {
          ast_log(LOG_ERROR, "音频帧发送编码器错误\n");
          goto clean;
        }

        /* 要通过rtp输出的包 */
        tms_init_pcma_packet(&pcma_enc.packet);

        while (1)
        {
          ret = avcodec_receive_packet(pcma_enc.cctx, &pcma_enc.packet);
          if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
          {
            break;
          }
          else if (ret < 0)
          {
            ast_log(LOG_ERROR, "Error encoding audio frame\n");
            goto clean;
          }
          //pcma_enc.nb_packets++;
          //pcma_enc.nb_bytes += pcma_enc.packet.size;

          //ast_debug(2, "生成编码包 #%d size= %d \n", pcma_enc.nb_packets, pcma_enc.packet.size);

          /* 通过rtp发送音频 */
          tms_rtp_send_audio(&pcma_enc, &player);
        }
        av_packet_unref(&pcma_enc.packet);
        av_frame_free(&pcma_enc.frame);
      }
    }

    av_packet_unref(pkt);
  }

  ast_debug(1, "共读取 %d 个包，包含：%d 个视频包，%d 个音频包，用时：%ld\n", player.nb_packets, player.nb_video_packets, player.nb_audio_packets, player.start_time - player.end_time);

  /* Log end */
  ast_log(LOG_DEBUG, "结束mp4play\n");

clean:
  if (frame)
    av_frame_free(&frame);

  if (pkt)
    av_packet_free(&pkt);

  if (h264bsfc)
    av_bsf_free(&h264bsfc);

  if (resampler.swrctx)
    swr_free(&resampler.swrctx);

  if (ictx)
    avformat_close_input(&ictx);

  /* Unlock module*/
  ast_module_user_remove(u);

  free(parse);

  /* Exit */
  return 0;
}

static int unload_module(void)
{
  int res = ast_unregister_application(app_play);

  ast_module_user_hangup_all();

  return res;
}

static int load_module(void)
{
  int res = ast_register_application(app_play, mp4_play, syn_play, des_play);

  return res;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "TMS mp4 player applications");
