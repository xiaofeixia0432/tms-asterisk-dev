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
#include <libavutil/intreadwrite.h>
#include <libavutil/log.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libavutil/timestamp.h>

static const char *app_play = "TMSH264Play";
static const char *syn_play = "H264 file playblack";
static const char *des_play = "  TMSH264Play(filename,[options]):  Play h264 file to user. \n";

#define AST_FRAME_GET_BUFFER(fr) ((uint8_t *)((fr)->data.ptr))

#define PKT_PAYLOAD 1460
#define PKT_SIZE (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET + PKT_PAYLOAD)
#define PKT_OFFSET (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET)

#define FF_RTP_FLAG_H264_MODE0 8

#define TMS_DEBUG_RTP 0

typedef struct InputStream
{
  struct ast_channel *chan;
  const char *name;
  AVStream *st;
  AVCodecContext *dec_ctx;
  char *src;
  struct ast_format *format;
  int bytes_per_sample;

  int saw_first_ts;

  int64_t start; /* time when read started */
  /* predicted dts of the next packet read for this stream or (when there are
     * several frames in a packet) of the next frame in current packet (in AV_TIME_BASE units) */
  int64_t next_dts;
  int64_t dts; ///< dts of the last packet read for this stream (in AV_TIME_BASE units)
} InputStream;

#define SIGN_BIT (0x80)
#define SEG_MASK (0x70)
#define QUANT_MASK (0xf)
#define SEG_SHIFT (4)

typedef struct NALU
{
  int startcodeprefix_len; //! 4 for parameter sets and first slice in picture, 3 for everything else (suggested)
  unsigned len;            //! Length of the NAL unit (Excluding the start code, which does not belong to the NALU)
  unsigned max_size;       //! Nal Unit Buffer size
  int forbidden_bit;       //! should be always FALSE
  int nal_reference_idc;   //! NALU_PRIORITY_xxxx
  int nal_unit_type;       //! NALU_TYPE_xxxx
  char *buf;               //! contains the first byte followed by the EBSP
} NALU;

typedef struct RTPMuxContext
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
  int nal_length_size;
  int buffered_nals;

  int flags;

  unsigned int frame_count;

  InputStream *video;
} RTPMuxContext;

static void ff_rtp_send_data(RTPMuxContext *s, const uint8_t *buf1, int len, int m)
{
  if (TMS_DEBUG_RTP)
  {
    ast_debug(1, "进入ff_rtp_send_data len=%d M=%d\n", len, m);
  }

  uint8_t *data;

  uint8_t buffer[PKT_SIZE];
  struct ast_frame *f = (struct ast_frame *)buffer;
  s->timestamp = s->cur_timestamp;

  /* Unset */
  memset(f, 0, PKT_SIZE);

  AST_FRAME_SET_BUFFER(f, f, PKT_OFFSET, PKT_PAYLOAD);

  f->src = strdup(s->video->src);
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
  f->subclass.format = s->video->format;
  f->subclass.frame_ending = m;
  f->offset = AST_FRIENDLY_OFFSET;

  /* Get data pointer */
  data = AST_FRAME_GET_BUFFER(f);

  f->datalen = len;

  memcpy(data, buf1, len);

  s->frame_count++;

  /* Write frame */
  ast_write(s->video->chan, f);

  ast_frfree(f);

  if (TMS_DEBUG_RTP)
  {
    ast_debug(1, "[chan %p] 完成第 %d 个视频RTP帧发送 \n", s->video->chan, s->frame_count);
  }
}

static void flush_buffered(RTPMuxContext *s, int last)
{
  if (TMS_DEBUG_RTP)
  {
    ast_debug(1, "flush_buffered\n");
  }
  if (s->buf_ptr != s->buf)
  {
    // If we're only sending one single NAL unit, send it as such, skip
    // the STAP-A/AP framing
    if (s->buffered_nals == 1)
    {
      enum AVCodecID codec = AV_CODEC_ID_H264;
      if (codec == AV_CODEC_ID_H264)
        ff_rtp_send_data(s, s->buf + 3, s->buf_ptr - s->buf - 3, last);
      else
        ff_rtp_send_data(s, s->buf + 4, s->buf_ptr - s->buf - 4, last);
    }
    else
    {
      ff_rtp_send_data(s, s->buf, s->buf_ptr - s->buf, last);
    }
  }
  s->buf_ptr = s->buf;
  s->buffered_nals = 0;
}

