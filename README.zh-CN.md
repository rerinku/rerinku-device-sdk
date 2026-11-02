# Rerinku Device SDK for C

[English](README.md) · 简体中文

把摄像头固件接入 [Rerinku View](https://github.com/rerinku/rerinku-view)(一个自托管的 NVR)。设备向 Rerinku View(下文称「平台」)注册,通过 MQTT 保持可接收命令的状态,并在有人观看或录像时,通过 WebRTC(WHIP)推送**硬件编码器的输出**。SDK 不负责采集和编码,编码好的帧由固件交给它。

- 纯 C(C11),只需包含一个头文件:`<rerinku/rerinku_device.h>`。
- 提供一个静态库(`librerinku_device.a`,所有依赖都在里面)和一个动态库(`librerinku_device.so`)。两者都只导出 `rerinku_*`,除 C 库外没有其他依赖。
- 可通过 CMake(`find_package(rerinku_device)`)和 pkg-config(`rerinku_device.pc`)找到。
- 用芯片厂商的工具链交叉编译;已在瑞芯微 RV1103 上测试(Luckfox Pico Plus:Cortex-A7、uClibc、GCC 8.3、64 MB 内存)。
- Apache-2.0 许可。所有依赖都是纯 C、版本锁定、从源码编译。

## 快速开始

**1. 编译并安装 SDK**(需要 CMake ≥ 3.21 和 C11 编译器;首次配置时会下载版本锁定的依赖):

```sh
git clone https://github.com/rerinku/rerinku-device-sdk.git
cd rerinku-device-sdk
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --component sdk --strip --prefix "$PWD/sdk"
```

**2. 集成。** 完整程序见 [`examples/quickstart/quickstart.c`](examples/quickstart/quickstart.c),核心部分如下:

```c
#include <rerinku/rerinku_device.h>

rerinku_device_config_t config;
rerinku_device_config_init(&config);            /* 默认值 + struct_size */
config.endpoint = "http://192.168.1.10:8080";   /* Rerinku View */
config.sn       = "CAM-0001";                   /* 这台设备的序列号 */

rerinku_device_callbacks_t callbacks = {
    .struct_size         = sizeof(callbacks),
    .on_status           = on_status,           /* 连接中 / 在线 / 离线 */
    .on_stream_start     = on_stream_start,     /* 有人观看:运行编码器 */
    .on_stream_stop      = on_stream_stop,      /* 没人观看:编码器可以空闲 */
    .on_keyframe_request = on_keyframe_request, /* 例如 RK_MPI_VENC_RequestIDR */
};

rerinku_device_t *device;
rerinku_err_t err = rerinku_device_create(&config, &callbacks, &device);
if (err != RERINKU_OK)
    fprintf(stderr, "%s\n", rerinku_strerror(err));

rerinku_device_channel_t video = {
    .struct_size = sizeof(video),
    .channel_id  = 0,
    .kind        = RERINKU_MEDIA_KIND_VIDEO,
    .codec       = RERINKU_CODEC_H264,
    .stream_id   = "main",
};
rerinku_device_add_channel(device, &video);
rerinku_device_start(device);                   /* 立即返回 */

/* 编码线程,每个编码好的访问单元调用一次: */
rerinku_device_write(device, 0, data, len, pts_us);

/* 关闭时: */
rerinku_device_stop(device);
rerinku_device_destroy(device);
```

**3. 链接。** 用 CMake:

```cmake
find_package(rerinku_device 0.5 CONFIG REQUIRED)  # -DCMAKE_PREFIX_PATH=<sdk>
target_link_libraries(camera PRIVATE rerinku::device)          # 静态库
# 或 rerinku::device_shared                                     # 动态库
```

用 pkg-config(适合 Makefile 构建、Buildroot、厂商 SDK):

```sh
export PKG_CONFIG_PATH=<sdk>/lib/pkgconfig
cc -std=c11 $(pkg-config --cflags rerinku_device) camera.c \
   "$(pkg-config --variable=libdir rerinku_device)/librerinku_device.a" -pthread   # 静态
cc -std=c11 $(pkg-config --cflags rerinku_device) camera.c \
   $(pkg-config --libs rerinku_device)                                              # 动态
```

`examples/quickstart/` 里两种构建方式都已备好(`CMakeLists.txt`、`Makefile`),并随 SDK 安装到 `share/rerinku_device/examples/`。

**4. 运行。** 让它连接平台。quickstart 用循环播放一段视频代替编码器:

```sh
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=25 -t 10 -c:v libx264 \
       -profile:v baseline -g 50 -bf 0 -x264-params aud=1 clip.h264
./quickstart http://192.168.1.10:8080 QUICKSTART-001 clip.h264
```

设备注册后就会出现在平台上,打开即可观看。功能更全的示例是 [`examples/device_example.c`](examples/device_example.c):H.264/H.265 文件加 G.711 音频,以及命令和快照请求。

## API 约定

API 遵循它所依赖的 C 库(mbedTLS、libjuice、libsrtp)的惯例:

- **命名。** 函数和类型以 `rerinku_` 开头,宏和枚举值以 `RERINKU_` 开头。类型以 `_t` 结尾,回调类型以 `_cb` 结尾。枚举值带上所属类型的名字(`rerinku_stream_state_t` → `RERINKU_STREAM_STATE_*`;唯一的例外是 `RERINKU_OK`)。全库函数是 `rerinku_*`;设备上的函数是 `rerinku_device_*`,第一个参数是设备。
- **错误。** 可能失败的函数返回 `rerinku_err_t`:`RERINKU_OK`(0)或负数的 `RERINKU_ERR_*`,可用 `rerinku_strerror()` 取描述。`rerinku_device_write()` 则返回 `rerinku_write_result_t`,因为它的大多数结果(没人观看、等待关键帧)都是正常情况。
- **对象。** `rerinku_device_t` 是不透明句柄:`create` / `destroy`,`start` / `stop`。
- **带版本的结构体。** 配置、通道和回调结构体都以 `struct_size` 开头。用旧头文件编译的程序传入较小的结构体,缺少的字段保持默认值;较旧的库会忽略后来新增的字段。
- **版本。** `<rerinku/rerinku_version.h>` 提供头文件的 `RERINKU_VERSION_MAJOR`、`_MINOR`、`_PATCH`、`_STRING` 和 `RERINKU_VERSION_NUMBER`(0xMMmmpp);`rerinku_version()` 和 `rerinku_version_number()` 报告实际链接的库的版本。项目遵循[语义化版本](https://semver.org/):0.x 阶段,次版本可能改变 API/ABI,soname 也会随之改变(`librerinku_device.so.0.5`);详见 [CHANGELOG.md](CHANGELOG.md)。
- **导出。** 导出的函数都标有 `RERINKU_API`,其他符号都隐藏。

## 使用 API

**生命周期。** `rerinku_device_start()` 立即返回;注册、MQTT 连接和故障恢复都在后台运行,并按退避策略无限重试(从 1 秒开始翻倍,最长 30 秒),所以摄像头比网络或服务器先启动也没关系,之后自然会上线。`on_status` 报告连接中 / 在线 / 离线。平台需要媒体时会发送 `push_sfu`:SDK 调用 `on_stream_start`,打开 WHIP 会话并请求关键帧;不再需要时调用 `on_stream_stop`。服务器断开媒体会话(重启、断网)时,SDK 会自动重建,不用应用层处理。平台在注册时签发的设备令牌会过期(Rerinku View:24 小时);SDK 在有效期过去四分之三时重新注册(按服务器时钟计算,所以设备时钟还停在 1970 年也没关系),WHIP 请求或快照上传被 401 拒绝时也会立即重新注册,然后重试该请求。

**线程。** 所有函数都可以从任意线程调用。`rerinku_device_write()` 只负责打包和排队,不碰网络;没人观看时它返回 `RERINKU_WRITE_RESULT_NOT_STREAMING`,几乎没有开销。回调运行在 SDK 的线程上(`on_keyframe_request` 和 `on_stream_state` 运行在媒体会话的线程上):回调要尽量短,并且不要在回调里调用 `rerinku_device_stop()` / `_destroy()`。SDK 自己运行三个线程(代理、命令处理、MQTT),推流时再加两个(ICE 和媒体会话)。

**命令和快照是异步的。** 用 `rerinku_device_reply_command()` / `_fail_command()` 响应 `on_command`,用 `rerinku_device_reply_snapshot()` 响应 `on_snapshot`。可以立即响应,也可以稍后从任意线程响应,硬件 JPEG 编码器什么时候出图都可以。

**事件不排队。** 设备离线时,`rerinku_device_send_event()` 返回 `RERINKU_ERR_NETWORK`。如果事件重要,请自行保存,等 `on_status` 报告在线后再发送一次。

### 送入帧数据

一次 `rerinku_device_write()` 调用传入一个完整的访问单元(一幅画面的所有 NAL 单元)或一帧音频,并带上以微秒为单位的采集时间(任意单调时间原点都可以,例如瑞芯微 MPI 的 `u64PTS` 可以直接用)。

H.264/H.265 通道默认使用 `RERINKU_BITSTREAM_FORMAT_AUTO`,它独立识别每个访问单元:三字节、四字节或混合起始码的 Annex-B,AVC/HEVC 长度前缀样本(1/2/4 字节长度),或单个裸 NAL。所有输入都会被校验并规范化为四字节 Annex-B(零拷贝快速路径)。SPS/PPS(以及 H.265 的 VPS)会被缓存,并在缺少它们的 IDR 之前补上,所以新的接收端总能拿到可解码的随机访问点。无效输入返回 `RERINKU_WRITE_RESULT_INVALID_FRAME`,不会进入 RTP。

支持的编码:H.264、H.265(不支持 H.265 的浏览器无法播放,面向网页观众时请推 H.264)、Opus、G.711 µ-law/A-law。

## 内存和体积

测试环境:RV1103(Cortex-A7 1.2 GHz),`MinSizeRel`,640×360 H.264 约 0.8 Mbit/s 加 G.711A(已扣除示例自带的 1 MB 测试视频):

| | |
|---|---|
| `librerinku_device.so`,去符号 | 0.39 MB(只依赖 C 库) |
| 示例程序,静态链接,去符号 | 0.39 MB |
| RSS 空闲(已注册,MQTT 在线) | 约 0.8 MB |
| RSS 推流 | 约 1.8 MB |
| CPU 推流 | 约占单核 6% |
| 线程 | 空闲 3 个,推流 5 个 |

推流结束后,分配器会保留已释放的内存供下次使用(RSS 维持在接近推流时的水平);这些内存会被复用,不是泄漏。

`rerinku_device_config_t` 里可调的项:

- `gop_cache_max_bytes`(默认每个视频通道 1 MiB):SDK 缓存当前 GOP,让新观众能立即出画。小内存设备请设为 0,并实现 `on_keyframe_request`,由编码器的 IDR 开始出画。
- `nack_history_packets`(默认每个视频通道 512 个,每个约 1.2 KB):保留已发送的包,用于响应重传请求。64 MB 的设备适合 128 到 256。

`librerinku_device.so` 的体积构成(RV1103):mbedTLS 约 160 KB,SDK 约 80 KB,libjuice 约 60 KB,libsrtp 约 25 KB,coreMQTT 和 cJSON 约 10 KB。mbedTLS 按 `cmake/mbedtls_config.h` 构建,只启用 SDK 用到的功能:带 ECDHE + AES-GCM 的 TLS/DTLS 1.2、ECDSA P-256/P-384 和 RSA 证书校验、DTLS-SRTP 扩展。没有 TLS 1.3,也没有错误字符串表(错误打印为 `mbedTLS -0x....`)。

## 构建

需要 CMake ≥ 3.21 和 C11 编译器。首次配置时会下载版本锁定的依赖。仅支持 Linux 和其他 POSIX 系统(pthreads、BSD sockets)。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --component sdk --strip --prefix <dir>
```

`sdk` 组件就是交付物:

```
include/rerinku/rerinku_device.h       API
include/rerinku/rerinku_version.h      版本宏(生成)
lib/librerinku_device.a                SDK 和所有依赖,合成一个静态库
lib/librerinku_device.so*              同上,动态库(soname librerinku_device.so.0.5)
lib/cmake/rerinku_device/              find_package(rerinku_device) → rerinku::device[_shared]
lib/pkgconfig/rerinku_device.pc        pkg-config rerinku_device(可重定位)
share/rerinku_device/examples/         quickstart/、device_example.c
share/doc/rerinku_device/              README、CHANGELOG、LICENSE、NOTICE
```

`librerinku_device.a` 把 SDK 和所有依赖合并成一个目标文件,全局符号只有 `rerinku_*`,所以固件只需链接一个文件,固件自己的 mbedTLS、cJSON 等也不会和 SDK 冲突。在 CMake 项目里也可以直接使用源码树:`add_subdirectory(rerinku-device-sdk)`,然后链接 `rerinku::device`。

### 交叉编译

给 CMake 传入目标平台的工具链文件。仓库里带了一份瑞芯微 RV1103/RV1106 的(厂商工具链 `arm-rockchip830-linux-uclibcgnueabihf`):

```sh
cmake -S . -B build-rv1106 -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rv1106-uclibc.cmake \
      -DRV1106_TOOLCHAIN_DIR=<sdk>/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf
cmake --build build-rv1106 -j
```

其他芯片:复制这个文件,修改编译器前缀和 CPU 参数。库必须用固件的工具链编译(C 库和浮点 ABI 必须一致)。Luckfox Pico 开发板可以直接用 [rerinku-luckfox-pico](https://github.com/rerinku/rerinku-luckfox-pico),它把这些步骤封装在厂商的官方编译容器里。

### Android

源码树不用改动就能用 NDK 编译:在 AGP 的 CMake 里 `add_subdirectory()` 引入(NDK 27、API 26+、16 KB 页面、不需要 C++ 运行时),再用一个小的 JNI 层封装 C API。可以直接送入 MediaCodec 的输出,AUTO 码流格式能直接接受 Android 的 Annex-B 访问单元。Android 的信任证书存放在 keystore 里,所以使用 `https://` / `mqtts://` 时,需要把系统根证书导出成 PEM 文件,并作为 `ca_file` 传入。

### TLS

`https://` 地址和 `mqtts://` broker 使用 mbedTLS,并校验服务器证书和主机名。校验依据是 `ca_file`(PEM),或找到的第一个系统证书包(`/etc/ssl/certs/ca-certificates.crt` 等)。只放你需要的根证书,可以省下完整证书包占用的内存。证书有效期校验需要正确的时钟(NTP)。`tls_insecure = 1` 会关闭校验,仅用于开发。

## 工作原理

1. **注册** — 向 `POST <endpoint>/` 发送 `X-Rerinku: true` 头和 `{sn, name, deviceId, model, fwVersion, secret}`;平台返回 WHIP 地址、MQTT broker、ICE 服务器和设备令牌。平台强制校验设备密钥时,`secret = base64(HMAC-SHA256(设备密钥, sn))`。
2. **控制** — MQTT 3.1.1(coreMQTT),主题是 `rerinku/d/<sn>/{cmd,status,ack,data}`:接收命令、保留状态和离线遗嘱、命令确认、设备事件。设备登录时用户名是 `<sn>`,密码是设备令牌,客户端 ID 是 `rerinku-dev-<sn>`;Rerinku View 的 broker 不接受其他连接,并且设备只能使用自己的四个主题(命令只能来自平台)。登录被拒绝通常是因为令牌在设备离线期间过期了:SDK 会重新注册,并用新令牌重连(最多每分钟一次)。每次连接都会发送状态,其中带有设备的静态描述,平台会保存这些信息:名称、型号、固件、通道目录和 `ext_json`(自由格式的信息,例如 SoC、CPU、内存或传感器)。
3. **媒体** — 收到 `push_sfu` 后,用一次 WHIP POST 发送 BUNDLE、rtcp-mux、只发送的 offer,带上所有 ICE 候选;平台的 SFU 应答。ICE 由 libjuice 完成,DTLS-SRTP 密钥协商由 mbedTLS 完成(支持任一 DTLS 角色,通过 SDP 指纹认证对端),SRTP 由 libsrtp 完成。RTP:H.264(RFC 6184 单 NAL / FU-A)、H.265(RFC 7798 单 NAL / FU)、G.711、Opus。视频按节奏发送(上限 48 Mbit/s,只平滑关键帧突发);NACK 从历史缓存响应,PLI/FIR 转成 `on_keyframe_request`,每秒发送一次发送端报告。`stop` 关闭会话(WHIP DELETE)。

   **多路码流,按需发送。** `stream_id` 不同的通道(摄像头的 `main` 和 `sub` 码流)都在同一个会话里声明,但只有平台当前需要的码流才会传输媒体。`push_sfu` 会列出它们,例如 `{"streams": ["main"], "rev": …}`,这是完整集合,不是增量。会话进行中收到 `push_sfu` 时,只开关通道,不重新协商:开始发送的通道从关键帧开始(为它触发 `on_keyframe_request`),写入无人需要的通道的帧返回 `RERINKU_WRITE_RESULT_NOT_STREAMING`,针对它的关键帧请求会被丢弃。`rev` 随每条命令递增,比最后应用的值更旧的命令会被忽略。没有 `streams`(较旧的平台),或者其中没有这台设备的任何码流时,发送所有通道。推流时音频通道总是发送,因为摄像头的音频服务于它的所有码流。状态里会列出正在发送的码流(`"streams"`)。
4. **快照** — `get_snapshot` → `on_snapshot` → 带设备令牌把 JPEG `POST` 到 `/api/v1/devices/<sn>/snapshot`。
5. **离线录像** — `rerinku_device_upload_recording()` 把设备在连不上平台期间录下的 MP4(H.264/H.265,可选 AAC)以流式方式发送到 `POST /api/v1/devices/<sn>/recordings?start=<ms>&stream=<id>`,带设备令牌;Rerinku View 会按录制时间把它归入录像。这个调用是阻塞的,请在在线时从自己的线程调用。
6. **运行状态** — `get_runtime` → `on_runtime_info`:温度、CPU 负载、内存以及设备测量的其他信息,按需响应。有人打开设备时,平台每隔几秒询问一次;其他时候不发送。

内置命令:`push_sfu`、`stop`、`get_runtime`、`get_snapshot`。其他命令都交给 `on_command`。

### 设备信息

分两类,走两条路径:

| | 静态(`ext_json`,配置) | 实时(`on_runtime_info`,回调) |
|---|---|---|
| 何时发送 | 随状态发送,每次连接时 | 平台询问时(`get_runtime`) |
| 是否保存 | 是,和设备一起保存 | 否,收到后直接显示 |
| 平台有标签的键 | `manufacturer`、`soc`、`cpu`、`cpuCores`、`cpuMHz`、`memoryKB`、`sensor`、`os`、`mac` | `tempC`、`cpuPct`、`memTotalKB`、`memAvailableKB`、`uptimeSec`、`load1` |

两者都是设备自定的 JSON 对象;平台不认识的键会按原名列出,所以设备可以加上风扇转速、电池电量或 Wi-Fi 信号,不用改平台。

## 第三方组件

| 组件 | 版本 | 用途 | 许可证 |
|---|---|---|---|
| [libjuice](https://github.com/paullouisageneau/libjuice) | 1.6.2 | ICE(STUN/TURN) | MPL-2.0 |
| [libsrtp](https://github.com/cisco/libsrtp) | 2.7.0 | SRTP | BSD-3-Clause |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | 3.6.2 | DTLS-SRTP、HTTPS、MQTTS | Apache-2.0 |
| [coreMQTT](https://github.com/FreeRTOS/coreMQTT) | 2.3.1 | MQTT 客户端 | MIT |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.18 | JSON | MIT |

## 已知限制

- 只支持推流(WHIP);不支持播放、trickle ICE 和数据通道。
- H.265 只能在支持它的接收端播放(Safari、部分平台上较新的 Chrome)。
- 只支持 POSIX;没有 Windows 版本。

## 许可证

Apache License 2.0,见 [LICENSE](LICENSE)。第三方组件遵循各自的许可证,见 [NOTICE](NOTICE)。libjuice 是 MPL-2.0:分发包含它的二进制文件时,需要提供它的源码(这里没有修改过)。
