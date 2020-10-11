#ifndef TMS_RTP_H
#define TMS_RTP_H

#include "asterisk/channel.h"

#define RTP_VERSION 2
#define RTCP_SR 200

#define RTP_H264_TIME_BASE 90000 // RTP中h264流的时间基

#define RTP_PCMA_TIME_BASE 8000 // RTP中pcma流的时间基

#define AST_FRAME_GET_BUFFER(fr) ((uint8_t *)((fr)->data.ptr))

#define PKT_PAYLOAD 1460
#define PKT_SIZE (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET + PKT_PAYLOAD)
#define PKT_OFFSET (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET)

typedef struct TmsPlayerContext
{
  struct ast_channel *chan;
  /* 时间 */
  int64_t start_time_us;     // 微秒
  int64_t end_time_us;       // 微秒
  int64_t pause_duration_us; // 微秒
  /* 计数器 */
  int nb_packets;
  int nb_video_packets;
  int nb_video_rtps;
  int nb_audio_packets;
  int nb_audio_frames;
  int nb_pcma_frames;
  int nb_audio_rtp_samples;
  int nb_audio_rtps;
  /* rtp and rtcp */
  uint32_t rtp_audio_ssrc;
  uint32_t rtp_video_ssrc;
  struct sockaddr_in rtp_audio_dest_addr;
  struct sockaddr_in rtp_video_dest_addr;
  int first_rtcp_auido;
  int first_rtcp_video;
} TmsPlayerContext;
/**
 * 记录视频RTP发送相关数据 
 */
typedef struct TmsAudioRtpContext
{
  uint32_t timestamp;
  uint32_t base_timestamp;
  uint32_t cur_timestamp;
  struct timeval delivery;
} TmsAudioRtpContext;

int tms_ast_channel_get_rtp_dest(struct ast_channel *chan, struct sockaddr_in *audio_addr, struct sockaddr_in *video_addr);

int tms_ast_channel_get_rtp_ssrc(struct ast_channel *chan, uint32_t *audio_ssrc, uint32_t *video_ssrc);

int tms_init_player_context(struct ast_channel *chan, TmsPlayerContext *player);

/**
 * 构造第一个rtcp包
 */
void tms_rtcp_first_sr(uint8_t *buf, uint32_t ssrc, struct timeval tv_ntp, int32_t rtcp_ts);

static void timeval2ntp(struct timeval tv, unsigned int *msw, unsigned int *lsw)
{
  unsigned int sec, usec, frac;
  sec = tv.tv_sec + 2208988800u; /* Sec between 1900 and 1970 */
  usec = tv.tv_usec;
  /*
	 * Convert usec to 0.32 bit fixed point without overflow.
	 *
	 * = usec * 2^32 / 10^6
	 * = usec * 2^32 / (2^6 * 5^6)
	 * = usec * 2^26 / 5^6
	 *
	 * The usec value needs 20 bits to represent 999999 usec.  So
	 * splitting the 2^26 to get the most precision using 32 bit
	 * values gives:
	 *
	 * = ((usec * 2^12) / 5^6) * 2^14
	 *
	 * Splitting the division into two stages preserves all the
	 * available significant bits of usec over doing the division
	 * all at once.
	 *
	 * = ((((usec * 2^12) / 5^3) * 2^7) / 5^3) * 2^7
	 */
  frac = ((((usec << 12) / 125) << 7) / 125) << 7;
  *msw = sec;
  *lsw = frac;
}

void tms_rtcp_first_sr(uint8_t *buf, uint32_t ssrc, struct timeval tv_ntp, int32_t rtcp_ts)
{
  uint32_t ntp_msw, ntp_lsw;
  uint8_t *buf_ptr;

  timeval2ntp(tv_ntp, &ntp_msw, &ntp_lsw);

  buf_ptr = buf;
  *buf_ptr++ = RTP_VERSION << 6;
  *buf_ptr++ = RTCP_SR;
  /* length */
  *buf_ptr++ = (uint8_t)(6 >> 8);
  *buf_ptr++ = (uint8_t)6;
  /* ssrc */
  *buf_ptr++ = (uint8_t)(ssrc >> 24);
  *buf_ptr++ = (uint8_t)(ssrc >> 16);
  *buf_ptr++ = (uint8_t)(ssrc >> 8);
  *buf_ptr++ = (uint8_t)ssrc;
  /* ntp */
  *buf_ptr++ = (uint8_t)(ntp_msw >> 24);
  *buf_ptr++ = (uint8_t)(ntp_msw >> 16);
  *buf_ptr++ = (uint8_t)(ntp_msw >> 8);
  *buf_ptr++ = (uint8_t)ntp_msw;
  *buf_ptr++ = (uint8_t)(ntp_lsw >> 24);
  *buf_ptr++ = (uint8_t)(ntp_lsw >> 16);
  *buf_ptr++ = (uint8_t)(ntp_lsw >> 8);
  *buf_ptr++ = (uint8_t)ntp_lsw;
  /* timestampe */
  *buf_ptr++ = (uint8_t)(rtcp_ts >> 24);
  *buf_ptr++ = (uint8_t)(rtcp_ts >> 16);
  *buf_ptr++ = (uint8_t)(rtcp_ts >> 8);
  *buf_ptr++ = (uint8_t)rtcp_ts;
  /* packet count = 0 */
  *buf_ptr++ = 0;
  *buf_ptr++ = 0;
  *buf_ptr++ = 0;
  *buf_ptr++ = 0;
  /* octet count = 0 */
  *buf_ptr++ = 0;
  *buf_ptr++ = 0;
  *buf_ptr++ = 0;
  *buf_ptr++ = 0;

  ast_debug(2, "rtcp ssrc = %d(%08x) ntp_time = [%u(%04x),%u(%04x)] rtp_ts = %d(%04x)\n", ssrc, ssrc, ntp_msw, ntp_msw, ntp_lsw, ntp_lsw, rtcp_ts, rtcp_ts);
  ast_debug(2, "rtcp = %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15], buf[16], buf[17], buf[18], buf[19]);
}

/* 解析字符串形式的地址，给结构体赋值 */
static int tms_addr_str_to_stuct(char *str, struct sockaddr_in *addr)
{
  char *delim = ":";
  char *ip, *s_port;
  int port;

  s_port = (ip = strtok(str, delim)) != NULL ? strtok(NULL, delim) : NULL;
  if (!s_port)
  {
    return -1;
  }
  port = atoi(s_port);

  bzero(addr, sizeof(struct sockaddr_in));
  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);
  addr->sin_addr.s_addr = inet_addr(ip);

  return 0;
}