static void nal_send(RTPMuxContext *s, const uint8_t *buf, int size, int last)
{
  enum AVCodecID codec = AV_CODEC_ID_H264;
  int nalu_type = buf[0] & 0x1F;

  if (TMS_DEBUG_RTP)
  {
    ast_debug(1, "Sending NAL %x of len %d M=%d\n", nalu_type, size, last);
  }
  // 跳过sei
  // if (nalu_type == 6)
  // 	return;

  if (size <= s->max_payload_size)
  {
    int buffered_size = s->buf_ptr - s->buf;
    int header_size;
    int skip_aggregate = 1;

    if (codec == AV_CODEC_ID_H264)
    {
      header_size = 1;
      //skip_aggregate = s->flags & FF_RTP_FLAG_H264_MODE0;
    }
    else
    {
      header_size = 2;
    }

    // Flush buffered NAL units if the current unit doesn't fit
    if (buffered_size + 2 + size > s->max_payload_size)
    {
      flush_buffered(s, 0);
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
        if (codec == AV_CODEC_ID_H264)
        {
          *s->buf_ptr++ = 24;
        }
        else
        {
          *s->buf_ptr++ = 48 << 1;
          *s->buf_ptr++ = 1;
        }
      }
      AV_WB16(s->buf_ptr, size);
      s->buf_ptr += 2;
      memcpy(s->buf_ptr, buf, size);
      s->buf_ptr += size;
      s->buffered_nals++;
    }
    else
    {
      flush_buffered(s, 0);
      ff_rtp_send_data(s, buf, size, last);
    }
  }
  else
  {
    int flag_byte, header_size;
    flush_buffered(s, 0);
    if (codec == AV_CODEC_ID_H264 && (s->flags & FF_RTP_FLAG_H264_MODE0))
    {
      ast_debug(1, "NAL size %d > %d, try -slice-max-size %d\n", size, s->max_payload_size, s->max_payload_size);
      return;
    }
    if (TMS_DEBUG_RTP)
    {
      ast_debug(1, "NAL size %d > %d\n", size, s->max_payload_size);
    }
    if (codec == AV_CODEC_ID_H264)
    {
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
    }
    else
    {
      uint8_t nal_type = (buf[0] >> 1) & 0x3F;
      /*
	           * create the HEVC payload header and transmit the buffer as fragmentation units (FU)
	           *
	           *    0                   1
	           *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
	           *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	           *   |F|   Type    |  LayerId  | TID |
	           *   +-------------+-----------------+
	           *
	           *      F       = 0
	           *      Type    = 49 (fragmentation unit (FU))
	           *      LayerId = 0
	           *      TID     = 1
	           */
      s->buf[0] = 49 << 1;
      s->buf[1] = 1;

      /*
	           *     create the FU header
	           *
	           *     0 1 2 3 4 5 6 7
	           *    +-+-+-+-+-+-+-+-+
	           *    |S|E|  FuType   |
	           *    +---------------+
	           *
	           *       S       = variable
	           *       E       = variable
	           *       FuType  = NAL unit type
	           */
      s->buf[2] = nal_type;
      /* set the S bit: mark as start fragment */
      s->buf[2] |= 1 << 7;

      /* pass the original NAL header */
      buf += 2;
      size -= 2;

      flag_byte = 2;
      header_size = 3;
    }

    while (size + header_size > s->max_payload_size)
    {
      memcpy(&s->buf[header_size], buf, s->max_payload_size - header_size);
      ff_rtp_send_data(s, s->buf, s->max_payload_size, 0);
      buf += s->max_payload_size - header_size;
      size -= s->max_payload_size - header_size;
      s->buf[flag_byte] &= ~(1 << 7);
    }
    s->buf[flag_byte] |= 1 << 6;
    memcpy(&s->buf[header_size], buf, size);
    ff_rtp_send_data(s, s->buf, size + header_size, last);
  }
}

