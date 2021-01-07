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
#include "asterisk/format_cache.h"
#include "asterisk/format_compatibility.h"
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
static const char *des_play = "  TMSMp4Play(filename,[stopdtmfs]):  Play mp4 file to user. \n";

/* 打开指定的文件，获得媒体流信息 */
static int tms_open_file(char *filename, AVFormatContext **ictx, AVBSFContext **h264bsfc, Resampler *resampler, PCMAEnc *pcma_enc, TmsInputStream **ists, int *out_nb_streams)
{
  int ret = 0;

  /* 打开指定的媒体文件 */
  if ((ret = avformat_open_input(ictx, filename, NULL, NULL)) < 0)
  {
    ast_log(LOG_WARNING, "无法打开媒体文件 %s\n", filename);
    return -1;
  }

  /* 获得指定的视频文件的信息 */
  if ((ret = avformat_find_stream_info(*ictx, NULL)) < 0)
  {
    ast_log(LOG_WARNING, "无法获取媒体文件信息 %s\n", filename);
    return -1;
  }

  int nb_streams = (*ictx)->nb_streams;

  ast_debug(1, "媒体文件 %s nb_streams = %d , duration = %s\n", filename, nb_streams, av_ts2str((*ictx)->duration));

  int i = 0;
  for (; i < nb_streams; i++)
  {
    TmsInputStream *ist = malloc(sizeof(TmsInputStream));
    tms_init_input_stream(*ictx, i, ist);
    tms_dump_stream_format(ist);

    ists[i] = ist;

    if (ist->codec->type == AVMEDIA_TYPE_VIDEO)
    {
      const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
      ret = av_bsf_alloc(filter, h264bsfc);
      avcodec_parameters_copy((*h264bsfc)->par_in, ist->st->codecpar);
      av_bsf_init(*h264bsfc);
    }
    else if (ist->codec->type == AVMEDIA_TYPE_AUDIO)
    {
      if ((ret = tms_init_pcma_encoder(pcma_enc)) < 0)
      {
        return -1;
      }
      /* 设置重采样，将解码出的fltp采样格式，转换为s16采样格式 */
      if ((ret = tms_init_audio_resampler(ist->dec_ctx, pcma_enc->cctx, resampler)) < 0)
      {
        return -1;
      }
    }
  }

  *out_nb_streams = nb_streams;

  return 0;
}
/* 释放输入流中的资源 */
static void tms_free_input_streams(TmsInputStream **ists, int nb_streams)
{
  int i = 0;
  for (; i < nb_streams; i++)
  {
    free(ists[i]);
  }
}
/* 根据指定的开始时间生成rtp起始时间戳 */
static uint32_t tms_rtp_base_timestamp(struct timeval tvstart)
{
  uint64_t base_timestamp = tvstart.tv_sec * AV_TIME_BASE + tvstart.tv_usec; // 单位是微秒
  uint32_t rtp_base_timestamp = base_timestamp;

  ast_debug(1, "tvstart.tv_sec = %ld tvstart.tv_usec = %ld base_timestamp = %ld rtp_base_timestamp = %u\n", (uint64_t)tvstart.tv_sec, (uint64_t)tvstart.tv_usec, base_timestamp, rtp_base_timestamp);

  return rtp_base_timestamp;
}
/* 初始化视频rtp发送上下文 */
static int tms_init_video_rtp_context(TmsVideoRtpContext *video_rtp_ctx, uint8_t *video_buf, uint32_t base_timestamp)
{
  video_rtp_ctx->buf = video_buf;
  video_rtp_ctx->max_payload_size = 1400;
  video_rtp_ctx->buffered_nals = 0;
  video_rtp_ctx->flags = 0;

  video_rtp_ctx->cur_timestamp = 0;
  video_rtp_ctx->base_timestamp = base_timestamp;

  return 0;
}
/* 初始化音频rtp发送上下文 */
static int tms_init_audio_rtp_context(TmsAudioRtpContext *audio_rtp_ctx, uint32_t base_timestamp)
{
  audio_rtp_ctx->cur_timestamp = base_timestamp;

  return 0;
}

