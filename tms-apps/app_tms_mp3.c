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
 * \brief TMS mp3 application -- play mp3 files
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

static const char *app_play = "TMSMp3Play";                                                 // 应用的名字，在extensions.conf中使用
static const char *syn_play = "mp3 file playblack";                                         // Synopsis，应用简介
static const char *des_play = "TMSMp3Play(filename,[options]):  Play mp3 file to user. \n"; // 应用描述

#define AST_FRAME_GET_BUFFER(fr) ((uint8_t *)((fr)->data.ptr))

#define PKT_PAYLOAD 1460
#define PKT_SIZE (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET + PKT_PAYLOAD)
#define PKT_OFFSET (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET)

#define ALAW_SAMPLE_RATE 8000 // alaw采样率
#define BYTES_PER_SAMPLE 1    // 每个采样的字节数
#define MAX_PKT_SAMPLES 320   // 每个rtp帧中包含的采样数

/**
 * 解码器 
 */
typedef struct Decoder
{
  char *filename;
  AVFormatContext *ictx;
  AVCodec *codec;
  AVCodecContext *cctx;
  int nb_packets;
  int nb_frames;
  int nb_bytes;
  int nb_samples;
  AVPacket *packet;
  AVFrame *frame;
} Decoder;
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
 * 编码器 
 */
typedef struct Encoder
{
  AVCodec *codec;
  AVCodecContext *cctx;
  int nb_packets;
  int nb_frames;
  int nb_bytes;
  int nb_samples;
  int nb_rtps;
  AVFrame *frame;
  AVPacket packet;
} Encoder;

/* 输出音频包调试信息 */
static void tms_dump_audio_packet(int nb_packets, AVPacket *packet)
{
  ast_debug(2, "读取音频包 #%d size= %d 字节\n", nb_packets, packet->size);
  if (nb_packets < 8)
  {
    uint8_t *dat = packet->data;
    ast_debug(2, "读取音频包 #%d 前8个字节 %02x %02x %02x %02x %02x %02x %02x %02x \n", nb_packets, dat[0], dat[1], dat[2], dat[3], dat[4], dat[5], dat[6], dat[7]);
  }
}