static const uint8_t *ff_avc_mp4_find_startcode(const uint8_t *start,
                                                const uint8_t *end,
                                                int nal_length_size)
{
  unsigned int res = 0;

  if (end - start < nal_length_size)
    return NULL;
  while (nal_length_size--)
    res = (res << 8) | *start++;

  if (res > end - start)
    return NULL;

  return start + res;
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

static void ff_rtp_send_h264_hevc(RTPMuxContext *s, const uint8_t *buf1, int size)
{
  const uint8_t *r, *end = buf1 + size;
  int nal_length_size = 0;

  s->buf_ptr = s->buf;

  if (nal_length_size)
    r = ff_avc_mp4_find_startcode(buf1, end, nal_length_size) ? buf1 : end;
  else
    r = ff_avc_find_startcode(buf1, end);
  while (r < end)
  {
    const uint8_t *r1;

    if (nal_length_size)
    {
      r1 = ff_avc_mp4_find_startcode(r, end, nal_length_size);
      if (!r1)
        r1 = end;
      r += nal_length_size;
    }
    else
    {
      while (!*(r++))
        ;
      r1 = ff_avc_find_startcode(r, end);
    }
    nal_send(s, r, r1 - r, r1 == end);
    if (TMS_DEBUG_RTP)
    {
      ast_debug(1, "ff_rtp_send_h264_hevc.nal_send r = %p r1 = %p end = %p\n", r, r1, end);
    }
    r = r1;
  }
  flush_buffered(s, 1);
}

/* 输出视频帧调试信息 */
static void tms_dump_h264_frame(int nb_frames, AVFrame *frame, AVCodecContext *cctx)
{
  ast_debug(1, "读取 frame #%d dts = %s pts = %s key_frame = %d picture_type = %c pkt_pos=%ld pkt_size = %d coded_picture_number = %d display_picture_number = %d\n",
            nb_frames,
            av_ts2timestr(frame->pkt_dts, &cctx->time_base),
            av_ts2timestr(frame->pts, &cctx->time_base),
            frame->key_frame,
            av_get_picture_type_char(frame->pict_type),
            frame->pkt_pos,
            frame->pkt_size,
            frame->coded_picture_number,
            frame->display_picture_number);
}

/* 输出视频包调试信息 */
static void tms_dump_h264_packet(int nb_packets, AVPacket *pkt, AVCodecContext *cctx)
{
  uint8_t *pkt_data = pkt->data;

  /* 前4个字节是startcode，第5个字节是nalu_header */
  NALU nalu;
  nalu.forbidden_bit = pkt_data[4] & 0x80;     //1 bit
  nalu.nal_reference_idc = pkt_data[4] & 0x60; // 2 bit
  nalu.nal_unit_type = pkt_data[4] & 0x1f;     // 5 bit

  int64_t dts = pkt->dts == INT64_MIN ? -1 : pkt->dts;
  int64_t pts = pkt->pts == INT64_MIN ? -1 : pkt->pts;

  ast_debug(1, "读取 packet #%d size= %d nal_unit_type = %d dts = %s pts = %s\n",
            nb_packets,
            pkt->size,
            nalu.nal_unit_type,
            av_ts2timestr(dts, &cctx->time_base),
            av_ts2timestr(pts, &cctx->time_base));
  if (nb_packets < 8)
  {
    ast_debug(2, "av_read_frame.packet 前12个字节 %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", pkt_data[0], pkt_data[1], pkt_data[2], pkt_data[3], pkt_data[4], pkt_data[5], pkt_data[6], pkt_data[7], pkt_data[8], pkt_data[9], pkt_data[10], pkt_data[11]);
  }
}

static void my_av_log_callback(void *ptr, int level, const char *fmt, va_list vl)
{
  char output[120];
  vsprintf(output, fmt, vl);
  ast_debug(2, "tms ffmpeg log: %s", output);
}
/* 解码媒体帧 */
static int process_frame(char *filename, AVCodecContext *cctx, AVPacket *pkt, AVFrame *frame, int *nb_frames, int *packet_new)
{
  int ret = 0, got_frame = 0;

  /* 编码包送解码器 */
  if (*packet_new)
  {
    ret = avcodec_send_packet(cctx, pkt);
    if (ret == AVERROR(EAGAIN))
    {
      ret = 0;
    }
    else if (ret >= 0 || ret == AVERROR_EOF)
    {
      ret = 0;
      *packet_new = 0;
    }
  }

  /* 从解码器获取音频帧 */
  if (ret >= 0)
  {
    ret = avcodec_receive_frame(cctx, frame);

    if (ret >= 0)
    {
      got_frame = 1;
    }
    else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    {
      ret = 0;
    }
  }

  if (ret < 0)
    return ret;

  if (got_frame)
  {
    (*nb_frames)++;
    tms_dump_h264_frame(*nb_frames, frame, cctx);
  }

  return got_frame || *packet_new;
}
/* asterisk调用入口 */
static int h264_play(struct ast_channel *chan, const char *data)
{
  struct ast_module_user *u = NULL;
  InputStream video = {chan, 0, NULL, NULL, 0, 0, 0, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE};
  uint8_t buf[1470];
  RTPMuxContext rtp_mux_ctx = {
      .buf = buf,
      .max_payload_size = 1400,
      .buffered_nals = 0,
      .flags = 0,
      .frame_count = 0,
      .video = &video};

  char *filename; // 要打开的文件
  int ret = 0;    // 返回结果
  char src[128];  // rtp.src
  char *parse;

  AST_DECLARE_APP_ARGS(args, AST_APP_ARG(filename); AST_APP_ARG(options););

  ast_debug(1, "TMSH264Play %s\n", (char *)data);

  /* Set random src */
  snprintf(src, 128, "TMSH264Play%08lx", ast_random());
  video.src = src;

  /* Lock module */
  u = ast_module_user_add(chan);

  /* Duplicate input */
  parse = ast_strdup(data);

  /* Get input data */
  AST_STANDARD_APP_ARGS(args, parse);

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
  if (nb_streams != 1)
  {
    ast_log(LOG_WARNING, "文件中的媒体流数据不唯一 %s\n", filename);
    goto clean;
  }

  ast_debug(1, "文件 %s nb_streams = %d , duration = %s\n", filename, nb_streams, av_ts2str(ictx->duration));

  AVStream *st = ictx->streams[0];
  AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);

  if (!codec)
  {
    ast_log(LOG_WARNING, "没有获得媒体流对应的编解码器 %s\n", filename);
    goto clean;
  }
  if (codec->type != AVMEDIA_TYPE_VIDEO)
  {
    ast_log(LOG_WARNING, "媒体流的类型不是视频 %s\n", filename);
    goto clean;
  }
  if (strcmp(codec->name, "h264") != 0)
  {
    ast_log(LOG_WARNING, "媒体流的类型不是h264 %s\n", filename);
    goto clean;
  }

  AVCodecContext *cctx = avcodec_alloc_context3(codec);

  avcodec_parameters_to_context(cctx, st->codecpar);

  if ((ret = avcodec_open2(cctx, codec, NULL)) < 0)
  {
    ast_log(LOG_WARNING, "读取媒体流基本信息势失败 %s\n", av_err2str(ret));
    goto end;
  }

  struct InputStream *ist = &video;

  ist->name = codec->name;
  ist->st = st;
  ist->dec_ctx = cctx;
  ist->bytes_per_sample = av_get_bytes_per_sample(cctx->sample_fmt);
  ist->format = ast_format_h264;

  int64_t dts = st->avg_frame_rate.num ? -cctx->has_b_frames * AV_TIME_BASE / av_q2d(st->avg_frame_rate) : 0;

  ast_debug(1, "H264 Stream\n");
  ast_debug(1, "-- codec.name %s\n", codec->name);
  ast_debug(1, "-- stream.avg_frame_rate(%d, %d)\n", st->avg_frame_rate.num, st->avg_frame_rate.den);
  ast_debug(1, "-- stream.tbn(%d, %d)\n", st->time_base.num, st->time_base.den);
  ast_debug(1, "-- stream.duration = %s\n", av_ts2str(st->duration));
  ast_debug(1, "-- codec_context.has_b_frames = %d\n", cctx->has_b_frames);
  ast_debug(1, "-- dts = %ld\n", dts);

  int64_t start_time = av_gettime_relative(); // Get the current time in microseconds.
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();

  //rtp_mux_ctx.base_timestamp = av_get_random_seed();
  rtp_mux_ctx.base_timestamp = 100000;
  rtp_mux_ctx.timestamp = rtp_mux_ctx.base_timestamp;
  rtp_mux_ctx.cur_timestamp = 0;

  int nb_packets = 0, nb_frames = 0;
  while (1)
  {
    if ((ret = av_read_frame(ictx, pkt)) == AVERROR_EOF)
    {
      int64_t end_time = av_gettime_relative();
      ast_debug(1, "完成从文件中读取媒体包，共读取 %d 个包，耗时 %ld\n", nb_packets, end_time - start_time);
      break;
    }
    else if (ret < 0)
    {
      ast_log(LOG_WARNING, "从文件中读取媒体帧失败 %s\n", av_err2str(ret));
      goto clean;
    }

    nb_packets++;

    tms_dump_h264_packet(nb_packets, pkt, cctx);

    /* 对于发送rtp包来说，是不需要解析媒体帧 */
    int packet_new = 1;
    while (process_frame(filename, cctx, pkt, frame, &nb_frames, &packet_new) < 0)
      ;

    /* 计算时间戳 */
    if (!video.saw_first_ts)
    {
      video.dts = video.st->avg_frame_rate.num ? -video.dec_ctx->has_b_frames * AV_TIME_BASE / av_q2d(video.st->avg_frame_rate) : 0;
      video.next_dts = video.dts;
      video.saw_first_ts = 1;
    }
    else
    {
      video.dts = video.next_dts;
    }

    /* 添加发送间隔 */
    int64_t dts = av_rescale(video.dts, 1000000, AV_TIME_BASE);
    int64_t now = av_gettime_relative() - start_time;
    if (dts > now)
    {
      usleep(dts - now);
    }

    video.next_dts += av_rescale_q(pkt->duration, video.st->time_base, AV_TIME_BASE_Q);

    // 从ffmpeg的dts时间转为rtp的时间
    int64_t video_ts = rtp_mux_ctx.base_timestamp + dts;
    int64_t rtp_ts = av_rescale(video_ts, 90000, AV_TIME_BASE);
    rtp_mux_ctx.cur_timestamp = rtp_ts;

    ast_debug(2, "now = %ld dts = %ld base_timestamp = %d video_ts = %ld rtp_ts = %ld\n", now, dts, rtp_mux_ctx.base_timestamp, video_ts, rtp_ts);

    /* 发送RTP包 */
    ff_rtp_send_h264_hevc(&rtp_mux_ctx, pkt->data, pkt->size);
  }

  av_packet_unref(pkt);

  //Flush remaining frames that are cached in the decoder
  while (process_frame(filename, cctx, pkt, frame, &nb_frames, &(int){1}) > 0)
    ;

end:
  /* Log end */
  ast_log(LOG_DEBUG, "TMSH264Play end.\n");

clean:
  if (frame)
    av_frame_free(&frame);

  if (pkt)
    av_packet_free(&pkt);

  if (ictx)
    avformat_close_input(&ictx);

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
  int res = ast_register_application(app_play, h264_play, syn_play, des_play);

  //av_log_set_callback(my_av_log_callback);

  return res;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "TMS h264 player applications");
