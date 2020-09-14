# tms-asterisk

提供了 asterisk 的 docker 运行环境，内置了几个开发的 asterisk 应用，主要用于学习 asterisk。

# 制作镜像

## 下载源代码

下载：https://downloads.asterisk.org/pub/telephony/asterisk/releases/asterisk-13.33.0.tar.gz

将文件放到`docker-13`目录下，执行命令`tar -zxf asterisk-13.33.0.tar.gz`。

## 解决无法从`github`下载文件的问题

`asterisk`编译过程中需要使用`pjproject`和`jansson`这两个第三方包。在`docker-13/third-party`目录中有这两个项目的目录，分别修改两个目录中的`Makefile.rules`，替换其中的文件下载地址。

```
# PACKAGE_URL ?= https://raw.githubusercontent.com/asterisk/third-party/master/jansson/$(JANSSON_VERSION)
PACKAGE_URL ?= http://$(HOST_DOMAIN)/third-party/master/jansson/$(JANSSON_VERSION)
```

```
# PACKAGE_URL ?= https://raw.githubusercontent.com/asterisk/third-party/master/pjproject/$(PJPROJECT_VERSION)
PACKAGE_URL ?= http://$(HOST_DOMAIN)/third-party/master/pjproject/$(PJPROJECT_VERSION)
```

> npm i http-server -g

> http-server -p 80

## 引入 ffmpeg 动态链接库

修改`main/Makefile`文件，引用`ffmpeg`库。

```
AST_LIBS+=-lavformat -lavcodec -lavutil -lswresample -lswscale -lavfilter
```

## 执行 docker-compose 命令

生成镜像

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

配置 3 个注册分机用户`6001`，`6002` 和 `6003`。

参考：https://wiki.asterisk.org/wiki/display/AST/Configuring+res_pjsip

## extensions.conf

拨号计划。

参考：https://wiki.asterisk.org/wiki/display/AST/Dialplan

## http.conf

参考：https://wiki.asterisk.org/wiki/display/AST/Asterisk+Builtin+mini-HTTP+Server

## ari.conf

参考：https://wiki.asterisk.org/wiki/pages/viewpage.action?pageId=29395573

# 媒体文件

将要使用的媒体文件放在`media`的目录中。

需要通过命令行生成`extensions.conf`中使用的样本文件。

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

| 号码 | 功能               | 代码           | 说明                                                                   |
| ---- | ------------------ | -------------- | ---------------------------------------------------------------------- |
| 1001 | 播放 alaw 格式文件 | app_tms_alaw.c |                                                                        |
| 2001 | 播放 mp3 格式文件  | app_tms_mp3.c  |                                                                        |
| 3001 | 播放 h264 格式文件 | app_tms_h264.c | main/3.1                                                               |
| 3002 | 播放 h264 格式文件 | app_tms_h264.c | main/3.1，包含红色和绿色                                               |
| 3003 | 播放 h264 格式文件 | app_tms_h264.c | main/3.1，包含红色（5 秒）和绿色（5 秒），rtp 帧间无间隔               |
| 3004 | 播放 h264 格式文件 | app_tms_h264.c | main/3.1，包含红色（2 秒）和绿色（8 秒），rtp 帧间无间隔               |
| 3010 | 播放 h264 格式文件 | app_tms_h264.c | testsrc2 baseline-31 生成视频，带变化的数字，带插播                    |
| 3011 | 播放 h264 格式文件 | app_tms_h264.c | testsrc2 baseline-31 生成视频，带变化的数字，不带插播                  |
| 3012 | 播放 h264 格式文件 | app_tms_h264.c | testsrc2 high 生成视频，带变化的数字，带插播，手机 linphone 播放有问题 |
| 3020 | 播放 h264 格式文件 | app_tms_h264.c | 红绿视频交替显示                                                       |
| 4001 | 播放 mp4 格式文件  | app_tms_mp4.c  | 采样率 44.1k                                                           |
| 4002 | 播放 mp4 格式文件  | app_tms_mp4.c  | 采样率 8k                                                              |
| 4022 | 播放 mp4 格式文件  | app_tms_mp4.c  | 10 秒 testsrc2 视频，8 秒 sine 音频。                                  |

因为 rtp 接收端音频采样率位 8k，如果输入的音频采样率高，会导致失真。

# ffmpeg 命令

在`media`目录下，可以使用`ffmpeg`命令生成指定规格的样本文件。文件名称应该和`extensions.conf`中的内容一致。

红色 h264 文件，包括 I/P/B 帧，只有 1 个 I 帧

> ffmpeg -t 10 -lavfi color=red color-red-10s.h264

红色 h264 文件指定规格

> ffmpeg -t 10 -lavfi color=red -c:v libx264 -profile:v baseline -level 3.1 color-red-baseline-31-10s.h264

绿色 h264 文件，包括 I/P/B 帧，只有 1 个 I 帧

> ffmpeg -t 10 -lavfi color=green color-green-10s.h264

将红色和绿色两个视频合并为 1 个 10 秒的视频

> ffmpeg -t 2 -i color-red-10s.h264 -t 8 -i color-green-10s.h264 -filter_complex "[0:0][1:0] concat=n=2:v=1 [v]" -map '[v]' color-red-2s-green-8s.h264

数字变化的 h264 文件

> ffmpeg -t 10s -lavfi testsrc2 testsrc2-10s.h264

默认规则的 mp4 文件

> ffmpeg -t 10 -lavfi sine -t 10 -lavfi color=red sine-red-10s.mp4

指定音频采样率，生成 mp4 文件

> ffmpeg -t 10 -lavfi sine -ar 8000 -t 10 -lavfi color=red sine-8k-red-10s.mp4

> ffmpeg -t 1 -lavfi sine -ar 8000 -t 1 -lavfi color=red -c:v libx264 -profile:v baseline -level 31 sine-8k-red-1s.mp4

> ffmpeg -t 1 -lavfi sine -ar 8000 -t 1 -lavfi color=green -c:v libx264 -profile:v baseline -level 31 sine-8k-green-1s.mp4

> ffmpeg -t 10 -lavfi sine -ar 8000 -t 10 -lavfi testsrc2 -c:v libx264 -profile:v baseline -level 31 sine-8k-testsrc2-10s.mp4

> ffmpeg -t 1 -lavfi sine -ar 8000 -t 1 -lavfi color=red -c:v libx264 -profile:v baseline -level 31 sine-8k-red-1s.

> ffmpeg -i color-red-1s.h264 -i sine-8k-green-1s.mp4 -filter_complex "[0:0][1:0][1:0][1:1] concat=n=2:v=1:a=1 [v] [a]" -map '[v]' -map '[a]' red-1s-sine-8k-green-1s.mp4

> ffmpeg -i testsrc2-baseline-31-10s.h264 -itsoffset 1 -i sine-8k-10s.mp3 -map 0:v:0 -map 1:a:0 testsrc2-baseline-31-10s-sine-9s.mp4

> ffmpeg -t 1 -lavfi anullsrc=r=8000:cl=mono -lavfi color=blue -c:v libx264 -profile:v baseline -level 3.1 anullsrc-blue-1s.mp4

# ffprobe 命令

查看媒体文件基本信息

> ffprobe sine-red-10s.mp4

查看媒体文件的 packet 并输出到文件

> ffprobe -show_packets color-red-10s.h264 > packets.txt

查看媒体文件的 frame 并输出到文件

> ffprobe -show_frames color-red-10s.h264 > frames.txt
