#include <stdlib.h>

#include "asterisk/frame.h"

#ifndef TMS_H264_H
#define TMS_H264_H

#include "tms_rtp.h"

#define FF_RTP_FLAG_H264_MODE0 8

/**
 * 记录视频RTP发送相关数据 
 */
typedef struct TmsVideoRtpContext
{
  //const AVClass *av_class;
  //AVFormatContext *ic;
  //AVStream *st;
  //int payload_type;
  //uint32_t ssrc;
  //const char *cname;
  //int seq;
  uint32_t timestamp;
  uint32_t base_timestamp;
  uint32_t cur_timestamp;
  int max_payload_size;
  //int num_frames;

  /* rtcp sender statistics */
  //int64_t last_rtcp_ntp_time;
  //int64_t first_rtcp_ntp_time;
  //unsigned int packet_count;
  //unsigned int octet_count;
  //unsigned int last_octet_count;
  //int first_packet;
  /* buffer for output */
  uint8_t *buf;
  uint8_t *buf_ptr;

  //int max_frames_per_packet;

  /**
    * Number of bytes used for H.264 NAL length, if the MP4 syntax is used
    * (1, 2 or 4)
    */
  // int nal_length_size;
  int buffered_nals;

  int flags;
} TmsVideoRtpContext;

void ff_rtp_send_h264(TmsVideoRtpContext *s, const uint8_t *buf1, int size, TmsPlayerContext *player);

void tms_dump_audio_frame(AVFrame *frame, TmsPlayerContext *player);

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

  /* Set type */
  f->frametype = AST_FRAME_VIDEO;
  /* Set number of samples */
  f->samples = 1;
  // f->delivery.tv_usec = 0;
  // f->delivery.tv_sec = 0;
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

  ast_debug(2, "[chan %p] 完成第 %d 个视频RTP帧发送\n", player->chan, player->nb_video_rtps);
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

void ff_rtp_send_h264(TmsVideoRtpContext *s, const uint8_t *buf1, int size, TmsPlayerContext *player)
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

/* 输出音频帧调试信息 */
void tms_dump_audio_frame(AVFrame *frame, TmsPlayerContext *player)
{
  // 	uint8_t *frame_dat = frame->extended_data[0];
  // 	ast_debug(2, "frame前8个字节 %02x %02x %02x %02x %02x %02x %02x %02x \n", frame_dat[0], frame_dat[1], frame_dat[2], frame_dat[3], frame_dat[4], frame_dat[5], frame_dat[6], frame_dat[7]);

  const char *frame_fmt = av_get_sample_fmt_name(frame->format);

  ast_debug(2, "从音频包 #%d 中读取音频帧 #%d, format = %s , sample_rate = %d , channels = %d , nb_samples = %d, pts = %ld, best_effort_timestamp = %ld\n", player->nb_audio_packets, player->nb_audio_frames, frame_fmt, frame->sample_rate, frame->channels, frame->nb_samples, frame->pts, frame->best_effort_timestamp);
}

#endif