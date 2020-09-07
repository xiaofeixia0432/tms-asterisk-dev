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

| 号码 | 功能               | 代码           |
| ---- | ------------------ | -------------- |
| 1001 | 播放 alaw 格式文件 | app_tms_alaw.c |
| 2001 | 播放 mp3 格式文件  | app_tms_mp3.c  |
| 3001 | 播放 h264 格式文件 | app_tms_h264.c |
