# 概述

提供了 asterisk 的 docker 运行环境，内置了几个自己开发的 asterisk 应用，主要用于学习 asterisk。

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

## 引入 ffmpeg 动态链接库

修改`docker-13/asterisk-13.33.0/main/Makefile`文件，引用`ffmpeg`库。

```
AST_LIBS+=-lavformat -lavcodec -lavutil -lswresample -lswscale -lavfilter
```

## 执行 docker-compose 命令

生成 docker 镜像。

如果需要，先安装`http-server`用于支持本地文件下载（用其它支持文件下载的服务也行）。

> npm i http-server -g

构建过程需要编译 asterisk 时（首次必须），在`docker-13`目录下启动`http-server`。

> http-server -p 80

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

上下文`tms-playback`中定义了用于验证播放媒体文件的自定义应用。

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

在`/usr/src/asterisk`目录下执行`make`和`make install`。用 docker-compose 启动时会自动执行。

# 应用（tms-apps 目录）

执行应用前，需要在`media`目录下生成样本文件。

## 播放 alaw

文件`app_tms_alaw.c`，不进行任何编解码工作，从文件中读取数据后直接通过 asterisk 发送。

| 号码 | 说明                       | 样本文件         |
| ---- | -------------------------- | ---------------- |
| 2001 | 8k 采样率，10s，裸流文件。 | sine-8k-10s.alaw |

用 ffmpeg 生成样本文件

> ffmpeg -lavfi sine -t 10 -ar 8000 -f alaw -c:a pcm_alaw sine-8k-10s.alaw

## 播放 mp3

文件`app_tms_mp3.c`，用`ffmpeg`进行编解码（要求输入文件的采样率必须是`8k`，从高采样率进行重采样会有严重的失真），通过 asrterisk 发送。

| 号码 | 说明                       | 样本文件        |
| ---- | -------------------------- | --------------- |
| 2002 | 8k 采样率，10s，mp3 格式。 | sine-8k-10s.mp3 |

用 ffmpeg 生成样本文件

> ffmpeg -lavfi sine -t 10 -ar 8000 -f mp3 -c:a mp3 sine-8k-10s.mp3

## 播放 h264

文件`app_tms_h264.c`，用`ffmpeg`解码 h264 文件后通过 asterisk 发送。解码的速度很快，如果每读取一个包就立刻发送，客户端不能及时处理，所以需要添加时间间隔。应用第 2 个参数可以指定是否需要添加时间间隔，值为`tight`时不添加。虽然发送的快，但是时间戳和`dts`一致。

用`linphone`作为客户端接收 h264 视频时，播放效果受编码格式的影响。例如：用 ffmpeg 生成默认规格的红色 h264 视频（3001），在 pc 端上可以正常播放，但是手机无法播放。这是因为生成的文件中只有 1 个 I 帧，如果这个帧如果`linphone`客户端不能正确处理，那么后续的帧都无法播放。为了验证这个问题，用 ffmpeg 分别生成时长为 1 秒的红色和绿色 h264 文件，红绿交叉共 6 个文件播放 6 秒（3002），完整播放应该看到红-绿-红-绿-红-绿，pc 上可以完整播放，手机上会频繁丢掉第 1 个红色。基于这个测试，是否可以通过在要播放的视频前添加一个引导视频解决前面的帧会丢失的问题？用 ffmpeg 生成一个画面变化（包含播放时间）的视频（3003），`linphone`在 pc 上可以播放，手机上大概率不能播放；在视频前添加 1 个时长 1 秒的视频（3004），在 pc 上可以看到两个视频，在手机上大概率只能看到 1 个视频。为了保证视频不因为丢失关键帧导致无法播放，可以在 ffmpeg 生成视频时指定`gop`大小，gop 就是两个 I 帧之间包含的帧数量，显然 gop 越小，包含的关键帧越多，但是文件也越大。用 ffmpeg 生成 1 个 gop 等于 10 的带播放时间的视频（3000），pc 和手机都可以正常播放。