/* 发送rtcp包，控制音视频流的同步 */
static int tms_audio_rtcp_first_sr(TmsPlayerContext *player, TmsAudioRtpContext *audio_rtp_ctx)
{
  int ret = 0;

  struct timeval now = ast_tvnow();
  uint8_t rtcp[28]; // 28个字节
  uint32_t audio_first_rtcp_ts = audio_rtp_ctx->cur_timestamp * 8 - 8000;
  tms_rtcp_first_sr(rtcp, player->rtp_audio_ssrc, now, audio_first_rtcp_ts);
  int sockfd;
  if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
  {
    ast_log(LOG_ERROR, "建立socket失败\n");
  }
  else
  {
    struct sockaddr_in rtcp_addr = player->rtp_audio_dest_addr;
    int port = ntohs(rtcp_addr.sin_port) + 1;
    rtcp_addr.sin_port = htons(port);
    ret = sendto(sockfd, rtcp, 28, 0, (struct sockaddr *)&rtcp_addr, sizeof(struct sockaddr_in));
    if (ret < 0)
    {
      ast_log(LOG_ERROR, "Audio RTCP 首个发送失败 fd = %d %d\n", sockfd, ret);
    }
  }

  player->first_rtcp_auido = 1;

  return 0;
}
/* 发送rtcp包，控制音视频流的同步 */
static int tms_video_rtcp_first_sr(TmsPlayerContext *player, TmsVideoRtpContext *video_rtp_ctx)
{
  int ret = 0;

  struct timeval now = ast_tvnow();
  uint8_t rtcp[28]; // 28个字节
  uint32_t video_first_rtcp_ts = video_rtp_ctx->cur_timestamp;
  tms_rtcp_first_sr(rtcp, player->rtp_video_ssrc, now, video_first_rtcp_ts);
  int sockfd;
  if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
  {
    ast_log(LOG_ERROR, "建立socket失败\n");
  }
  else
  {
    struct sockaddr_in rtcp_addr = player->rtp_video_dest_addr;
    int port = ntohs(rtcp_addr.sin_port) + 1;
    rtcp_addr.sin_port = htons(port);
    ret = sendto(sockfd, rtcp, 28, 0, (struct sockaddr *)&rtcp_addr, sizeof(struct sockaddr_in));
    if (ret < 0)
    {
      ast_log(LOG_ERROR, "Video RTCP 首个发送失败 fd = %d %d\n", sockfd, ret);
    }
  }
  player->first_rtcp_video = 1;

  return 0;
}
/* 处理视频媒体包 */
static int tms_handle_video_packet(TmsPlayerContext *player, TmsInputStream *ist, AVPacket *pkt, AVBSFContext *h264bsfc, TmsVideoRtpContext *video_rtp_ctx)
{
  int ret = 0;

  player->nb_video_packets++;
  /*将avcc格式转为annexb格式*/
  if ((ret = av_bsf_send_packet(h264bsfc, pkt)) < 0)
  {
    ast_log(LOG_ERROR, "av_bsf_send_packet error");
    return -1;
  }
  while ((ret = av_bsf_receive_packet(h264bsfc, pkt)) == 0)
    ;

  tms_dump_video_packet(pkt, player);

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
  int64_t elapse = av_gettime_relative() - player->start_time_us - player->pause_duration_us;
  if (dts > elapse)
    usleep(dts - elapse);

  ist->next_dts += av_rescale_q(pkt->duration, ist->st->time_base, AV_TIME_BASE_Q);

  /* 指定帧时间戳，单位是毫秒 */
  int64_t video_ts = video_rtp_ctx->base_timestamp + dts; // 微秒
  /**
   * int h264_sample_rate = (int)ast_format_get_sample_rate(ast_format_h264);
   * asterisk中，h264的sample_rate取到的值是1000，实际上应该是90000，需要在设置ts的时候修正
   */
  video_rtp_ctx->cur_timestamp = video_ts / 1000 * (RTP_H264_TIME_BASE / 1000); // 毫秒
  if (!player->first_rtcp_video)
  {
    tms_video_rtcp_first_sr(player, video_rtp_ctx);
  }

  ast_debug(2, "elapse = %ld dts = %ld base_timestamp = %d video_ts = %ld\n", elapse, dts, video_rtp_ctx->base_timestamp, video_ts);

  /* 发送RTP包 */
  ff_rtp_send_h264(video_rtp_ctx, pkt->data, pkt->size, player);

  return 0;
}
/* 处理音频媒体包 */
// --- 2020-12-24 by wpc modify , add two parameter char *sendbuff,int sendbuff_memory_size end ---
static int tms_handle_audio_packet(TmsPlayerContext *player, TmsInputStream *ist, Resampler *resampler, PCMAEnc *pcma_enc, AVPacket *pkt, AVFrame *frame, TmsAudioRtpContext *audio_rtp_ctx,rtp_split_msg *msg)
{
  int ret = 0;
  player->nb_audio_packets++;
  /* 将媒体包发送给解码器 */
  if ((ret = avcodec_send_packet(ist->dec_ctx, pkt)) < 0)
  {
    ast_log(LOG_WARNING, "读取音频包 #%d 失败 %s\n", player->nb_audio_packets, av_err2str(ret));
    return -1;
  }
  int nb_packet_frames = 0;
  ast_debug(1, "读取音频包 #%d size= %d \n", player->nb_audio_packets, pkt->size);

  while (1)
  {
    /* 从解码器获取音频帧 */
    ret = avcodec_receive_frame(ist->dec_ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    {
      if (nb_packet_frames == 0)
      {
        ast_log(LOG_WARNING, "未从音频包 #%d 中读取音频帧，原因：%s\n", player->nb_audio_packets, av_err2str(ret));
      }
      break;
    }
    else if (ret < 0)
    {
      ast_log(LOG_WARNING, "读取音频帧 #%d 错误 %s\n", player->nb_audio_frames + 1, av_err2str(ret));
      return -1;
    }
    nb_packet_frames++;
    player->nb_audio_frames++;
    tms_dump_audio_frame(frame, player);

    /* 添加发送间隔 */
    //由于大网中sdp协商中pcma时间间隔ptime:20ms, 1000/20=50, 8000/50=160,每次发送160采样数据包
    //mp4文件读出数据不是按照160组包,所以要自己封包发送,并且后面添加间隔时间 ---2020-12-23 by wpc modify ---
    //tms_add_audio_frame_send_delay(frame, player);

    /* 对获得的音频帧执行重采样 */
    ret = tms_audio_resample(resampler, frame, pcma_enc);
    if (ret < 0)
    {
      return -1;
    }
    /* 重采样后的媒体帧 */
    ret = tms_init_pcma_frame(pcma_enc, resampler);
    if (ret < 0)
    {
      return -1;
    }
    player->nb_pcma_frames++;

    /* 音频帧送编码器准备编码 */
    if ((ret = avcodec_send_frame(pcma_enc->cctx, pcma_enc->frame)) < 0)
    {
      ast_log(LOG_ERROR, "音频帧发送编码器错误\n");
      return -1;
    }

    /* 要通过rtp输出的包 */
    tms_init_pcma_packet(&pcma_enc->packet);

    while (1)
    {
      ret = avcodec_receive_packet(pcma_enc->cctx, &pcma_enc->packet);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      {
        break;
      }
      else if (ret < 0)
      {
        ast_log(LOG_ERROR, "Error encoding audio frame\n");
        return -1;
      }
      /* 计算时间戳，单位毫秒（milliseconds） a*b/c */
      //---2020-12-24 by wpc add --- 由于pcma在大网传输中进行每160采样发送一次,所以从mp4中读取的一个音频帧要拆分多个指定大小160包进行发送
      audio_rtp_ctx->cur_timestamp += av_rescale(pcma_enc->nb_samples, AV_TIME_BASE, RTP_PCMA_TIME_BASE) / 1000;
      if (!player->first_rtcp_auido)
      {
        tms_audio_rtcp_first_sr(player, audio_rtp_ctx);
        *(msg->rtp_timestamp) =  audio_rtp_ctx->cur_timestamp;
      }
      /* 通过rtp发送音频 */
      //tms_rtp_send_audio(audio_rtp_ctx, pcma_enc, player);
      split_packet_size(msg,pcma_enc,player);
    }
    av_packet_unref(&pcma_enc->packet);
    av_frame_free(&pcma_enc->frame);
  }
  /*
  if(strlen(sendbuff)>0)
  {
    tms_send_audio_rtp(audio_rtp_ctx, player, sendbuff,strlen(sendbuff));
    ast_debug(2,"@@@av_read_frame while after,#####send_rtp####strlen(tmp):%d !@@@\n",(int)strlen(sendbuff));
  }
  */
  return 0;
}
/**
 * 等恢复播放 
 */
static int tms_wait_resume(struct ast_channel *chan, char *resumedtmfs, int64_t *pause_duration_us, int *stop)
{
  int64_t start = av_gettime_relative();
  int ms = -1;

  while (1)
  {
    ms = ast_waitfor(chan, ms);
    if (ms < 0)
    {
      ast_debug(1, "Hangup detected\n");
      *stop = 1;
      break;
    }
    if (ms)
    {
      struct ast_frame *f = ast_read(chan);
      if (!f)
      {
        ast_debug(1, "Null frame == hangup() detected\n");
        *stop = 1;
        break;
      }
      /* 如果是dtmf，检查是否需要处理按键控制逻辑 */
      if (f->frametype == AST_FRAME_DTMF)
      {
        char key[2] = {(char)f->subclass.integer, '\0'};
        ast_debug(2, "等待恢复，收到DTMF：key=%s\n", key);
        /* 恢复播放 */
        if (!ast_strlen_zero(resumedtmfs) && strchr(resumedtmfs, key[0]))
        {
          break;
        }
      }
      ast_frfree(f);
    }
  }

  *pause_duration_us += (av_gettime_relative() - start);

  return 0;
}
/**
 * 播放指定的mp4文件
 */
static int mp4_play_once(struct ast_channel *chan, char *filename, int *stop, int max_playing_ms, char *stopdtmfs, char *pausedtmfs, char *resumedtmfs, int64_t *out_pause_duration_us)
{
  int ret = 0;
  int pause = 0; // 暂停状态
  int ms = -1;
  char tmp[2048] = {'\0'};
  TmsInputStream *ists[2]; // 记录媒体流信息
  AVFormatContext *ictx = NULL;
  AVBSFContext *h264bsfc; // mp4转h264，将sps和pps放到推送流中
  /* 音频重采样 */
  Resampler resampler = {.max_nb_samples = 0};
  PCMAEnc pcma_enc = {.nb_samples = 0};
  int nb_streams = 0; // 媒体流的数量
  uint32_t cur_timestamp = 0;
  rtp_split_msg msg;
  memset(&msg,0,sizeof(msg));
  msg.buff = tmp;
  msg.buff_memory_size = sizeof(tmp);
  msg.split_packet_size = 160;

  if ((ret = tms_open_file(filename, &ictx, &h264bsfc, &resampler, &pcma_enc, ists, &nb_streams)) < 0)
  {
    *stop = 1;
    goto clean;
  }

  struct timeval tvstart = ast_tvnow(); // tv_sec 有10位，tv_usec 有6位

  uint32_t rtp_base_timestamp = tms_rtp_base_timestamp(tvstart); // 如果dialplan中连续调用应用，需要让每次推送的rtp流的timestamp具有连续性，因此要基于当前时间生成rtp起始时间戳
  uint8_t video_buf[1470];
  TmsVideoRtpContext video_rtp_ctx;
  TmsAudioRtpContext audio_rtp_ctx;

  tms_init_video_rtp_context(&video_rtp_ctx, video_buf, rtp_base_timestamp);
  tms_init_audio_rtp_context(&audio_rtp_ctx, rtp_base_timestamp);

  TmsPlayerContext player;
  if ((ret = tms_init_player_context(chan, &player)) < 0)
  {
    *stop = 1;
    goto clean;
  }

  AVPacket *pkt = av_packet_alloc(); // ffmpeg媒体包
  AVFrame *frame = av_frame_alloc(); // ffmpeg媒体帧
  msg.rtp_timestamp = &cur_timestamp;
  while (1)
  {
    /**
     * 处理获得的媒体包 
     */
    player.nb_packets++;
    if ((ret = av_read_frame(ictx, pkt)) == AVERROR_EOF)
    {
      player.end_time_us = av_gettime_relative();
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
      if ((ret = tms_handle_video_packet(&player, ist, pkt, h264bsfc, &video_rtp_ctx)) < 0)
      {
        *stop = 1;
        goto clean;
      }
    }
    else if (ist->codec->type == AVMEDIA_TYPE_AUDIO)
    {
      //--- 2020-12-24 by wpc modify ---
      if ((ret = tms_handle_audio_packet(&player, ist, &resampler, &pcma_enc, pkt, frame, &audio_rtp_ctx,&msg)) < 0)
      {
        *stop = 1;
        goto clean;
      }
    }

    av_packet_unref(pkt);

    /** 
     * 解决挂机后数据清理问题和dtmf处理
     * 是否会存在没有输入的情况？
     */
    ms = ast_waitfor(chan, ms);
    if (ms < 0)
    {
      ast_debug(1, "Hangup detected\n");
      *stop = 1;
      goto clean;
    }
    if (ms)
    {
      struct ast_frame *f = ast_read(chan);
      if (!f)
      {
        ast_debug(1, "Null frame == hangup() detected\n");
        *stop = 1;
        goto clean;
      }
      /* 如果是dtmf，检查是否需要处理按键控制逻辑 */
      if (f->frametype == AST_FRAME_DTMF)
      {
        char key[2] = {(char)f->subclass.integer, '\0'};
        ast_debug(2, "收到DTMF(%s)，检查是否需要播放控制\n", key);
        /* 停止播放 */
        if (!ast_strlen_zero(stopdtmfs) && strchr(stopdtmfs, key[0]))
        {
          pbx_builtin_setvar_helper(chan, "TMSDTMFKEY", key);
          ast_frfree(f);
          *stop = 1;
          goto end;
        }
        /* 停止播放 */
        else if (!ast_strlen_zero(pausedtmfs) && strchr(pausedtmfs, key[0]))
        {
          pause = 1;
        }
      }
      ast_frfree(f);
    }
    /** 
     * 检查是否已经达到播放时间 
     */
    if (max_playing_ms > 0)
    {
      if (ast_remaining_ms(tvstart, max_playing_ms + (player.pause_duration_us / 1000)) <= 0)
      {
        ast_debug(1, "播放超时，结束本次播放 %s\n", filename);
        *stop = 1;
        goto end;
      }
    }
    /**
     * 是否进入暂停状态
     */
    if (pause)
    {
      tms_wait_resume(chan, resumedtmfs, &player.pause_duration_us, stop);
      if (*stop)
        goto end;

      ast_debug(2, "暂停播放 %ld 微秒\n", player.pause_duration_us);

      pause = 0;
      if (out_pause_duration_us)
        *out_pause_duration_us = player.pause_duration_us;
    }
  }
  if(strlen(tmp)>0)
  {
    tms_send_audio_rtp(&msg, &player, tmp,strlen(tmp));
    ast_debug(2,"@@@av_read_frame while after,#####send_rtp####strlen(tmp):%d !@@@\n",(int)strlen(tmp));
  } 

end:
  /* Log end */
  ast_debug(1, "完成文件播放 %s，共读取 %d 个包，包含：%d 个视频包，%d 个音频包，用时：%ld微秒\n", filename, player.nb_packets, player.nb_video_packets, player.nb_audio_packets, player.end_time_us - player.start_time_us);

clean:
  if (nb_streams > 0)
    tms_free_input_streams(ists, nb_streams);

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

  char *filename;                             // 要打开的文件
  char *stopdtmfs, *pausedtmfs, *resumedtmfs; // 控制播放的按键，停止，暂停，恢复
  int repeat = 0;                             // 重复播放的次数，等于0不重复，共播放1+repeat次
  int max_duration_ms = 0;                    // 播放总时长，单位毫秒
  int remaining_ms = 0;                       // 剩余播放时长，单位毫秒
  int nb_play_times = 0;                      // 已经播放的次数
  int stop = 0;                               // 是否停止播放

  char *parse;

  AST_DECLARE_APP_ARGS(
      args,
      AST_APP_ARG(filename);
      AST_APP_ARG(repeat);
      AST_APP_ARG(duration);
      AST_APP_ARG(stopdtmfs);
      AST_APP_ARG(pausedtmfs);
      AST_APP_ARG(resumedtmfs););

  ast_debug(1, "进入TMSMp4Play(%s)\n", data);

  /* Lock module */
  u = ast_module_user_add(chan);

  /* Duplicate input */
  parse = ast_strdup(data);

  /* Get input data */
  AST_STANDARD_APP_ARGS(args, parse);

  /* 处理传入的参数 */
  filename = args.filename;
  stopdtmfs = args.stopdtmfs;
  pausedtmfs = args.pausedtmfs;
  resumedtmfs = args.resumedtmfs;

  if (!ast_strlen_zero(args.repeat))
  {
    if ((repeat = atoi(args.repeat)) < 0)
      repeat = 0;
  }
  if (!ast_strlen_zero(args.duration))
  {
    max_duration_ms = atof(args.duration);
    max_duration_ms = max_duration_ms <= 0 ? 0 : max_duration_ms * 1000.0;
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
        int64_t pause_duration_us = 0; // 暂停播放时长
        mp4_play_once(chan, filename, &stop, remaining_ms, stopdtmfs, pausedtmfs, resumedtmfs, &pause_duration_us);
        max_duration_ms += (pause_duration_us / 1000);
      }
    }
    else
    {
      mp4_play_once(chan, filename, &stop, 0, stopdtmfs, pausedtmfs, resumedtmfs, NULL);
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
