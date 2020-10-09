# 概述

提供了 asterisk 的 docker 运行环境，内置了几个开发的 asterisk 应用，主要用于学习 asterisk。

| 目录和文件            | 说明                                                          |
| --------------------- | ------------------------------------------------------------- |
| docker-13             | 存放 asterisk13 版本的源代码。                                |
| conf                  | 存放 asterisk 配置文件。                                      |
| shell                 | 存放用于操作 asterisk 的脚本。                                |
| tms-apps              | 自定义 asterisk 应用。                                        |
| tms-ami               | asterisk ami 接口示例程序。                                   |
| tms-ari               | asterisk ari 接口示例程序。                                   |
| docker-compose.13.yml | docker-compose 文件。                                         |
| docker-up-on-mac.sh   | 在 mac 环境运行时设置环境参数并启动。                         |
| logs                  | docker-compose 启动时创建，存放 asterisk 日志。               |
| media                 | docker-compose 启动时创建，存放拨号计划中应用使用的样本文件。 |

# 制作镜像

## 下载源代码

下载：https://downloads.asterisk.org/pub/telephony/asterisk/releases/asterisk-13.33.0.tar.gz

将文件放到`docker-13`目录下，执行命令`tar -zxf asterisk-13.33.0.tar.gz`，解压后的目录为`asterisk-13.33.0`。

## 解决无法从`github`下载文件的问题

`asterisk`编译过程中需要使用`pjproject`和`jansson`这两个第三方包。在`docker-13/asterisk-13.33.0/third-party`目录中有这两个项目的目录，分别修改两个目录中的`Makefile.rules`文件，替换其中的文件下载地址。

```
# PACKAGE_URL ?= https://raw.githubusercontent.com/asterisk/third-party/master/jansson/$(JANSSON_VERSION)
PACKAGE_URL ?= http://$(HOST_DOMAIN)/third-party/master/jansson/$(JANSSON_VERSION)
```

```
# PACKAGE_URL ?= https://raw.githubusercontent.com/asterisk/third-party/master/pjproject/$(PJPROJECT_VERSION)
PACKAGE_URL ?= http://$(HOST_DOMAIN)/third-party/master/pjproject/$(PJPROJECT_VERSION)
```

如果需要安装`http-server`用于支持本地文件下载。

> npm i http-server -g

在`docker-13`目录下启动`http-server`。

> http-server -p 80

## 引入 ffmpeg 动态链接库

修改`docker-13/asterisk-13.33.0/main/Makefile`文件，引用`ffmpeg`库。

```
AST_LIBS+=-lavformat -lavcodec -lavutil -lswresample -lswscale -lavfilter
```

## 执行 docker-compose 命令

生成 docker 镜像。

> docker-compose -f docker-compose.13.yml build

# 修改配置文件（conf 目录）

`conf`目录下包含了从镜像中获得的默认配置文件，并添加了`sample`的扩展名。使用时需要复制文件，去掉`sample`扩展名。

## logger.conf

参考：https://wiki.asterisk.org/wiki/display/AST/Logging+Configuration

## rtp.conf

| 参数     | 说明         |
| -------- | ------------ |
| rtpstart | RTP 开始端口 |
| rtpend   | RTP 结束端口 |

## pjsip.conf

| 参数                       | 说明     |
| -------------------------- | -------- |
| external_media_address     | 媒体地址 |
| external_signaling_address | 信令地址 |

因为 asterisk 是在容器中运行，所以默认会使用容器的 ip 地址作为接收 sip 信令中的地址，这样会导致外部的 sip 终端无法访问，因此需要将上面两个参数指定为宿主机的地址。

配置 3 个注册分机用户`9001`，`9002` 和 `9003`。

参考：https://wiki.asterisk.org/wiki/display/AST/Configuring+res_pjsip

## extensions.conf

拨号计划。

参考：https://wiki.asterisk.org/wiki/display/AST/Dialplan

## http.conf

参考：https://wiki.asterisk.org/wiki/display/AST/Asterisk+Builtin+mini-HTTP+Server

## ari.conf

参考：https://wiki.asterisk.org/wiki/pages/viewpage.action?pageId=29395573

## manager.conf

配置 AMI。

参考：https://wiki.asterisk.org/wiki/pages/viewpage.action?pageId=4817239

# 运行镜像

- 启动镜像

> docker-compose -f docker-compose.13.yml up

启动镜像后会在项目的根目录下建立 2 个目录`logs`和`media`分别用于存放日志和要使用的媒体文件。

- 进入镜像

> docker exec -it tms-asterisk_13.33.0 bash

进入后的默认路径是`/usr/src/asterisk`。