| 号码 | 说明                                                           | 样本文件                                        |
| ---- | -------------------------------------------------------------- | ----------------------------------------------- |
| 3000 | gop 等于 10 的 h264 文件。                                     | testsrc2-baseline31-gop10-10s.h264              |
| 3001 | 10s，320x240，250 帧，main/31，包括 I/P/B 帧，只有 1 个 I 帧。 | color-red-10s.h264                              |
| 3002 | 长度为 1 秒的红、绿视频各 3 个，baseline/31，交替显示。        | color-red-1s.h264,color-green-1s.h264           |
| 3003 | testsrc2 baseline-31 生成视频，带变化的数字                    | testsrc2-baseline31-10s.h264                    |
| 3004 | testsrc2 baseline-31 生成视频，带变化的数字，带引导视频        | color-red-1s.h264, testsrc2-baseline31-10s.h264 |

读取 h264 文件的速度是远高于播放速度，如果读取了数据包就发送，那么客户端能正常处理吗？用 ffmpeg 制作一个 前 5 秒红色后 5 秒绿色的视频（3011），`linphone`可以正常播放。播放时设置为不添加时间间隔，读取后就发送（3012），但是要等到播放时长才挂断，只能看到红色，绿色视频丢失（猜测是发送的帧太多被`linphone`丢弃了）。用 ffmpeg 制作一个 前 2 秒红色后 8 秒绿色的视频（3013），读取后就发送，可以看到红色但是播放时间不足 2 秒，之后都是绿色，猜测是`linphone`的缓冲区满了，被后面的内容替换掉。从测试结果看，似乎`linphone`并不是按照时间戳控制视频的播放，而是按照收到的时间，因此必须在发送端控制发送的速率。

| 号码 | 说明                                                    | 样本文件                   |
| ---- | ------------------------------------------------------- | -------------------------- |
| 3011 | main/31，包含红色（5 秒）和绿色（5 秒），rtp 帧间有间隔 | color-red-green-10s.h264   |
| 3012 | main/31，包含红色（5 秒）和绿色（5 秒），rtp 帧间无间隔 | color-red-green-10s.h264   |
| 3013 | main/31，包含红色（2 秒）和绿色（8 秒），rtp 帧间无间隔 | color-red-2s-green-8s.h264 |

通过 ffmpeg 生成 h264 文件时可以通过`profile`和`level`两个参数指定规格，如果不指定默认的规格是`high/13`。生成带播放时间默认规格的文件（3021），`linphone`在 pc 端可以正常播放，在手机端无发播放；生成带播放时间默认规格 gop 等于 10 的文件（3022），pc 端和手机端都可以正常播放。

| 号码 | 说明                                                            | 样本文件                |
| ---- | --------------------------------------------------------------- | ----------------------- |
| 3021 | testsrc2 high/13 生成视频，带变化的数字，带 2 个 1 秒的引导视频 | testsrc2-10s.h264       |
| 3022 | testsrc2 high/13 生成视频，带变化的数字，带 2 个 1 秒的引导视频 | testsrc2-gop10-10s.h264 |

用 ffmpeg 生成样本文件

- 规格 main/31，单色，10s

> ffmpeg -t 10 -lavfi color=red -c:v libx264 -profile:v main -level 3.1 color-red-10s.h264

> ffmpeg -t 10 -lavfi color=green -c:v libx264 -profile:v main -level 3.1 color-green-10s.h264

- 规格 baseline/31，单色

> ffmpeg -t 1 -lavfi color=red -c:v libx264 -profile:v baseline -level 3.1 color-red-1s.h264

> ffmpeg -t 1 -lavfi color=green -c:v libx264 -profile:v baseline -level 3.1 color-green-1s.h264

- 规格 baseline/31，显示播放时间，10 秒