/* 输出音频帧调试信息 */
static void tms_dump_audio_frame(int nb_packets, int nb_frames, AVFrame *frame)
{
  const char *frame_fmt = av_get_sample_fmt_name(frame->format);

  ast_debug(2, "从音频包 #%d 中读取音频帧 #%d, format = %s , sample_rate = %d , channels = %d , nb_samples = %d, pts = %ld, best_effort_timestamp = %ld\n", nb_packets, nb_frames, frame_fmt, frame->sample_rate, frame->channels, frame->nb_samples, frame->pts, frame->best_effort_timestamp);
}
/* 初始化解码器 */
static int init_decoder(Decoder *decoder)
{
  int ret = 0;
  char *filename = decoder->filename;
  AVFormatContext *ifmt_ctx = NULL;
  AVStream *st;
  AVCodec *c;
  AVCodecContext *cctx;

  if ((ret = avformat_open_input(&ifmt_ctx, decoder->filename, NULL, NULL)) < 0)
  {
    ast_log(LOG_WARNING, "无法打开指定文件 %s\n", filename);
    return ret;
  }
  /* 获得指定的视频文件的信息 */
  if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0)
  {
    ast_log(LOG_WARNING, "无法获取指定文件 %s 的媒体流信息\n", filename);
    return ret;
  }
  /* 应该只包含一路媒体流 */
  if (ifmt_ctx->nb_streams != 1)
  {
    ast_log(LOG_WARNING, "文件 %s 中媒体流数量 %d 不等于1，不支持\n", filename, ifmt_ctx->nb_streams);
    return -1;
  }

  st = ifmt_ctx->streams[0]; // 媒体流

  c = avcodec_find_decoder(st->codecpar->codec_id); // 解码器
  if (NULL == c)
  {
    ast_log(LOG_WARNING, "文件 %s 中的媒体流的解码器 %s 找不到\n", filename, avcodec_get_name(st->codecpar->codec_id));
    return -1;
  }
  if (c->type != AVMEDIA_TYPE_AUDIO)
  {
    ast_log(LOG_WARNING, "文件 %s 中的媒体流 %s 不是音频\n", filename, c->name);
    return -1;
  }

  cctx = avcodec_alloc_context3(c); // 解码器上下文
  avcodec_parameters_to_context(cctx, st->codecpar);
  if ((ret = avcodec_open2(cctx, c, NULL)) < 0)
  {
    ast_log(LOG_WARNING, "读取文件 %s 媒体流基本信息失败 %s\n", filename, av_err2str(ret));
    return ret;
  }

  decoder->ictx = ifmt_ctx;
  decoder->codec = c;
  decoder->cctx = cctx;
  decoder->packet = av_packet_alloc();
  decoder->frame = av_frame_alloc();

  ast_debug(1, "文件 %s 中媒体编码 %s，时长 %s\n", filename, c->name, av_ts2str(ifmt_ctx->duration));

  return 0;
}
/* 初始化编码器 */
static int init_encoder(Encoder *encoder)
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
  cctx->bit_rate = 64000;
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
static int init_resampler(Decoder *decoder,
                          Encoder *encoder,
                          Resampler *resampler)
{
  int error;

  AVCodecContext *input_codec_context = decoder->cctx;
  AVCodecContext *output_codec_context = encoder->cctx;
  SwrContext **resample_context = &resampler->swrctx;
  ast_debug(2, "in_fmt:%d,in_sample_rate:%d,out_fmt:%d,out_sample_rate:%d\n",input_codec_context->sample_fmt,input_codec_context->sample_rate,output_codec_context->sample_fmt,output_codec_context->sample_rate);
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
  /*
  * Perform a sanity check so that the number of converted samples is
  * not greater than the number of samples to be converted.
  * If the sample rates differ, this case has to be handled differently
  */
  av_assert0(output_codec_context->sample_rate == input_codec_context->sample_rate);
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
 * Initialize one data packet for reading or writing.
 * @param packet Packet to be initialized
 */
static void init_encoder_packet(AVPacket *packet)
{
  av_init_packet(packet);
  /* Set the packet data and size so that it is recognized as being empty. */
  packet->data = NULL;
  packet->size = 0;
}
/**
 * @return Error code (0 if successful)
 */
static int init_encoder_frame(Encoder *encoder, Resampler *resampler)
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
  ast_debug(2, "sample_fmt:%d,nb_samples:%d,frame->sample_rate:%d\n",frame->format,frame->nb_samples,frame->sample_rate);
  return 0;
}
/**
 * 添加时间间隔
 */
static int add_interval(Decoder *decoder)
{
  AVFrame *frame = decoder->frame;
  /* 添加时间间隔，微秒 */
  int duration = (int)(((float)frame->nb_samples / (float)frame->sample_rate) * 1000 * 1000);
  ast_debug(2, "添加延迟时间，控制速率 samples = %d, sample_rate = %d, duration = %d \n", frame->nb_samples, frame->sample_rate, duration);
  usleep(duration);

  return duration;
}
/**
 * 执行重采样
 */
static int resample(Resampler *resampler, Decoder *decoder, Encoder *encoder)
{
  int ret = 0;

  int nb_resample_samples = av_rescale_rnd(swr_get_delay(resampler->swrctx, decoder->frame->sample_rate) + decoder->frame->nb_samples, encoder->cctx->sample_rate, decoder->frame->sample_rate, AV_ROUND_UP);

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

  ret = swr_convert(resampler->swrctx, resampler->data, nb_resample_samples, (const uint8_t **)decoder->frame->data, decoder->frame->nb_samples);
  if (ret < 0)
  {
    ast_log(LOG_ERROR, "Could not allocate destination samples\n");
    goto end;
  }

  encoder->nb_samples = nb_resample_samples;

end:
  return ret;
}
/* 发送RTP包 */
static int send_rtp(struct ast_channel *chan, char *src, char *buff,int buflen)
{
  //uint8_t *output_data = (uint8_t *)buff;//encoder->packet.data;
  //int nb_samples = encoder->nb_samples;
  
  //---add wpc 2020-12-10 
  //int count = buflen;//nb_samples;
  //int i = 0;
  //while(count > 0)
  //{
  unsigned char buffer[PKT_SIZE];
  struct ast_frame *f = (struct ast_frame *)buffer;
  //---add wpc 2020-12-10 
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
  //f->samples = nb_samples;
  /* 设置采样 */
  uint8_t *data;
  data = AST_FRAME_GET_BUFFER(f);
  /*
  if(count - 320 >=0)
  {
    
  memcpy(data, output_data+i, 320);
  count -= 320;
  i +=320;
  f->datalen = 320;
  f->samples = 160;
  ast_debug(2, "@@@f->samples:%d\n", f->samples);

  }else 
  {	
    memcpy(data, output_data+i, count);
    f->datalen = count;
    i += count;
    f->samples = count/2;
    ast_debug(2, "@@@f->samples:%d\n", f->samples);
    count = 0;
  }
  */
  memcpy(data, buff, buflen);
  f->datalen = buflen;
  f->samples = buflen;
  //encoder->nb_rtps++;

  //ast_debug(2, "@@@@duration@@@@\n");
 /* Write frame */
  ast_write(chan, f);
  ast_frfree(f);
  int duration = 20000;
  usleep(duration);
 //}
  //ast_debug(2, "完成 #%d 个音频RTP包发送 \n", encoder->nb_rtps);

  return 0;
}



