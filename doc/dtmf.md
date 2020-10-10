> Asterisk 现有版本不支持播放视频文件（支持视频通话），无法满足发送视频通知、视频 IVR 等场景。本系列文章，通过学习音视频的相关知识和工具，尝试实现一个通过 Asterisk 播放 mp4 视频文件的应用。

- [Asterisk 播放 mp4（1）——音频和 PCM 编码](https://www.jianshu.com/p/760f8557d308)
- [Asterisk 播放 mp4（2）——音频封装](https://www.jianshu.com/p/dcfc6608488b)
- [Asterisk 播放 mp4（3）——搭建开发环境](https://www.jianshu.com/p/de070b95f748)
- [Asterisk 播放 mp4（4）——H264&AAC](https://www.jianshu.com/p/5d01a60d2b6c)
- [Asterisk 播放 mp4（5）——MP4 文件解析](https://www.jianshu.com/p/2c85abb4cf23)
- [Asterisk 播放 mp4（6）——音视频同步](https://www.jianshu.com/p/6b8c318e6ef1)
- [Asterisk 播放 mp4（7）——DTMF](https://www.jianshu.com/p/bed78861efac)

通过 sip 终端控制 asterisk 播放视频，例如：暂停，恢复，切换等，最直接方式就是让用户通过终端的按键进行控制，因此我们研究一下 DTMF 和 Asterisk 处理 DTMF 的方式。

# DTMF（双音多频）

双音多频（DTMF，Dual Tone Multi Frequency），由高频群和低频群组成，高低频群各包含 4 个频率。一个高频信号和一个低频信号叠加组成一个组合信号，代表一个符号。DTMF 信号有 16 个编码。选择双音方式是由于它能够可靠地将拨号信息从语音中区分出来。CCITT 规定每秒最多按 10 个键，即每个键时隙最短为 100 毫秒，其中音频实际持续时间至少为 45 毫秒，不大于 55 毫秒，时隙的其他时间内保持静默。因此生成 DTMF 流程包含音频任务和静默任务，前者是产生双音频采样值，后者产生静默样值，每个任务结束时，要重置定时器和下一个任务。

发送 DTMF 有三种方式：1，RFC4733/RFC2833；2，SIPINFO；3，INBAND。

1. RFC4733/RFC2833
   为带内检测方式，通过 RTP 传输，由特殊的 rtp PayloadType 即`TeleponeEvent`来标示 RFC2833 数据包。同一个 DTMF 按键通常会对应多个 RTP 包，这些 RTP 数据包的时间戳均相同，此可以作为识别同一个按键的判断依据，最后一组 RTP 数据包的 end 标志置 1 表示 DTMF 数据结束。RFC4733 用于替代 RFC2833。

![telephone-event RTP包定义](https://upload-images.jianshu.io/upload_images/258497-d11405936a472e82.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

| 字段     | 说明                                                        |
| -------- | ----------------------------------------------------------- |
| event    | 按键事件对应的值，例如：`1`对应 1，`*`对应 10，`#`对应 11。 |
| E        | 事件是否结束标志，1 代表结束。                              |
| R        | 保留位，必须为 0。                                          |
| volumn   | 音量。                                                      |
| duration | 事件持续时间，时间戳单位，网络字节序。                      |

2. SIPINFO(RFC2976)
   为带外检测方式，通过 SIP 信令通道传输 DTMF 数据。INFO 消息通常用于传递应用层消息，INFO 方法并不是用于改变 SIP 呼叫的状态，也不是用于改变被 SIP 初始化地会议状态（和 re-invite 等的区别）。接收端接收 INFO 成功后必须返回`200 OK`。规范中并不包括消息体的定义，DTMF 数据由`Signal`和`Duration`两个字端组成，各占 1 行。注意当 DTMF 为“_”时不同的标准实现对应的`Signal=_`或`Signal=10`。SIPINFO 的好处是不影响 RTP 数据包的传输，但可能会造成不同步。

3. INBAND
   为带内检测方式，而且与普通的 RTP 语音包混在一起传送。在进行 INBAND DTMF 检测时唯一的办法就是提取 RTP 数据包进行频谱分析，经过频谱分析得到高频和低频的频率，然后查表得到对应的按键。这种存在一个问题，如果采用压缩比高的音频编码，可能导致无法识别出按键。

通过 RTP 或 SIP INFO 传递 DTMF 时，传递的值不是原始的音频数据，而是经过编码的。

![DTMF按键编码](https://upload-images.jianshu.io/upload_images/258497-00af5af9aa63404d.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

# Asterisk 处理 DTMF

通过`pjsip.conf`中的`dtmf_mode`可以设置 Asterisk 发送 DTMF 方式，文档中说明支持上面提到的 3 种方式，应该也支持接收这 3 种方式发送的数据（后面会验证前两种）。

在 Asterisk 应用中，Asterisk 将 DTMF 和媒体数据都作为`ast_frame`，不论发送发采用何种方式，都可以通过`ast_channel`读取，基本代码如下：

```
struct ast_frame *f = ast_read(chan);
if ((f->frametype == AST_FRAME_DTMF) && (f->subclass.integer == '#')) {
  // 收到按键后的处理
}
```

需要注意的是`subclass.integer`是 DTMF 字符对应的`ASCII`码值，不是 RFC2833 中定义的数值。

利用`dialplan`中的变量，可以将 DTMF 的识别结果反馈到`dialplan`中，实现根据按键执行不同的呼叫流程。在应用中设置`dialplan`变量的代码如下：

```
pbx_builtin_setvar_helper(chan, "TMSDTMFKEY", key);
```

除了在应用中获得 DTMF，Asterisk 的`AMI`和`ARI`接口也都提供了接收 DTMF 事件和发送 DTMF 按键。

# 样本数据

`linphone`可以设置 DTMF 的发送方式，这样就可以通过`linphone`获取不同方式下的样本数据。

![linphone设置DTMF发送方式](https://upload-images.jianshu.io/upload_images/258497-2a386a0e04f31cd4.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

## RFC4833/RFC2833

![SDP中指定DTMF媒体参数](https://upload-images.jianshu.io/upload_images/258497-802eedd2a5b04aed.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

![按键1对应的RTP包](https://upload-images.jianshu.io/upload_images/258497-b7214e6a3700b671.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

一个按键产生了多个 RTP 包，标记事件结束的包也会有多个，按键包之间还包括了声音包。

![开始按键的RTP包](https://upload-images.jianshu.io/upload_images/258497-71d1f39a77df2bc0.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

第一个字节`01`对应按键`1`；第 2 个字节`0a`，第 1 位`0`说明事件为未结束，第 2 位保留，后 6 位是音量，值为 10；第 3 和第 4 字节`00 a0`，持续时间，值为 160。

![结束按键的RTP包](https://upload-images.jianshu.io/upload_images/258497-651eddabb2a49415.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

## SIPINFO

![SIP INFO 消息 DTMF数字](https://upload-images.jianshu.io/upload_images/258497-d89692d51eb2b7d8.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

![STP INFO 消息 DTMF字符](https://upload-images.jianshu.io/upload_images/258497-cca53352414d31a2.png?imageMogr2/auto-orient/strip%7CimageView2/2/w/1240)

# 参考

[RFC4733 - RTP Payload for DTMF Digits, Telephony Tones and Telephony Signals](https://www.ietf.org/rfc/rfc4733.txt)

[RFC2833 - RTP Payload for DTMF Digits, Telephony Tones and Telephony Signals](https://www.ietf.org/rfc/rfc2833.txt)

[RFC2976 - The SIP INFO Method](https://tools.ietf.org/html/rfc2976)

[pjsip.conf dtmf_mode](https://wiki.asterisk.org/wiki/display/AST/Asterisk+12+Configuration_res_pjsip#Asterisk12Configuration_res_pjsip-endpoint_dtmf_mode)

[Asterisk Dialplan 变量](https://wiki.asterisk.org/wiki/display/AST/Variables)

[Asterisk 13 ManagerEvent_DTMFBegin](https://wiki.asterisk.org/wiki/display/AST/Asterisk+13+ManagerEvent_DTMFBegin)

[Asterisk 13 ManagerEvent_DTMFEnd](https://wiki.asterisk.org/wiki/display/AST/Asterisk+13+ManagerEvent_DTMFEnd)

[Asterisk 通过 ARI 接口处理 DTMF 示例](https://github.com/asterisk/node-ari-client/blob/master/examples/playback.js)