> ffmpeg -t 10 -lavfi testsrc2 -c:v libx264 -profile:v baseline -level 3.1 testsrc2-baseline31-10s.h264

- 规格 baseline/31，显示播放时间，gop 等于 10，10 秒

> ffmpeg -t 10 -lavfi testsrc2 -c:v libx264 -profile:v baseline -level 3.1 -g 10 testsrc2-baseline31-gop10-10s.h264

- 规格 main/31，5 秒红色+ 5 秒绿色

> ffmpeg -t 5 -i color-red-10s.h264 -t 5 -i color-green-10s.h264 -filter_complex "[0:0][1:0] concat=n=2:v=1 [v]" -map '[v]' color-red-2s-green-8s.h264

- 规格 main/31，2 秒红色+ 8 秒绿色

> ffmpeg -t 2 -i color-red-10s.h264 -t 8 -i color-green-10s.h264 -filter_complex "[0:0][1:0] concat=n=2:v=1 [v]" -map '[v]' color-red-2s-green-8s.h264

- 规格 baseline/31，10 秒，红色

> ffmpeg -t 10 -lavfi color=red -c:v libx264 -profile:v baseline -level 3.1 color-red-baseline31-10s.h264

- 规格 high/13，显示播放时间，10 秒

> ffmpeg -t 10 -lavfi testsrc2 testsrc2-10s.h264

- 规格 high/13，显示播放时间，10 秒，gop 等于 10

> ffmpeg -t 10 -lavfi testsrc2 -g 10 testsrc2-gop10-10s.h264

## 播放 mp4

文件`app_tms_mp4.c`，用`ffmpeg`解码 mp4 文件后通过 asterisk 发送，音频会重采样为 8k。生成默认规格的 10 秒 mp4 文件（4001），播放没有问题，但是声音有严重的失真，这是因为默认的音频采样率是 44.1k，转为 8k 导致失真；将音频采样率指定为 8k（4002），再播就没有声音失真问题了。

| 号码 | 说明                                      | 样本文件                                          |
| ---- | ----------------------------------------- | ------------------------------------------------- |
| 4000 | baseline/31，gop 等于 10；音频采样率 8k。 | sine-8k-testsrc2-baseline31-gop10-10s.mp4         |
| 4001 | 音频采样率 44.1k                          | sine-red-10s.mp4                                  |
| 4002 | 音频采样率 8k                             | sine-8k-red-10s.mp4                               |
| 4003 | 播放 9:16 竖屏。                          | sine-8k-testsrc2-baseline31-gop10-10s-360x640.mp4 |

- 视频规格 high/13，红色；音频采样率 44.1k；10s

> ffmpeg -t 10 -lavfi sine -t 10 -lavfi color=red sine-red-10s.mp4

- 视频规格 high/13，红色；音频采样率 8k；10s

> ffmpeg -t 10 -lavfi sine -ar 8000 -t 10 -lavfi color=red sine-8k-red-10s.mp4

- 视频规格 baseline/31，显示播放时间，gop 等于 10；音频采样率 8k 率；10 秒

> ffmpeg -t 10 -lavfi sine -ar 8000 -t 10 -lavfi testsrc2 -c:v libx264 -profile:v baseline -level 3.1 -g 10 sine-8k-testsrc2-baseline31-gop10-10s.mp4

- 设置图像的分辨率和显示比例

> ffmpeg -t 10 -lavfi sine -ar 8000 -t 10 -lavfi testsrc2 -aspect 360:640 -c:v libx264 -profile:v baseline -level 3.1 -g 10 -s 360x640 sine-8k-testsrc2-baseline31-gop10-10s-360x640.mp4

通常 mp4 文件中包含视频流和音频流，但是音频流和视频流不一定总是同步的，下面尝试几种音视频不同步的方式。mp4 文件中包含音频流，但是为静默音（4011），这样能够实现感觉上“不同步”；拼接媒体时，控制音频流完 2 秒开始（4012），产生真正的不同步（音频流的时长不变，只是延后 2 秒，因此视频流结束后，音频仍然会播放 2 秒）。