// static int mp3_play(struct ast_channel *chan, const char *data)
// {
//   struct ast_module_user *u = NULL;

//   Decoder decoder = {.nb_bytes = 0, .nb_packets = 0, .nb_frames = 0, .nb_samples = 0};
//   Resampler resampler = {.max_nb_samples = 0};
//   Encoder encoder = {.nb_bytes = 0, .nb_packets = 0, .nb_frames = 0, .nb_rtps = 0};

//   int ret = 0;
//   char src[128]; // rtp.src
//   char *parse;
//   char buff[8192] = {'\0'};
//   int i = 0, j = 0;
//   AST_DECLARE_APP_ARGS(args, AST_APP_ARG(filename); AST_APP_ARG(options););

//   ast_debug(1, "mp3play %s\n", (char *)data);

//   /* Set random src */
//   snprintf(src, 128, "mp3play%08lx", ast_random());

//   /* Lock module */
//   u = ast_module_user_add(chan);

//   /* Duplicate input */
//   parse = ast_strdup(data);

//   /* Get input data */
//   AST_STANDARD_APP_ARGS(args, parse);

//   char *filename = (char *)args.filename;

//   decoder.filename = filename;

//   /* 设置解码器 */
//   if ((ret = init_decoder(&decoder)) < 0)
//   {
//     goto clean;
//   }
//   /* 设置编码器 */
//   if ((ret = init_encoder(&encoder)) < 0)
//   {
//     goto clean;
//   }
//   /* 设置重采样，将解码出的fltp采样格式，转换为s16采样格式 */
//   if ((ret = init_resampler(&decoder, &encoder, &resampler)) < 0)
//   {
//     goto clean;
//   }

//   int64_t start_time = av_gettime_relative(); // Get the current time in microseconds.

//   while (1)
//   {
//     if ((ret = av_read_frame(decoder.ictx, decoder.packet)) == AVERROR_EOF)
//     {
//       break;
//     }
//     else if (ret < 0)
//     {
//       ast_log(LOG_WARNING, "文件 %s 读取编码包失败 %s\n", filename, av_err2str(ret));
//       goto clean;
//     }

//     decoder.nb_packets++;
//     decoder.nb_bytes += decoder.packet->size;
//     ast_debug(2,"@@@nb_packets:%d,nb_bytes:%d@@@\n",decoder.nb_packets,decoder.nb_bytes);
//     tms_dump_audio_packet(decoder.nb_packets, decoder.packet);

//     /* 编码包送解码器 */
//     if ((ret = avcodec_send_packet(decoder.cctx, decoder.packet)) < 0)
//     {
//       ast_log(LOG_WARNING, "文件 %s 编码包送解码器失败 %s\n", filename, av_err2str(ret));
//       goto clean;
//     }

//     while (1)
//     {
//       /* 从解码器获取音频帧 */
//       ret = avcodec_receive_frame(decoder.cctx, decoder.frame);
//       if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
//       {
//         break;
//       }
//       else if (ret < 0)
//       {
//         ast_log(LOG_WARNING, "读取文件 %s 音频帧 #%d 错误 %s\n", filename, decoder.nb_frames + 1, av_err2str(ret));
//         goto clean;
//       }

