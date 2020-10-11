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

#include "tms_h264.h"
#include "tms_pcma.h"
#include "tms_rtp.h"
#include "tms_stream.h"

static const char *app_play = "TMSMp4Play";
static const char *syn_play = "MP4 file playblack";
static const char *des_play = "  TMSMp4Play(filename,[stopkeys]):  Play mp4 file to user. \n";

/**
 * 播放指定的mp4文件
 */
static int mp4_play_once(struct ast_channel *chan, char *filename, char *stopkeys, int remaining_ms, int *stop)
{
  int ret = 0;
  int ms = -1;

  // 记录媒体流信息
  TmsInputStream *ists[2];

  uint8_t video_buf[1470];
  TmsVideoRtpContext video_rtp_ctx = {
      .buf = video_buf,
      .max_payload_size = 1400,
      .buffered_nals = 0,
      .flags = 0};

  TmsAudioRtpContext audio_rtp_ctx = {};

  AVFormatContext *ictx = NULL;

  /* 打开指定的媒体文件 */
  if ((ret = avformat_open_input(&ictx, filename, NULL, NULL)) < 0)
  {
    ast_log(LOG_WARNING, "无法打开媒体文件 %s\n", filename);
    *stop = 1;
    goto clean;
  }

  /* 获得指定的视频文件的信息 */
  if ((ret = avformat_find_stream_info(ictx, NULL)) < 0)
  {
    ast_log(LOG_WARNING, "无法获取媒体文件信息 %s\n", filename);
    *stop = 1;
    goto clean;
  }

  int nb_streams = ictx->nb_streams;

  ast_debug(1, "媒体文件 %s nb_streams = %d , duration = %s\n", filename, nb_streams, av_ts2str(ictx->duration));

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
        *stop = 1;
        goto clean;
      }
      /* 设置重采样，将解码出的fltp采样格式，转换为s16采样格式 */
      if ((ret = tms_init_audio_resampler(ist->dec_ctx, pcma_enc.cctx, &resampler)) < 0)
      {
        *stop = 1;
        goto clean;
      }
    }
  }

  /**
   * 应用有可能在一个dialplan中多次调用，实现多个文件的连续播放，因此，应该用当前时间保证多个应用间的时间戳是连续增长的，否则可以指定为任意值
   */
  struct timeval tvstart = ast_tvnow();                                      // tv_sec 有10位，tv_usec 有6位
  uint64_t base_timestamp = tvstart.tv_sec * AV_TIME_BASE + tvstart.tv_usec; // 单位是微秒
  uint32_t rtp_base_timestamp = base_timestamp;
  video_rtp_ctx.cur_timestamp = 0;
  video_rtp_ctx.base_timestamp = audio_rtp_ctx.cur_timestamp = rtp_base_timestamp;

  ast_debug(1, "tvstart.tv_sec = %ld tvstart.tv_usec = %ld base_timestamp = %ld rtp_base_timestamp = %u\n", (uint64_t)tvstart.tv_sec, (uint64_t)tvstart.tv_usec, base_timestamp, rtp_base_timestamp);

  TmsPlayerContext player = {
      .chan = chan,
      .start_time = av_gettime_relative(), // 单位是微秒
      .nb_packets = 0,
      .nb_video_packets = 0,
      .nb_video_rtps = 0,
      .nb_audio_packets = 0,
      .nb_audio_frames = 0,
      .nb_audio_rtp_samples = 0,
      .nb_audio_rtps = 0};

  if ((ret = tms_ast_channel_get_rtp_dest(chan, &player.rtp_audio_dest_addr, &player.rtp_video_dest_addr)) < 0)
  {
    *stop = 1;
    goto clean;
  }
  if ((ret = tms_ast_channel_get_rtp_ssrc(chan, &player.rtp_audio_ssrc, &player.rtp_video_ssrc)) < 0)
  {
    *stop = 1;
    goto clean;
  }
  ast_debug(1, "音频 RTP 地址 %s:%d，视频 RTP 地址 %s:%d，音频 ssrc %d，视频 ssrc %d\n", ast_inet_ntoa(player.rtp_audio_dest_addr.sin_addr), ntohs(player.rtp_audio_dest_addr.sin_port), ast_inet_ntoa(player.rtp_video_dest_addr.sin_addr), ntohs(player.rtp_video_dest_addr.sin_port), player.rtp_audio_ssrc, player.rtp_video_ssrc);

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
      *stop = 1;
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
        *stop = 1;
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

      /* 指定帧时间戳，单位是毫秒 */
      int64_t video_ts = video_rtp_ctx.base_timestamp + dts; // 微秒
      /**
       * int h264_sample_rate = (int)ast_format_get_sample_rate(ast_format_h264);
       * asterisk中，h264的sample_rate取到的值是1000，实际上应该是90000，需要在设置ts的时候修正
       */
      video_rtp_ctx.cur_timestamp = video_ts / 1000 * (RTP_H264_TIME_BASE / 1000); // 毫秒
      if (!player.first_rtcp_video)
      {
        /* 发送rtcp包，控制音视频流的同步 */
        struct timeval now = ast_tvnow();
        uint8_t rtcp[28]; // 28个字节
        uint32_t video_first_rtcp_ts = video_rtp_ctx.cur_timestamp;
        tms_rtcp_first_sr(rtcp, player.rtp_video_ssrc, now, video_first_rtcp_ts);
        int sockfd;
        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        {
          ast_log(LOG_ERROR, "建立socket失败\n");
          *stop = 1;
          goto clean;
        }
        struct sockaddr_in rtcp_addr = player.rtp_video_dest_addr;
        int port = ntohs(rtcp_addr.sin_port) + 1;
        rtcp_addr.sin_port = htons(port);
        ret = sendto(sockfd, rtcp, 28, 0, (struct sockaddr *)&rtcp_addr, sizeof(struct sockaddr_in));
        if (ret < 0)
        {
          ast_log(LOG_ERROR, "Video RTCP 首个发送失败 fd = %d %d\n", sockfd, ret);
        }
        player.first_rtcp_video = 1;
      }
      ast_debug(2, "elapse = %ld dts = %ld base_timestamp = %d video_ts = %ld\n", elapse, dts, video_rtp_ctx.base_timestamp, video_ts);

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
        *stop = 1;
        goto clean;
      }
      int nb_packet_frames = 0;
      ast_debug(1, "读取音频包 #%d size= %d \n", player.nb_audio_packets, pkt->size);

      while (1)
      {
        /* 从解码器获取音频帧 */
        ret = avcodec_receive_frame(ist->dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
          if (nb_packet_frames == 0)
          {
            ast_log(LOG_WARNING, "未从音频包 #%d 中读取音频帧，原因：%s\n", player.nb_audio_packets, av_err2str(ret));
          }
          break;
        }
        else if (ret < 0)
        {
          ast_log(LOG_WARNING, "读取音频帧 #%d 错误 %s\n", player.nb_audio_frames + 1, av_err2str(ret));
          *stop = 1;
          goto clean;
        }
        nb_packet_frames++;
        player.nb_audio_frames++;
        tms_dump_audio_frame(frame, &player);

        /* 添加发送间隔 */
        tms_add_audio_frame_send_delay(frame, &player);

        /* 对获得的音频帧执行重采样 */
        ret = tms_audio_resample(&resampler, frame, &pcma_enc);
        if (ret < 0)
        {
          *stop = 1;
          goto clean;
        }
        /* 重采样后的媒体帧 */
        ret = tms_init_pcma_frame(&pcma_enc, &resampler);
        if (ret < 0)
        {
          *stop = 1;
          goto clean;
        }
        player.nb_pcma_frames++;

        /* 音频帧送编码器准备编码 */
        if ((ret = avcodec_send_frame(pcma_enc.cctx, pcma_enc.frame)) < 0)
        {
          ast_log(LOG_ERROR, "音频帧发送编码器错误\n");
          *stop = 1;
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
            *stop = 1;
            goto clean;
          }
          /* 计算时间戳，单位毫秒（milliseconds） a*b/c */
          audio_rtp_ctx.cur_timestamp += av_rescale(pcma_enc.nb_samples, AV_TIME_BASE, RTP_PCMA_TIME_BASE) / 1000;
          if (!player.first_rtcp_auido)
          {
            /* 发送rtcp包，控制音视频流的同步 */
            struct timeval now = ast_tvnow();
            uint8_t rtcp[28]; // 28个字节
            uint32_t audio_first_rtcp_ts = audio_rtp_ctx.cur_timestamp * 8 - 8000;
            tms_rtcp_first_sr(rtcp, player.rtp_audio_ssrc, now, audio_first_rtcp_ts);
            int sockfd;
            if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
            {
              ast_log(LOG_ERROR, "建立socket失败\n");
              goto clean;
            }
            struct sockaddr_in rtcp_addr = player.rtp_audio_dest_addr;
            int port = ntohs(rtcp_addr.sin_port) + 1;
            rtcp_addr.sin_port = htons(port);
            ret = sendto(sockfd, rtcp, 28, 0, (struct sockaddr *)&rtcp_addr, sizeof(struct sockaddr_in));
            if (ret < 0)
            {
              ast_log(LOG_ERROR, "Audio RTCP 首个发送失败 fd = %d %d\n", sockfd, ret);
            }
            player.first_rtcp_auido = 1;
          }
          /* 通过rtp发送音频 */
          tms_rtp_send_audio(&audio_rtp_ctx, &pcma_enc, &player);
        }
        av_packet_unref(&pcma_enc.packet);
        av_frame_free(&pcma_enc.frame);
      }
    }

    av_packet_unref(pkt);

    /* 解决挂机后数据清理问题 */
    ms = ast_waitfor(chan, ms);
    if (ms < 0)
    {
      ast_debug(1, "Hangup detected\n");
      *stop = 1;
      goto clean;
    }
    if (ms)
    {
      /* 检查是否需要结束 */
      if (!ast_strlen_zero(stopkeys))
      {
        struct ast_frame *f = ast_read(chan);
        if (!f)
        {
          ast_debug(1, "Null frame == hangup() detected\n");
          *stop = 1;
          goto clean;
        }
        if (f->frametype == AST_FRAME_DTMF)
        {
          char key[2] = {(char)f->subclass.integer, '\0'};
          ast_debug(1, "收到DTMF：key=%s\n", key);
          if (strchr(stopkeys, key[0]))
          {
            pbx_builtin_setvar_helper(chan, "TMSDTMFKEY", key);
            ast_frfree(f);
            *stop = 1;
            goto clean;
          }
          ast_frfree(f);
        }
        ast_frfree(f);
      }
    }
    /* 检查是否已经超时 */
    if (remaining_ms > 0)
    {
      if (ast_remaining_ms(tvstart, remaining_ms) <= 0)
      {
        ast_debug(1, "播放超时，结束本次播放 %s\n", filename);
        *stop = 1;
        goto clean;
      }
    }
  }

  ast_debug(1, "共读取 %d 个包，包含：%d 个视频包，%d 个音频包，用时：%ld微秒\n", player.nb_packets, player.nb_video_packets, player.nb_audio_packets, player.end_time - player.start_time);

  /* Log end */
  ast_debug(1, "完成1次播放 %s\n", filename);

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

  return ret;
}
/**
 * 播放mp4文件应用主程序
 */