- 编译应用

在`/usr/src/asterisk`目录下执行`make`和`make install`。

# 应用（tms-apps 目录）

执行应用前，需要在`media`目录下生成样本文件。

## 播放 alaw

文件`app_tms_alaw.c`

| 号码 | 说明                       | 样本文件                                                                 |
| ---- | -------------------------- | ------------------------------------------------------------------------ |
| 2001 | 8k 采样率，10s，裸流文件。 | ffmpeg -lavfi sine -t 10 -ar 8000 -f alaw -c:a pcm_alaw sine-8k-10s.alaw |

## 播放 mp3

文件`app_tms_mp3.c`

| 号码 | 说明                       | 样本文件                                                          |
| ---- | -------------------------- | ----------------------------------------------------------------- |
| 2002 | 8k 采样率，10s，mp3 格式。 | ffmpeg -lavfi sine -t 10 -ar 8000 -f mp3 -c:a mp3 sine-8k-10s.mp3 |

## 播放 h264

文件`app_tms_h264.c`

| 号码 | 说明                                                                   | 样本文件                                         |
| ---- | ---------------------------------------------------------------------- | ------------------------------------------------ |
| 3001 | 10s，320x240，250 帧，main/3.1，包括 I/P/B 帧，只有 1 个 I 帧。        | color-red-10s.h264                               |
| 3002 | main/3.1，包含红色（5 秒）和绿色（5 秒），rtp 帧间有间隔               | color-red-green-10s.h264                         |
| 3003 | main/3.1，包含红色（5 秒）和绿色（5 秒），rtp 帧间无间隔               | color-red-green-10s.h264                         |
| 3004 | main/3.1，包含红色（2 秒）和绿色（8 秒），rtp 帧间无间隔               | color-red-2s-green-8s.h264                       |
| 3010 | testsrc2 baseline-31 生成视频，带变化的数字，带插播                    | color-red-1s.h264, testsrc2-baseline-31-10s.h264 |
| 3011 | testsrc2 baseline-31 生成视频，带变化的数字，不带插播                  | testsrc2-baseline-31-10s.h264                    |
| 3012 | testsrc2 high 生成视频，带变化的数字，带插播，手机 linphone 播放有问题 |
| 3020 | 红绿视频交替显示                                                       | color-red-1s.h264,color-green-1s.h264            |
| 3030 | gop 等于 10 的 h264 文件。                                             | testsrc2-gop10-10s.h264                          |

用 ffmpeg 生成样本文件

- 默认规格生成 red 和 green 两个 h264 文件。

> ffmpeg -t 10 -lavfi color=red color-red-10s.h264

> ffmpeg -t 10 -lavfi color=green color-green-10s.h264

> ffmpeg -t 1 -lavfi color=red color-red-1s.h264

> ffmpeg -t 1 -lavfi color=green color-green-1s.h264

- 默认规格生成 5 秒 red 然后 5 秒 green 的 h264 文件。

> ffmpeg -t 5 -i color-red-10s.h264 -t 5 -i color-green-10s.h264 -filter_complex "[0:0][1:0] concat=n=2:v=1 [v]" -map '[v]' color-red-2s-green-8s.h264

- 默认规格生成 2 秒 red 然后 8 秒 green 的 h264 文件。

> ffmpeg -t 2 -i color-red-10s.h264 -t 8 -i color-green-10s.h264 -filter_complex "[0:0][1:0] concat=n=2:v=1 [v]" -map '[v]' color-red-2s-green-8s.h264

- 红色 h264 文件指定规格

> ffmpeg -t 10 -lavfi color=red -c:v libx264 -profile:v baseline -level 3.1 color-red-baseline-31-10s.h264

- 指定规格数字变化的 h264 文件。

> ffmpeg -t 10 -lavfi testsrc2 -c:v libx264 -profile:v baseline -level 3.1 testsrc2-baseline-31-10s.h264

- 指定规格数字变化的 h264 文件，gop 等于 10。

> ffmpeg -t 10 -lavfi testsrc2 -c:v libx264 -profile:v baseline -level 3.1 -g 10 testsrc2-gop10-10s.h264

## 播放 mp4

文件`app_tms_mp4.c`