//       decoder.nb_frames++;
//       decoder.nb_samples += decoder.frame->nb_samples;
//       tms_dump_audio_frame(decoder.nb_packets, decoder.nb_frames, decoder.frame);
//       ast_debug(2, "decoder.nb_frames:%d,decoder.frame->nb_samples:%d,decoder.nb_samples:%d\n",decoder.nb_frames,decoder.frame->nb_samples,decoder.nb_samples);
//       /* 添加时间间隔 */
//       //add_interval(&decoder);
//       //int duration = 320/8000 * 1000 * 1000;
//       //usleep(duration);
//       /* 对获得音频帧执行重采样 */
//       ret = resample(&resampler, &decoder, &encoder);
//       if (ret < 0)
//       {
//         goto clean;
//       }

//       /* 重采样后的媒体帧 */
//       ret = init_encoder_frame(&encoder, &resampler);
//       if (ret < 0)
//       {
//         goto clean;
//       }
//       encoder.nb_frames++;

//       /* 音频帧送编码器准备编码 */
//       if ((ret = avcodec_send_frame(encoder.cctx, encoder.frame)) < 0)
//       {
//         ast_log(LOG_ERROR, "音频帧发送编码器错误\n");
//         goto clean;
//       }

//       /* 要通过rtp输出的包 */
//       init_encoder_packet(&encoder.packet);

//       while (1)
//       {
//         ret = avcodec_receive_packet(encoder.cctx, &encoder.packet);
//         if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
//         {
//           break;
//         }
//         else if (ret < 0)
//         {
//           ast_log(LOG_ERROR, "Error encoding audio frame\n");
//           goto clean;
//         }
//         encoder.nb_packets++;
//         encoder.nb_bytes += encoder.packet.size;
// 	 ast_debug(2,"0000@@@strlen(buff):%d,i:%d@@@\n",(int)strlen(buff),i);

//         if(encoder.packet.size !=576) continue;
// 	ast_debug(2,"111111@@@strlen(buff):%d,i:%d@@@\n",(int)strlen(buff),i);

// 	if(strlen(buff)<=5760)
// 	{
// 		ast_debug(2,"22222@@@strlen(buff):%d,i:%d@@@\n",(int)strlen(buff),i);

// 		if(strlen(buff)==5760)
// 		{
// 			ast_debug(2,"@@@strlen(buff):%d,i:%d@@@\n",(int)strlen(buff),i);

// 			send_rtp(chan, src, buff,strlen(buff));
// 			memset(buff,0,sizeof(buff));
// 			i = 0;
// 		}else
// 		{
// 			ast_debug(2,"@@@strlen(buff):%d,i:%d,encoder.packet.size:%d@@@\n",(int)strlen(buff),i,encoder.packet.size);

// 			memcpy(buff+i,encoder.packet.data,encoder.packet.size);
// 			i += encoder.packet.size;
// 		}
// 	}else
// 	{
// 		ast_debug(2,"@@@strlen(buff):%d,i:%d@@@\n",(int)strlen(buff),i);
// 		memset(buff,0,sizeof(buff));
//                 i = 0;

// 	}
//         ast_debug(2, "生成编码包 #%d size= %d \n", encoder.nb_packets, encoder.packet.size);

//         //send_rtp(chan, src, &encoder);
//       }
//       av_packet_unref(&encoder.packet);
//       av_frame_free(&encoder.frame);
//     }
//     av_packet_unref(decoder.packet);
//   }

//   int64_t end_time = av_gettime_relative();

//   ast_debug(1, "结束播放文件 %s，共读取 %d 个包，共 %d 字节，共生成 %d 个包，共 %d 字节，共发送RTP包 %d 个，采样 %d 个，耗时 %ld\n", filename, decoder.nb_packets, decoder.nb_bytes, encoder.nb_packets, encoder.nb_bytes, encoder.nb_rtps, decoder.nb_samples, end_time - start_time);

// clean:
//   if (resampler.data)
//     av_freep(&resampler.data);

//   // if (decoder.frame)
//   //   av_frame_free(&decoder.frame);

//   // if (input_pkt)
//   //   av_packet_free(&input_pkt);

