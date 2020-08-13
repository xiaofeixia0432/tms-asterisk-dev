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
 * \brief TMS alaw application -- play alaw files
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

static const char *app_play = "TMSAlawPlay";                                                  // 应用的名字，在extensions.conf中使用
static const char *syn_play = "alaw file playblack";                                          // Synopsis，应用简介
static const char *des_play = "TMSAlawPlay(filename,[options]):  Play alaw file to user. \n"; // 应用描述

#define AST_FRAME_GET_BUFFER(fr) ((uint8_t *)((fr)->data.ptr))

#define PKT_PAYLOAD 1460
#define PKT_SIZE (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET + PKT_PAYLOAD)
#define PKT_OFFSET (sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET)

#define ALAW_SAMPLE_RATE 8000 // alaw采样率
#define BYTES_PER_SAMPLE 1    // 每个采样的字节数
#define MAX_PKT_SAMPLES 320   // 每个rtp帧中包含的采样数

static int alaw_play(struct ast_channel *chan, const char *data)
{
  struct ast_module_user *u = NULL;

  int ret = 0;
  char src[128]; // rtp.src
  char *parse;

  AST_DECLARE_APP_ARGS(args, AST_APP_ARG(filename); AST_APP_ARG(options););

  ast_debug(1, "alawplay %s\n", (char *)data);

  /* Set random src */
  snprintf(src, 128, "alawplay%08lx", ast_random());

  /* Lock module */
  u = ast_module_user_add(chan);

  /* Duplicate input */
  parse = ast_strdup(data);

  /* Get input data */
  AST_STANDARD_APP_ARGS(args, parse);

  char *filename = (char *)args.filename;

  struct ast_str *codec_buf = ast_str_alloca(AST_FORMAT_CAP_NAMES_LEN);

  ast_debug(1, "RTP通道 %s writeformat = %s, rawwriteformat = %s,  nativeformats = %s\n",
            ast_channel_name(chan),
            ast_format_get_name(ast_channel_writeformat(chan)),
            ast_format_get_name(ast_channel_rawwriteformat(chan)),
            ast_format_cap_get_names(ast_channel_nativeformats(chan), &codec_buf));

  /* 打开alaw文件 */
  FILE *file_alaw = NULL;
  if ((file_alaw = fopen(filename, "rb")) == NULL)
  {
    ast_log(LOG_WARNING, "无法打开指定文件 %s\n", filename);
    goto clean;
  }

  int nb_rtps = 0;          // rtp包发送数据
  int nb_total_samples = 0; // 总采样数

  while (!feof(file_alaw))
  {
    nb_rtps++;

    int duration = 0;                                   // 每帧之间添加的发送延时
    int8_t samples[BYTES_PER_SAMPLE * MAX_PKT_SAMPLES]; // 从文件中读取的采样
    size_t nb_samples;                                  // 获得的采样数

    nb_samples = fread(samples, BYTES_PER_SAMPLE, MAX_PKT_SAMPLES, file_alaw);
    if (nb_samples <= 0)
    {
      break;
    }
    nb_total_samples += nb_samples;

    unsigned char buffer[PKT_SIZE];
    struct ast_frame *f = (struct ast_frame *)buffer;

    /* Unset */
    memset(f, 0, PKT_SIZE);

    AST_FRAME_SET_BUFFER(f, f, PKT_OFFSET, PKT_PAYLOAD);
    f->src = strdup(src);
    /* 设置帧类型和编码格式 */
    f->frametype = AST_FRAME_VOICE;
    f->subclass.format = ast_format_alaw;
    f->delivery.tv_usec = 0;
    f->delivery.tv_sec = 0;
    /* Don't free the frame outside */
    f->mallocd = 0;
    f->offset = AST_FRIENDLY_OFFSET;

    /* 帧中包含的采样数 */
    f->samples = nb_samples;
    /* 每帧包含的采样数据 */
    f->datalen = nb_samples;
    uint8_t *data;
    data = AST_FRAME_GET_BUFFER(f);
    memcpy(data, samples, nb_samples);

    ast_debug(2, "准备发送第 %d 个RTP帧，包含采样数 %ld\n", nb_rtps, nb_samples);

    /* Write frame */
    ast_write(chan, f);

    /* 计算每一帧采样的持续时间，设置发送延迟 */
    duration = (int)(((float)nb_samples / (float)ALAW_SAMPLE_RATE) * 1000 * 1000);
    ast_debug(2, "完成第 %d 个RTP帧发送，添加延时 %d\n", nb_rtps, duration);
    usleep(duration);
  }

  /* Log end */
  ast_debug(1, "结束播放文件 %s，共发送RTP包 %d 个，采样 %d 个\n", filename, nb_rtps, nb_total_samples);

clean:
  if (file_alaw)
    fclose(file_alaw);

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
  int res = ast_register_application(app_play, alaw_play, syn_play, des_play);

  return res;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "TMS alaw applications");