/* 从channel中获得呼入端rtp地址 */
int tms_ast_channel_get_rtp_dest(struct ast_channel *chan, struct sockaddr_in *audio_addr, struct sockaddr_in *video_addr)
{
  if (!ast_channel_tech(chan) || !ast_channel_tech(chan)->func_channel_read)
  {
    return -1;
  }

  int len = 21;

  if (video_addr)
  {
    char buf[len];
    char *chan_rtp_video_dest = "rtp,dest,video";
    if (ast_channel_tech(chan)->func_channel_read(chan, NULL, chan_rtp_video_dest, buf, len))
    {
      return -1;
    }
    if (tms_addr_str_to_stuct(buf, video_addr) < 0)
    {
      return -1;
    }
  }

  if (audio_addr)
  {
    char buf[len];
    char *chan_rtp_audio_dest = "rtp,dest,audio";
    if (ast_channel_tech(chan)->func_channel_read(chan, NULL, chan_rtp_audio_dest, buf, len))
    {
      return -1;
    }
    if (tms_addr_str_to_stuct(buf, audio_addr) < 0)
    {
      return -1;
    }
  }

  return 0;
}
/* 获得通道呼入端rtp的ssrc */
int tms_ast_channel_get_rtp_ssrc(struct ast_channel *chan, uint32_t *audio_ssrc, uint32_t *video_ssrc)
{
  if (!ast_channel_tech(chan) || !ast_channel_tech(chan)->func_channel_read)
  {
    return -1;
  }
  int len = 32;
  char buf[len];

  if (audio_ssrc)
  {
    bzero(buf, len);
    char *chan_rtp_audio_ssrc = "rtcp,local_ssrc,audio";
    if (ast_channel_tech(chan)->func_channel_read(chan, NULL, chan_rtp_audio_ssrc, buf, len))
    {
      return -1;
    }
    *audio_ssrc = atoi(buf);
  }

  if (video_ssrc)
  {
    bzero(buf, len);
    char *chan_rtp_video_ssrc = "rtcp,local_ssrc,video";
    if (ast_channel_tech(chan)->func_channel_read(chan, NULL, chan_rtp_video_ssrc, buf, len))
    {
      return -1;
    }
    *video_ssrc = atoi(buf);
  }

  return 0;
}

/* 初始化播放器上下文对象 */
int tms_init_player_context(struct ast_channel *chan, TmsPlayerContext *player)
{
  player->chan = chan;
  player->start_time_us = av_gettime_relative(); // 单位是微秒
  player->end_time_us = 0;
  player->pause_duration_us = 0;
  player->nb_packets = 0;
  player->nb_video_packets = 0;
  player->nb_video_rtps = 0;
  player->nb_audio_packets = 0;
  player->nb_audio_frames = 0;
  player->nb_audio_rtp_samples = 0;
  player->nb_audio_rtps = 0;

  if (tms_ast_channel_get_rtp_dest(chan, &player->rtp_audio_dest_addr, &player->rtp_video_dest_addr) < 0)
  {
    return -1;
  }
  if (tms_ast_channel_get_rtp_ssrc(chan, &player->rtp_audio_ssrc, &player->rtp_video_ssrc) < 0)
  {
    return -1;
  }

  ast_debug(1, "音频 RTP 地址 %s:%d，视频 RTP 地址 %s:%d，音频 ssrc %d，视频 ssrc %d\n", ast_inet_ntoa(player->rtp_audio_dest_addr.sin_addr), ntohs(player->rtp_audio_dest_addr.sin_port), ast_inet_ntoa(player->rtp_video_dest_addr.sin_addr), ntohs(player->rtp_video_dest_addr.sin_port), player->rtp_audio_ssrc, player->rtp_video_ssrc);

  return 0;
}

#endif