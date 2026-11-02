# Rerinku Device SDK for C

English · [简体中文](README.zh-CN.md)

Connects a camera's firmware to [Rerinku View](https://github.com/rerinku/rerinku-view),
a self-hosted NVR. The device registers with Rerinku View (the "platform" below),
stays reachable for commands over MQTT, and pushes its **hardware encoder's
output** over WebRTC (WHIP) whenever someone watches or records. The SDK does not
capture or encode; the firmware hands it encoded frames.

- Plain C (C11), one header to include: `<rerinku/rerinku_device.h>`.
- Ships as one static archive (`librerinku_device.a`, every dependency inside)
  and one shared library (`librerinku_device.so`); both export only `rerinku_*`
  and need nothing but the C library.
- Found by CMake (`find_package(rerinku_device)`) and pkg-config
  (`rerinku_device.pc`).
- Cross-compiles with the chip vendor's toolchain; tested on Rockchip RV1103
  (Luckfox Pico Plus: Cortex-A7, uClibc, GCC 8.3, 64 MB RAM).
- Apache-2.0. Every dependency is plain C, pinned, and built from source.

## Quick start

**1. Build and install the SDK** (CMake ≥ 3.21, a C11 compiler; the first
configure downloads the pinned dependencies):

```sh
git clone https://github.com/rerinku/rerinku-device-sdk.git
cd rerinku-device-sdk
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --component sdk --strip --prefix "$PWD/sdk"
```

**2. Integrate.** The whole program is
[`examples/quickstart/quickstart.c`](examples/quickstart/quickstart.c); its core:

```c
#include <rerinku/rerinku_device.h>

rerinku_device_config_t config;
rerinku_device_config_init(&config);            /* defaults + struct_size */
config.endpoint = "http://192.168.1.10:8080";   /* Rerinku View */
config.sn       = "CAM-0001";                   /* this device's serial number */

rerinku_device_callbacks_t callbacks = {
    .struct_size         = sizeof(callbacks),
    .on_status           = on_status,           /* connecting / online / offline */
    .on_stream_start     = on_stream_start,     /* someone watches: run the encoder */
    .on_stream_stop      = on_stream_stop,      /* nobody does: it may idle */
    .on_keyframe_request = on_keyframe_request, /* e.g. RK_MPI_VENC_RequestIDR */
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
rerinku_device_start(device);                   /* returns at once */

/* encoder thread, for every encoded access unit: */
rerinku_device_write(device, 0, data, len, pts_us);

/* shutting down: */
rerinku_device_stop(device);
rerinku_device_destroy(device);
```

**3. Link it.** With CMake:

```cmake
find_package(rerinku_device 0.5 CONFIG REQUIRED)  # -DCMAKE_PREFIX_PATH=<sdk>
target_link_libraries(camera PRIVATE rerinku::device)          # static archive
# or rerinku::device_shared                                     # the .so
```

With pkg-config (Makefile builds, Buildroot, vendor SDKs):

```sh
export PKG_CONFIG_PATH=<sdk>/lib/pkgconfig
cc -std=c11 $(pkg-config --cflags rerinku_device) camera.c \
   "$(pkg-config --variable=libdir rerinku_device)/librerinku_device.a" -pthread   # static
cc -std=c11 $(pkg-config --cflags rerinku_device) camera.c \
   $(pkg-config --libs rerinku_device)                                              # shared
```

`examples/quickstart/` has both builds ready (`CMakeLists.txt`, `Makefile`), and
is installed with the SDK under `share/rerinku_device/examples/`.

**4. Run it** against a platform. The quick start replays a clip in place of an
encoder:

```sh
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=25 -t 10 -c:v libx264 \
       -profile:v baseline -g 50 -bf 0 -x264-params aud=1 clip.h264
./quickstart http://192.168.1.10:8080 QUICKSTART-001 clip.h264
```

The device shows up on the platform as soon as it registers; open it to watch.
A fuller program is [`examples/device_example.c`](examples/device_example.c): an
H.264/H.265 file plus G.711 audio, commands and snapshot requests.

## API conventions

The API follows the conventions of the C libraries it is built on (mbedTLS,
libjuice, libsrtp):

- **Names.** Functions and types start with `rerinku_`, macros and enum values
  with `RERINKU_`. Types end in `_t`, callback types in `_cb`. Enum values
  carry their type's name (`rerinku_stream_state_t` → `RERINKU_STREAM_STATE_*`;
  the one exception is `RERINKU_OK`). Library-wide functions are `rerinku_*`;
  functions on a device are `rerinku_device_*` and take the device first.