| 号码 | 说明                                  | 样本文件                                     |
| ---- | ------------------------------------- | -------------------------------------------- |
| 4011 | 1 秒，静默音，蓝色                    | anullsrc-blue-1s.mp4                         |
| 4012 | 10 秒 testsrc2 视频，8 秒 sine 音频。 | testsrc2-baseline31-gop10-10s-sine-8k-8s.mp4 |

- 视频规格 baseline/31，蓝色；音频采样率 8k，静默音，单声道；1s

> ffmpeg -t 1 -lavfi anullsrc=r=8000:cl=mono -lavfi color=blue -c:v libx264 -profile:v baseline -level 3.1 anullsrc-blue-1s.mp4

- 拼接 h264 和 mp3 文件；视频规格 baseline/31，显示播放时间，gop 等于 10，10 秒；音频延迟 2s，采样率 8k，8 秒

> ffmpeg -i testsrc2-baseline31-gop10-10s.h264 -itsoffset 2 -i sine-8k-10s.mp3 -map 0:v:0 -map 1:a:0 -c:v libx264 -profile:v baseline -level 3.1 -g 10 testsrc2-baseline31-gop10-10s-sine-8k-8s.mp4

播放控制功能。支持通过`dtmf`控制停止，暂停和恢复播放；支持设置重播次数和播放总时长（暂停时间不计入播放时长）。

| 号码 | 说明                                                     | 样本文件                                  |
| ---- | -------------------------------------------------------- | ----------------------------------------- |
| 4021 | 按`0`键退出，按`1`暂停，按`2`恢复。                      | sine-8k-testsrc2-baseline31-gop10-10s.mp4 |
| 4022 | 重复播放 3 遍，按`0`键退出，按`1`暂停，按`2`恢复。       | sine-8k-testsrc2-baseline31-gop10-10s.mp4 |
| 4023 | 播放 5 秒，超时退出；按`0`键退出，按`1`暂停，按`2`恢复。 | sine-8k-testsrc2-baseline31-gop10-10s.mp4 |

## 接收 dtmf

文件`app_tms_dtmf`

| 号码 | 说明                                        |
| ---- | ------------------------------------------- |
| 5001 | 按`0`播放`goodbye`，否则播放`hello-world`。 |

# AMI 接口

目录`tms-ami`下

nodejs 版本

实现点击拨号

接收命令

node app.js

在`manager.conf`文件中添加用户

# ARI 接口

通过实现`视频IVR`演示`ARI`接口的使用。

## 制作数据

用图像生成首页视频

> ffmpeg -f image2 -stream_loop 250 -i home.png -pix_fmt yuv420p -s 320x240 -c:v libx264 -profile:v baseline -level 3.1 -g 10 home.mp4

用图片生成 1 级视频

> ffmpeg -f image2 -stream_loop 250 -i red.png -pix_fmt yuv420p -s 320x240 -c:v libx264 -profile:v baseline -level 3.1 -g 10 1_1.mp4

> ffmpeg -f image2 -stream_loop 250 -i green.png -pix_fmt yuv420p -s 320x240 -c:v libx264 -profile:v baseline -level 3.1 -g 10 1_2.mp4

> ffmpeg -f image2 -stream_loop 250 -i blue.png -pix_fmt yuv420p -s 320x240 -c:v libx264 -profile:v baseline -level 3.1 -g 10 1_3.mp4

## 在`dialplan`中实现

6010

## 用`ari`实现

目录`tms-ari`下

# 早期媒体

# sipp 测试

因为 SIP 客户端，例如：linphone，不一定支持所有头（100rel，P-Early-Media 等），因此有些功能的验证需要引入测试工具`sipp`。