| 号码 | 说明                                  | 样本文件                                    |
| ---- | ------------------------------------- | ------------------------------------------- |
| 4001 | 音频采样率 44.1k                      | sine-red-10s.mp4                            |
| 4002 | 音频采样率 8k                         | sine-8k-red-10s.mp4                         |
| 4020 | 多个 mp4 文件顺序播放。               | sine-8k-red-1s.mp4,sine-8k-green-1s.mp4     |
| 4021 | 添加 1 个 1 秒的引导 mp4 文件。       | sine-8k-red-1s.mp4,sine-8k-testsrc2-10s.mp4 |
| 4022 | 10 秒 testsrc2 视频，8 秒 sine 音频。 | testsrc2-baseline-31-10s-sine-8s.mp4        |
| 4030 | 播放 gop 等于 10 的标准测试文件。     | sine-8k-testsrc2-gop10-10s.mp4              |

用 ffmpeg 生成样本文件

- 默认规格的 mp4 文件

> ffmpeg -t 10 -lavfi sine -t 10 -lavfi color=red sine-red-10s.mp4

- 指定音频采样率，生成 mp4 文件

> ffmpeg -t 10 -lavfi sine -ar 8000 -t 10 -lavfi color=red sine-8k-red-10s.mp4

- 指定音频采样率，指定视频规格，生成 mp4 文件

> ffmpeg -t 1 -lavfi sine -ar 8000 -t 1 -lavfi color=red -c:v libx264 -profile:v baseline -level 31 sine-8k-red-1s.mp4

> ffmpeg -t 1 -lavfi sine -ar 8000 -t 1 -lavfi color=green -c:v libx264 -profile:v baseline -level 31 sine-8k-green-1s.mp4

- 时间变化的视频

> ffmpeg -t 10 -lavfi sine -ar 8000 -t 10 -lavfi testsrc2 -c:v libx264 -profile:v baseline -level 31 sine-8k-testsrc2-10s.mp4

- 时间延迟 2s 的 mp4

> ffmpeg -i testsrc2-baseline-31-10s.h264 -itsoffset 2 -i sine-8k-10s.mp3 -map 0:v:0 -map 1:a:0 testsrc2-baseline-31-10s-sine-8s.mp4

- 拼接媒体文件

> ffmpeg -i color-red-1s.h264 -i sine-8k-green-1s.mp4 -filter_complex "[0:0][1:0][1:0][1:1] concat=n=2:v=1:a=1 [v] [a]" -map '[v]' -map '[a]' red-1s-sine-8k-green-1s.mp4

- 控制 gop 的长度，解决丢帧不能稳定播放的问题

> ffmpeg -t 10 -lavfi sine -ar 8000 -t 10 -lavfi testsrc2 -c:v libx264 -profile:v baseline -level 3.1 -g 10 sine-8k-testsrc2-gop10-10s.mp4

- 生成静默声音的 mp4 文件。

> ffmpeg -t 1 -lavfi anullsrc=r=8000:cl=mono -lavfi color=blue -c:v libx264 -profile:v baseline -level 3.1 anullsrc-blue-1s.mp4

## 接收 dtmf

文件`app_tms_dtmf`

| 号码 | 说明                                        |
| ---- | ------------------------------------------- |
| 5001 | 按`0`播放`goodbye`，否则播放`hello-world`。 |

# AMI 接口

目录`tms-ami`下

# ARI 接口

目录`tms-ari`下

# sipp 测试

测试 asterisk 信令重发制，参考`pjsip.conf`中的`timer_t1`和`timer_b`参数。

# 其它 ffmpeg 命令

## rtp 推流

- 基础

> ffmpeg -re -i sine-8k-testsrc2-gop10-10s.mp4 -c:a pcm_alaw -vn -f rtp rtp://192.168.43.165:7078 -an -c:v copy -bsf: h264_mp4toannexb -f rtp rtp://192.168.43.165:9078

- 音频延迟 2 秒播放

ffmpeg -re -i sine-8k-testsrc2-gop10-10s.mp4 -itsoffset 2 -i sine-8k-testsrc2-gop10-10s.mp4 -map 0:v:0 -map 1:a:0 -c:a pcm_alaw -vn -f rtp rtp://192.168.43.165:7078 -an -c:v copy -bsf: h264_mp4toannexb -f rtp rtp://192.168.43.165:7080

- 音频时间戳延后 1 秒

> ffmpeg -re -i sine-8k-testsrc2-gop10-10s.mp4 -c:a pcm_alaw -vn -output_ts_offset 1 -f rtp rtp://192.168.43.165:5006 -an -c:v copy -bsf: h264_mp4toannexb -f rtp rtp://192.168.43.165:5008

## ffprobe 命令

- 查看媒体文件基本信息

> ffprobe sine-red-10s.mp4

- 查看媒体文件的 packet 并输出到文件

> ffprobe -show_packets color-red-10s.h264 > packets.txt

- 查看媒体文件的 frame 并输出到文件

> ffprobe -show_frames color-red-10s.h264 > frames.txt