//   // if (ictx)
//   //   avformat_close_input(&ictx);

//   /* Unlock module*/
//   ast_module_user_remove(u);

//   free(parse);

//   /* Exit */
//   return ret;
// }

static void split_packet_size(struct ast_channel *chan,char *src,int split_packet_size,char *data,int data_memory_size, Encoder *encoder)
{
  char buff[640] = {'\0'};
  int index = 0,  j = 0, k = 0;
  if(encoder->packet.size > split_packet_size)
  {
    ast_debug(2,"@@@encoder->packet.size >%d,strlen(data):%d \n",split_packet_size,(int)strlen(data));
    if(strlen(data)>0)
    {
      memcpy(buff,data,strlen(data)>data_memory_size ? data_memory_size : strlen(data));
      memcpy(buff+(strlen(data)>data_memory_size ? data_memory_size : strlen(data)),encoder->packet.data,split_packet_size - (strlen(data)>data_memory_size ? data_memory_size : strlen(data)));
      send_rtp(chan, src, buff,strlen(buff));
      index = split_packet_size - (strlen(data)>data_memory_size ? data_memory_size : strlen(data));
      ast_debug(2,"@@@encoder->packet.size >%d strlen(data)>0,index:%d@@@\n",split_packet_size,index);

      //剩余数据/160循环发送,不足160再次保存到data中
      for(j=0;j<(encoder->packet.size - (split_packet_size - (strlen(data)>data_memory_size ? data_memory_size : strlen(data))))/split_packet_size;j++)
      {
        memset(buff,0,sizeof(buff));
        memcpy(buff,encoder->packet.data+(split_packet_size -(strlen(data)>data_memory_size ? data_memory_size : strlen(data)) + split_packet_size * j),split_packet_size);
        send_rtp(chan, src, buff,strlen(buff));             
        index += split_packet_size;
        ast_debug(2,"@@@encoder->packet.size >%d index:%d@@@\n",split_packet_size,index);
      }
      //encoder->packet.size %160余数保存到data中
      memset(data,0,data_memory_size);
      memcpy(data,encoder->packet.data+index,encoder->packet.size - index);
      ast_debug(2,"@@@encoder->packet.size >%d strlen(data)>0 data:%d@@@\n",split_packet_size,encoder->packet.size - index);            
      
    }else 
    {
        index = 0;
        memset(data,0,data_memory_size);
        for(j=0;j<encoder->packet.size/split_packet_size;j++)
      {
        memset(buff,0,sizeof(buff));
        memcpy(buff,encoder->packet.data + split_packet_size * j,split_packet_size);
        send_rtp(chan, src, buff,strlen(buff));
        index += split_packet_size;
        ast_debug(2,"@@@encoder->packet.size >%d strlen(data)<0 index:%d@@@\n",split_packet_size,index);
      }           
      //memcpy(buff,encoder->packet.data,160);
      memcpy(data,encoder->packet.data+index,encoder->packet.size % split_packet_size);
      ast_debug(2,"@@@encoder->packet.size >%d strlen(data)<0 data:%d@@@\n",split_packet_size,encoder->packet.size % split_packet_size);
    }


  }else if(encoder->packet.size==split_packet_size) 
  {
    
    memcpy(buff,encoder->packet.data,split_packet_size);
    send_rtp(chan, src, buff,strlen(buff));
    ast_debug(2,"@@@encoder->packet.size ==%d strlen(data)<0 @@@\n",split_packet_size);
    memset(buff,0,sizeof(buff));
  }else
  {
    /* 小于160 */
    /* code */
    ast_debug(2,"@@@encoder->packet.size < %d @@@\n",split_packet_size);
    if(strlen(data)>0)
    {
      //因为只要数据达到160就发送，encoder->packet.size<160，data<160也小于160
      ast_debug(2,"@@@encoder->packet.size < %d strlen(data)>0 @@@\n",split_packet_size);
      if((strlen(data)+encoder->packet.size)>split_packet_size)
      {
        memcpy(buff,data,(strlen(data)>data_memory_size ? data_memory_size : strlen(data)));
        memcpy(buff+(strlen(data)>data_memory_size ? data_memory_size : strlen(data)),encoder->packet.data,split_packet_size - (strlen(data)>data_memory_size ? data_memory_size : strlen(data)));
        send_rtp(chan, src, buff,strlen(buff));
        
        j = split_packet_size -(strlen(data)>data_memory_size ? data_memory_size : strlen(data));
        memset(data,0,data_memory_size);
        memcpy(data,encoder->packet.data+j,encoder->packet.size - j);
        ast_debug(2,"@@@(strlen(data)+encoder->packet.size)>%d ,j:%d, data:%d@@@\n",split_packet_size,j,encoder->packet.size - j);

      }else if((strlen(data)+encoder->packet.size)==split_packet_size)
      {
        memcpy(buff,data,(strlen(data)>data_memory_size ? data_memory_size : strlen(data)));
        memcpy(buff+(strlen(data)>data_memory_size ? data_memory_size : strlen(data)),encoder->packet.data,encoder->packet.size);
        send_rtp(chan, src, buff,strlen(buff));
        ast_debug(2,"@@@(strlen(data)+encoder->packet.size)==%d @@@\n",split_packet_size);
        memset(buff,0,sizeof(buff));
        memset(data,0,data_memory_size);
      }else
      {
        /* code */
        k = strlen(data)>data_memory_size ? data_memory_size : strlen(data);
        memcpy(data + k,encoder->packet.data,encoder->packet.size);
        ast_debug(2,"@@@(strlen(data)+encoder->packet.size)<%d packet.size:%d@@@\n",split_packet_size,encoder->packet.size);
      }
      
    }else 
    {
      memset(data,0,data_memory_size);
      memcpy(data,encoder->packet.data,encoder->packet.size);
      ast_debug(2,"@@@(strlen(data)<0 packet.size:%d@@@\n",encoder->packet.size);
    }
  }


}