- **Errors.** Functions that can fail return `rerinku_err_t`: `RERINKU_OK` (0)
  or a negative `RERINKU_ERR_*`; `rerinku_strerror()` describes it.
  `rerinku_device_write()` returns a `rerinku_write_result_t` instead, since
  most of its outcomes (nobody watching, waiting for a keyframe) are normal.
- **Objects.** `rerinku_device_t` is an opaque handle: `create` / `destroy`,
  `start` / `stop`.
- **Versioned structs.** Config, channel and callback structs start with
  `struct_size`. A program built against an older header passes a smaller
  struct and the missing fields keep their defaults; fields added later are
  ignored by an older library.
- **Versions.** `<rerinku/rerinku_version.h>` has `RERINKU_VERSION_MAJOR`,
  `_MINOR`, `_PATCH`, `_STRING` and `RERINKU_VERSION_NUMBER` (0xMMmmpp) for the
  headers; `rerinku_version()` and `rerinku_version_number()` report the
  library actually linked. The project follows [Semantic Versioning](https://semver.org/):
  while 0.x, a minor release may change the API/ABI and the soname changes with
  it (`librerinku_device.so.0.5`); see [CHANGELOG.md](CHANGELOG.md).
- **Exports.** Exported functions are marked `RERINKU_API`; everything else is
  hidden.

## Using the API

**Lifecycle.** `rerinku_device_start()` returns immediately; registration,
the MQTT connection and recovery run in the background and retry forever with
backoff (1 s doubling to 30 s), so a camera that boots before the network or
the server is up simply comes online later. `on_status` reports
connecting / online / offline. When the platform needs media it sends
`push_sfu`: the SDK calls `on_stream_start`, opens the WHIP session and asks for
a keyframe; when nobody needs it any more, `on_stream_stop`. A media session the
server drops (restart, network loss) is rebuilt without bothering the
application. The device token the platform issues at registration expires
(Rerinku View: 24 h); the SDK registers again after three quarters of its lifetime
— timed against the server's clock, so a device clock still at 1970 does not
matter — and at once whenever a WHIP request or snapshot upload is refused
with 401, then retries that request.

**Threading.** Every function may be called from any thread.
`rerinku_device_write()` only packetizes and queues — it never touches the
network — and while nobody is watching it returns `RERINKU_WRITE_RESULT_NOT_STREAMING`
almost for free. Callbacks run on SDK threads (`on_keyframe_request` and
`on_stream_state` on the media session's thread): keep them short, and never
call `rerinku_device_stop()` / `_destroy()` from one. The SDK runs three threads
of its own (agent, command worker, MQTT) plus, while streaming, one for ICE and
one for the media session.

**Commands and snapshots are asynchronous.** Answer `on_command` with
`rerinku_device_reply_command()` / `_fail_command()` and `on_snapshot` with
`rerinku_device_reply_snapshot()`, immediately or later from any thread — a
hardware JPEG encoder can deliver whenever it is ready.

**Events are not queued.** `rerinku_device_send_event()` returns
`RERINKU_ERR_NETWORK` while the device is offline; keep the event and send it
again once `on_status` reports online if it matters.

### Feeding frames

One `rerinku_device_write()` call carries one complete access unit (all NAL
units of a picture) or one audio frame, with its capture time in microseconds
(any monotonic origin, e.g. Rockchip MPI's `u64PTS` as is).

H.264/H.265 channels default to `RERINKU_BITSTREAM_FORMAT_AUTO`, which recognizes each
access unit independently: Annex-B with three-byte, four-byte or mixed start
codes, AVC/HEVC length-prefixed samples (1/2/4-byte lengths), or a single raw
NAL. Everything is validated and normalized to four-byte Annex-B (the zero-copy
fast path). SPS/PPS (and H.265 VPS) are cached and restored before an IDR that
omits them, so a fresh receiver always gets a decodable random access point.
Invalid input returns `RERINKU_WRITE_RESULT_INVALID_FRAME` and never reaches RTP.

Codecs: H.264, H.265 (browsers without H.265 support cannot play it; push H.264
for web viewers), Opus, G.711 µ-law/A-law.

## Memory and footprint

Measured on RV1103 (Cortex-A7 1.2 GHz), `MinSizeRel`, 640×360 H.264 at
~0.8 Mbit/s plus G.711A (the example's own 1 MB test clip subtracted):

| | |
|---|---|
| `librerinku_device.so`, stripped | 0.39 MB (needs only the C library) |
| example program, statically linked, stripped | 0.39 MB |
| RSS idle (registered, MQTT online) | ~0.8 MB |
| RSS streaming | ~1.8 MB |
| CPU streaming | ~6 % of one core |
| threads | 3 idle, 5 streaming |

After a stream the allocator keeps the freed memory for the next one (RSS stays
near the streaming figure); it is reused, not leaked.

Knobs in `rerinku_device_config_t`:

- `gop_cache_max_bytes` (default 1 MiB per video channel): the SDK keeps the
  current GOP so a new viewer starts at once. Set 0 on small-memory devices and
  implement `on_keyframe_request` instead; the encoder's IDR then starts the
  picture.
- `nack_history_packets` (default 512 per video channel, ~1.2 KB each): sent
  packets kept to answer retransmission requests. 128–256 suits 64 MB devices.

Where the bytes go in `librerinku_device.so` (RV1103): mbedTLS ~160 KB, the SDK
~80 KB, libjuice ~60 KB, libsrtp ~25 KB, coreMQTT and cJSON ~10 KB. mbedTLS is
built from `cmake/mbedtls_config.h`, which enables only what the SDK uses:
TLS/DTLS 1.2 with ECDHE + AES-GCM, ECDSA P-256/P-384 and RSA certificate
verification, the DTLS-SRTP extension. No TLS 1.3, no error string table
(errors print as `mbedTLS -0x....`).

## Build

CMake ≥ 3.21 and a C11 compiler. The first configure downloads the pinned
dependencies. Linux and other POSIX systems only (pthreads, BSD sockets).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --component sdk --strip --prefix <dir>
```

The `sdk` component is the deliverable:

```
include/rerinku/rerinku_device.h       the API
include/rerinku/rerinku_version.h      version macros (generated)
lib/librerinku_device.a                the SDK and every dependency, one archive
lib/librerinku_device.so*              the same, shared (soname librerinku_device.so.0.5)
lib/cmake/rerinku_device/              find_package(rerinku_device) → rerinku::device[_shared]
lib/pkgconfig/rerinku_device.pc        pkg-config rerinku_device (relocatable)
share/rerinku_device/examples/         quickstart/, device_example.c
share/doc/rerinku_device/              README, CHANGELOG, LICENSE, NOTICE
```

`librerinku_device.a` is the SDK and all its dependencies merged into one object
whose only global symbols are `rerinku_*`, so the firmware links one file and
its own mbedTLS/cJSON/... cannot clash with the SDK's. Inside a CMake project
the tree can also be used directly: `add_subdirectory(rerinku-device-sdk)` and link
`rerinku::device`.

### Cross-compiling

Pass a CMake toolchain file for the target. One is included for Rockchip
RV1103/RV1106 (vendor toolchain `arm-rockchip830-linux-uclibcgnueabihf`):

```sh
cmake -S . -B build-rv1106 -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rv1106-uclibc.cmake \
      -DRV1106_TOOLCHAIN_DIR=<sdk>/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf
cmake --build build-rv1106 -j
```

Other chips: copy that file and change the compiler prefix and CPU flags. The
library must be built with the firmware's toolchain (C library and float ABI
must match). For Luckfox Pico boards,
[rerinku-luckfox-pico](https://github.com/rerinku/rerinku-luckfox-pico) wraps this in the vendor's
official build container.

### Android

The tree builds unchanged with the NDK: `add_subdirectory()` it under AGP's CMake
(NDK 27, API 26+, 16 KB pages, no C++ runtime) and wrap the C API in a small JNI
layer. Feed it MediaCodec output directly — the AUTO bitstream format takes
Android's Annex-B access units as they are. Android keeps its trust store in a
keystore, so for `https://` / `mqtts://` export the system roots to a PEM file and
pass it as `ca_file`.

### TLS

`https://` endpoints and `mqtts://` brokers use mbedTLS and verify the server
certificate and host name, against `ca_file` (PEM) or the first system bundle
found (`/etc/ssl/certs/ca-certificates.crt`, ...). A file with only the roots
you need saves the RAM a full bundle costs. Certificate validity needs a
correct clock (NTP). `tls_insecure = 1` disables verification for development.

## How it works

1. **Register** — `POST <endpoint>/` with `X-Rerinku: true` and
   `{sn, name, deviceId, model, fwVersion, secret}`; the platform answers with
   the WHIP URL, MQTT broker, ICE servers and a device token. When the platform
   enforces device keys, `secret = base64(HMAC-SHA256(device key, sn))`.
2. **Control** — MQTT 3.1.1 (coreMQTT), topics `rerinku/d/<sn>/{cmd,status,ack,data}`:
   commands in, retained status and Last Will offline, command acks, device
   events. The device logs in with username `<sn>`, password = its device
   token and client id `rerinku-dev-<sn>`; Rerinku View's broker admits nobody
   else and lets a device use only its own four topics (commands come from the
   platform alone). A refused login usually means the token expired while the
   device was offline: the SDK registers again and reconnects with the new
   one (at most once a minute). The status goes out on every connect and carries the device's
   static description — name, model, firmware, the channel catalog and
   `ext_json`, free-form facts such as SoC, CPU, memory or sensor — which the
   platform stores.
3. **Media** — on `push_sfu`, one WHIP POST carries a BUNDLEd, rtcp-muxed,
   send-only offer with every ICE candidate; the platform's SFU answers. ICE by
   libjuice, DTLS-SRTP keying by mbedTLS (either DTLS role, peer authenticated
   by the SDP fingerprint), SRTP by libsrtp. RTP: H.264 (RFC 6184 single NAL /
   FU-A), H.265 (RFC 7798 single NAL / FU), G.711, Opus. Video is paced (48 Mbit/s
   ceiling, shaping keyframe bursts only); NACKs are answered from the history,
   PLI/FIR become `on_keyframe_request`, sender reports go out every second.
   `stop` tears the session down (WHIP DELETE).

   **Several streams, sent on demand.** Channels with different `stream_id`s
   (a camera's `main` and `sub` stream) are all declared in the one session,
   but only the streams the platform currently needs carry media. `push_sfu`
   names them — `{"streams": ["main"], "rev": …}`, the complete set, not a
   change — and a `push_sfu` arriving while the session is up only switches
   channels on or off, without renegotiating: a channel that starts sending
   begins at a keyframe (`on_keyframe_request` fires for it), frames written
   to a channel nobody wants return `RERINKU_WRITE_RESULT_NOT_STREAMING`, and
   keyframe requests for it are dropped. `rev` grows with every command; one
   older than the last applied is ignored. Without `streams` (an older
   platform), or when it names none of the device's streams, every channel
   is sent. Audio channels are always sent while streaming: a camera's audio
   serves all its streams. The status lists the streams being sent
   (`"streams"`).
4. **Snapshot** — `get_snapshot` → `on_snapshot` → JPEG `POST` to
   `/api/v1/devices/<sn>/snapshot` with the device token.

5. **Recordings made offline** — `rerinku_device_upload_recording()` streams an
   MP4 (H.264/H.265, optional AAC) the device recorded while it could not reach
   the platform to `POST /api/v1/devices/<sn>/recordings?start=<ms>&stream=<id>`
   with the device token; Rerinku View files it in its recordings at the time it
   was recorded. Blocking; call it from your own thread while online.

6. **Runtime state** — `get_runtime` → `on_runtime_info`: temperature, CPU
   load, memory and whatever else the device measures, answered on demand.
   The platform asks every few seconds while someone has the device open;
   nothing is sent otherwise.

Built-in commands: `push_sfu`, `stop`, `get_runtime`, `get_snapshot`.
Everything else goes to `on_command`.

### Device information

Two kinds, two paths:

| | static (`ext_json`, config) | live (`on_runtime_info`, callback) |
|---|---|---|
| when | with the status, on every connect | when the platform asks (`get_runtime`) |
| stored | yes, with the device | no, shown as received |
| keys the platform labels | `manufacturer`, `soc`, `cpu`, `cpuCores`, `cpuMHz`, `memoryKB`, `sensor`, `os`, `mac` | `tempC`, `cpuPct`, `memTotalKB`, `memAvailableKB`, `uptimeSec`, `load1` |

Both are JSON objects of the device's choosing; keys the platform does not
know are listed under their own names, so a device can add fan speed, battery
level or Wi-Fi signal without a platform change.

## Third-party components

| Component | Version | Use | License |
|---|---|---|---|
| [libjuice](https://github.com/paullouisageneau/libjuice) | 1.6.2 | ICE (STUN/TURN) | MPL-2.0 |
| [libsrtp](https://github.com/cisco/libsrtp) | 2.7.0 | SRTP | BSD-3-Clause |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | 3.6.2 | DTLS-SRTP, HTTPS, MQTTS | Apache-2.0 |
| [coreMQTT](https://github.com/FreeRTOS/coreMQTT) | 2.3.1 | MQTT client | MIT |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.18 | JSON | MIT |

## Known limits

- Push only (WHIP); no playback, no trickle ICE, no data channels.
- H.265 plays only in receivers that support it (Safari, recent Chrome on
  some platforms).
- POSIX only; no Windows build.

## License

Apache License 2.0 — see [LICENSE](LICENSE). Third-party components keep their
own licenses; see [NOTICE](NOTICE). libjuice is MPL-2.0: distributing a binary
that contains it means making its source available (unmodified here).