static int mp4_exec(struct ast_channel *chan, const char *data)
{
  struct ast_module_user *u = NULL;

  char *filename;          // 要打开的文件
  char *stopkeys;          // 停止播放的按键
  int repeat = 0;          // 重复播放的次数，等于0不重复，共播放1+repeat次
  int max_duration_ms = 0; // 播放总时长，单位毫秒
  int remaining_ms = 0;    // 等待播放时长，单位毫秒
  int nb_play_times = 0;   // 已经播放的次数
  int stop = 0;            // 是否停止播放

  char *parse;

  AST_DECLARE_APP_ARGS(
      args,
      AST_APP_ARG(filename);
      AST_APP_ARG(repeat);
      AST_APP_ARG(duration);
      AST_APP_ARG(stopkeys););

  ast_debug(1, "进入TMSMp4Play(%s)\n", data);

  /* Lock module */
  u = ast_module_user_add(chan);

  /* Duplicate input */
  parse = ast_strdup(data);

  /* Get input data */
  AST_STANDARD_APP_ARGS(args, parse);

  filename = args.filename;
  stopkeys = args.stopkeys;

  if (!ast_strlen_zero(args.repeat))
  {
    repeat = atoi(args.repeat);
    if (repeat < 0)
      repeat = 0;
  }
  if (!ast_strlen_zero(args.duration))
  {
    max_duration_ms = atof(args.duration);
    if (max_duration_ms <= 0)
      max_duration_ms = 0;
    else
      max_duration_ms = max_duration_ms * 1000.0;
  }

  struct timeval tvstart = ast_tvnow(); // 开始播放时间

  while (nb_play_times < repeat + 1)
  {
    if (max_duration_ms > 0)
    {
      /* 检查播放时间限制 */
      remaining_ms = ast_remaining_ms(tvstart, max_duration_ms);
      if (remaining_ms <= 0)
      {
        ast_log(LOG_DEBUG, "播放超时，结束播放 %s\n", data);
        break;
      }
      else
      {
        mp4_play_once(chan, filename, stopkeys, remaining_ms, &stop);
      }
    }
    else
    {
      mp4_play_once(chan, filename, stopkeys, 0, &stop);
    }

    if (stop)
    {
      ast_log(LOG_DEBUG, "停止播放 %s\n", data);
      break;
    }

    nb_play_times++;
  }

  /* Unlock module*/
  ast_module_user_remove(u);

  /* 释放资源 */
  free(parse);

  ast_log(LOG_DEBUG, "退出TMSMp4Play(%s)\n", data);

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
  int res = ast_register_application(app_play, mp4_exec, syn_play, des_play);

  return res;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "TMS mp4 player applications");