## 测试 asterisk 响应

> sipp host.tms.asterisk:5060 -p 20001 -sf 1001.xml -inf 9002.csv -l 1 -m 1 -r 1 -trace_screen -bg

> sipp host.tms.asterisk:5060 -p 20001 -sf 1002.xml -inf 9002.csv -l 1 -m 1 -r 1 -trace_screen -bg

> sipp host.tms.asterisk:5060 -p 20001 -l 1 -m 1 -r 1 -trace_screen -bg -inf 9002.csv -sf 1003.xml

> sipp host.tms.asterisk:5060 -p 20001 -l 1 -m 1 -r 1 -trace_screen -bg -inf 9002.csv -sf 1004.xml

> sipp host.tms.asterisk:5060 -p 20001 -l 1 -m 1 -r 1 -trace_screen -bg -inf 9002.csv -sf 1005.xml

> sipp host.tms.asterisk:5060 -p 20001 -l 1 -m 1 -r 1 -trace_screen -bg -inf 9002.csv -sf 1006.xml

## 测试 asterisk 重发机制

测试 asterisk 信令重发制，参考`pjsip.conf`中的`timer_t1`和`timer_b`参数。

# 其它 ffmpeg 命令

## rtp 推流

- 基础

> ffmpeg -re -i sine-8k-testsrc2-gop10-10s.mp4 -c:a pcm_alaw -vn -f rtp rtp://host.tms.asterisk:7078 -an -c:v copy -bsf: h264_mp4toannexb -f rtp rtp://host.tms.asterisk:9078

- 音频延迟 2 秒播放

> ffmpeg -re -i sine-8k-testsrc2-gop10-10s.mp4 -itsoffset 2 -i sine-8k-testsrc2-gop10-10s.mp4 -map 0:v:0 -map 1:a:0 -c:a pcm_alaw -vn -f rtp rtp://host.tms.asterisk:7078 -an -c:v copy -bsf: h264_mp4toannexb -f rtp rtp://host.tms.asterisk:7080

- 音频时间戳延后 1 秒

> ffmpeg -re -i sine-8k-testsrc2-gop10-10s.mp4 -c:a pcm_alaw -vn -output_ts_offset 1 -f rtp rtp://host.tms.asterisk:5006 -an -c:v copy -bsf: h264_mp4toannexb -f rtp rtp://host.tms.asterisk:5008

## ffprobe 命令

- 查看媒体文件基本信息

> ffprobe sine-red-10s.mp4

- 查看媒体文件的 stream 信息

> ffprobe -show_streams sine-8k-red-10s.mp4

- 查看媒体文件的 packet 并输出到文件

> ffprobe -show_packets color-red-10s.h264 > packets.txt

- 查看媒体文件的 frame 并输出到文件

> ffprobe -show_frames color-red-10s.h264 > frames.txt

# 应用说明

| 应用        | 说明                 | 代码           |
| ----------- | -------------------- | -------------- |
| TMSArgs     | 演示处理输入参数。   | app_tms_args.c |
| TMSAlawPlay | 播放 alaw 裸流文件。 | app_tms_alaw.c |
| TMSMp3Play  | 播放 mp3 文件。      | app_tms_mp3.c  |
| TMSH264Play | 播放 h264 裸流文件。 | app_tms_h264.c |
| TMSMp4Play  | 播放 mp4 文件。      | app_tms_mp4.c  |

| 参数     | 说明                                          | 必填 |
| -------- | --------------------------------------------- | ---- |
| filename | 要播放的文件                                  | 是   |
| repeat   | 重复播放的次数。小于等于 0 或不指定不重复。   | 否   |
| duration | 播放总时长，单位秒，支持小数                  | 否   |
| stop     | 停止播放的按键。支持指定多个按键，例如：0&9。 | 否   |
| pause    | 暂停播放的按键                                | 否   |
| resume   | 恢复播放的按键                                | 否   |
