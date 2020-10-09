#define ALAW_BIT_RATE 64000   // alaw比特率
#define ALAW_SAMPLE_RATE 8000 // alaw采样率

#ifndef TMS_PCMA_H
#define TMS_PCMA_H

#include "tms_rtp.h"
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
 * 重采样 
 */
typedef struct Resampler
{
  SwrContext *swrctx;
  int max_nb_samples; // 重采样缓冲区最大采样数
  int linesize;       // 声道平面尺寸
  uint8_t **data;     // 重采样缓冲区
} Resampler;

int tms_init_pcma_encoder(PCMAEnc *encoder);

int tms_init_audio_resampler(AVCodecContext *input_codec_context,
                             AVCodecContext *output_codec_context,
                             Resampler *resampler);

int tms_audio_resample(Resampler *resampler, AVFrame *frame, PCMAEnc *encoder);

void tms_init_pcma_packet(AVPacket *packet);

int tms_init_pcma_frame(PCMAEnc *encoder, Resampler *resampler);

void tms_add_audio_frame_send_delay(AVFrame *frame, TmsPlayerContext *player);

int tms_rtp_send_audio(TmsAudioRtpContext *rtp, PCMAEnc *encoder, TmsPlayerContext *player);

void tms_dump_video_packet(AVPacket *pkt, TmsPlayerContext *player);

/* 初始化音频编码器（转换为pcma格式） */
int tms_init_pcma_encoder(PCMAEnc *encoder)
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
int tms_init_audio_resampler(AVCodecContext *input_codec_context,
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
int tms_audio_resample(Resampler *resampler, AVFrame *frame, PCMAEnc *encoder)
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

/**
 * Initialize one data packet for reading or writing.
 * @param packet Packet to be initialized
 */
void tms_init_pcma_packet(AVPacket *packet)
{
  av_init_packet(packet);
  /* Set the packet data and size so that it is recognized as being empty. */
  packet->data = NULL;
  packet->size = 0;
}
/**
 * @return Error code (0 if successful)
 */
int tms_init_pcma_frame(PCMAEnc *encoder, Resampler *resampler)
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

/* 添加音频帧发送延时 */
void tms_add_audio_frame_send_delay(AVFrame *frame, TmsPlayerContext *player)
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
 * 发送RTP包
 * 
 * 应该处理采样数超过限制进行分包的情况 
 */
int tms_rtp_send_audio(TmsAudioRtpContext *rtp, PCMAEnc *encoder, TmsPlayerContext *player)
{
  struct ast_channel *chan = player->chan;

  uint8_t *output_data = encoder->packet.data;
  int nb_samples = encoder->nb_samples;

  unsigned char buffer[PKT_SIZE];
  struct ast_frame *f = (struct ast_frame *)buffer;
  rtp->timestamp = rtp->cur_timestamp;

  /* Unset */
  memset(f, 0, PKT_SIZE);

  AST_FRAME_SET_BUFFER(f, f, PKT_OFFSET, PKT_PAYLOAD);

  /* 设置帧类型和编码格式 */
  f->frametype = AST_FRAME_VOICE;
  f->subclass.format = ast_format_alaw;
  /* 时间戳 */
  // f->delivery.tv_usec = 0;
  // f->delivery.tv_sec = 0;
  f->delivery = ast_tvnow();
  // 告知asterisk使用指定的时间戳
  ast_set_flag(f, AST_FRFLAG_HAS_TIMING_INFO);
  f->ts = rtp->timestamp;
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
void tms_dump_video_packet(AVPacket *pkt, TmsPlayerContext *player)
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

#endif