static int mp3_play(struct ast_channel *chan, const char *data)
{
  struct ast_module_user *u = NULL;

  Decoder decoder = {.nb_bytes = 0, .nb_packets = 0, .nb_frames = 0, .nb_samples = 0};
  Resampler resampler = {.max_nb_samples = 0};
  Encoder encoder = {.nb_bytes = 0, .nb_packets = 0, .nb_frames = 0, .nb_rtps = 0};

  int ret = 0;
  char src[128]; // rtp.src
  char *parse;
  //char buff[8192] = {'\0'};
  //char buff[640] = {'\0'};
  char tmp[2048] = {'\0'};
  //int index = 0, i = 0, j = 0, k = 0;
  int split_size = 160;
  AST_DECLARE_APP_ARGS(args, AST_APP_ARG(filename); AST_APP_ARG(options););

  ast_debug(1, "mp3play %s\n", (char *)data);

  /* Set random src */
  snprintf(src, 128, "mp3play%08lx", ast_random());

  /* Lock module */
  u = ast_module_user_add(chan);

  /* Duplicate input */
  parse = ast_strdup(data);

  /* Get input data */
  AST_STANDARD_APP_ARGS(args, parse);

  char *filename = (char *)args.filename;

  decoder.filename = filename;

  /* 设置解码器 */
  if ((ret = init_decoder(&decoder)) < 0)
  {
    goto clean;
  }
  /* 设置编码器 */
  if ((ret = init_encoder(&encoder)) < 0)
  {
    goto clean;
  }
  /* 设置重采样，将解码出的fltp采样格式，转换为s16采样格式 */
  if ((ret = init_resampler(&decoder, &encoder, &resampler)) < 0)
  {
    goto clean;
  }

  int64_t start_time = av_gettime_relative(); // Get the current time in microseconds.

  while (1)
  {
    if ((ret = av_read_frame(decoder.ictx, decoder.packet)) == AVERROR_EOF)
    {
      break;
    }
    else if (ret < 0)
    {
      ast_log(LOG_WARNING, "文件 %s 读取编码包失败 %s\n", filename, av_err2str(ret));
      goto clean;
    }

    decoder.nb_packets++;
    decoder.nb_bytes += decoder.packet->size;
    ast_debug(2,"@@@nb_packets:%d,nb_bytes:%d@@@\n",decoder.nb_packets,decoder.nb_bytes);
    tms_dump_audio_packet(decoder.nb_packets, decoder.packet);

    /* 编码包送解码器 */
    if ((ret = avcodec_send_packet(decoder.cctx, decoder.packet)) < 0)
    {
      ast_log(LOG_WARNING, "文件 %s 编码包送解码器失败 %s\n", filename, av_err2str(ret));
      goto clean;
    }

    while (1)
    {
      /* 从解码器获取音频帧 */
      ret = avcodec_receive_frame(decoder.cctx, decoder.frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      {
        break;
      }
      else if (ret < 0)
      {
        ast_log(LOG_WARNING, "读取文件 %s 音频帧 #%d 错误 %s\n", filename, decoder.nb_frames + 1, av_err2str(ret));
        goto clean;
      }

      decoder.nb_frames++;
      decoder.nb_samples += decoder.frame->nb_samples;
      tms_dump_audio_frame(decoder.nb_packets, decoder.nb_frames, decoder.frame);
      ast_debug(2, "decoder.nb_frames:%d,decoder.frame->nb_samples:%d,decoder.nb_samples:%d\n",decoder.nb_frames,decoder.frame->nb_samples,decoder.nb_samples);
      /* 添加时间间隔 */
      //add_interval(&decoder);
      //int duration = 320/8000 * 1000 * 1000;
      //usleep(duration);
      /* 对获得音频帧执行重采样 */
      ret = resample(&resampler, &decoder, &encoder);
      if (ret < 0)
      {
        goto clean;
      }

      /* 重采样后的媒体帧 */
      ret = init_encoder_frame(&encoder, &resampler);
      if (ret < 0)
      {
        goto clean;
      }
      encoder.nb_frames++;

      /* 音频帧送编码器准备编码 */
      if ((ret = avcodec_send_frame(encoder.cctx, encoder.frame)) < 0)
      {
        ast_log(LOG_ERROR, "音频帧发送编码器错误\n");
        goto clean;
      }

      /* 要通过rtp输出的包 */
      init_encoder_packet(&encoder.packet);

      while (1)
      {
        ret = avcodec_receive_packet(encoder.cctx, &encoder.packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
          break;
        }
        else if (ret < 0)
        {
          ast_log(LOG_ERROR, "Error encoding audio frame\n");
          goto clean;
        }
        encoder.nb_packets++;
        encoder.nb_bytes += encoder.packet.size;
        split_packet_size(chan,src,split_size,tmp,sizeof(tmp), &encoder);
        //ast_debug(2, "生成编码包 #%d size= %d \n", encoder.nb_packets, encoder.packet.size);
      }
      av_packet_unref(&encoder.packet);
      av_frame_free(&encoder.frame);
    }
    ast_debug(2,"@@@avcodec_receive_frame while after!@@@\n");
    av_packet_unref(decoder.packet);
  }
  ast_debug(2,"@@@av_read_frame while after,strlen(tmp):%d !@@@\n",(int)strlen(tmp));
  if(strlen(tmp)>0)
  {

    send_rtp(chan, src, tmp,strlen(tmp));
    ast_debug(2,"@@@av_read_frame while after,#####send_rtp####strlen(tmp):%d !@@@\n",(int)strlen(tmp));
  }
 
  
  int64_t end_time = av_gettime_relative();

  ast_debug(1, "结束播放文件 %s，共读取 %d 个包，共 %d 字节，共生成 %d 个包，共 %d 字节，共发送RTP包 %d 个，采样 %d 个，耗时 %ld\n", filename, decoder.nb_packets, decoder.nb_bytes, encoder.nb_packets, encoder.nb_bytes, encoder.nb_rtps, decoder.nb_samples, end_time - start_time);

clean:
  if (resampler.data)
    av_freep(&resampler.data);

  // if (decoder.frame)
  //   av_frame_free(&decoder.frame);

  // if (input_pkt)
  //   av_packet_free(&input_pkt);

  // if (ictx)
  //   avformat_close_input(&ictx);

  /* Unlock module*/
  ast_module_user_remove(u);

  free(parse);

  /* Exit */
  return ret;
}




static int unload_module(void)
{
  int res = ast_unregister_application(app_play);

  ast_module_user_hangup_all();

  return res;
}

static int load_module(void)
{
  int res = ast_register_application(app_play, mp3_play, syn_play, des_play);

  return res;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "TMS mp3 applications");
