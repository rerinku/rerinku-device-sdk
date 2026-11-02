# Rerinku Device SDK for C

[English](README.md) · [简体中文](README.zh-CN.md) · 日本語

カメラのファームウェアを、セルフホスト型 NVR の [Rerinku View](https://github.com/rerinku/rerinku-view) に接続します。デバイスは Rerinku View(以下「プラットフォーム」)に登録し、MQTT でコマンドを受け取れる状態を保ちます。誰かが視聴または録画しているときは、**ハードウェアエンコーダーの出力**を WebRTC(WHIP)で送信します。SDK はキャプチャやエンコードを行いません。エンコード済みのフレームはファームウェアから渡します。

- 純粋な C(C11)。インクルードするヘッダーは `<rerinku/rerinku_device.h>` の 1 つだけです。
- 静的ライブラリ 1 つ(`librerinku_device.a`、依存関係をすべて含む)と共有ライブラリ 1 つ(`librerinku_device.so`)を提供します。どちらも `rerinku_*` のみをエクスポートし、C ライブラリ以外に依存しません。
- CMake(`find_package(rerinku_device)`)と pkg-config(`rerinku_device.pc`)で検出できます。
- チップベンダーのツールチェーンでクロスコンパイルできます。Rockchip RV1103 でテスト済みです(Luckfox Pico Plus:Cortex-A7、uClibc、GCC 8.3、64 MB RAM)。
- Apache-2.0 ライセンス。依存関係はすべて純粋な C で、バージョンを固定し、ソースからビルドします。

## クイックスタート

**1. SDK のビルドとインストール**(CMake 3.21 以上と C11 コンパイラーが必要です。初回の構成時に、バージョン固定された依存関係をダウンロードします):

```sh
git clone https://github.com/rerinku/rerinku-device-sdk.git
cd rerinku-device-sdk
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --component sdk --strip --prefix "$PWD/sdk"
```

**2. 組み込み。** プログラム全体は [`examples/quickstart/quickstart.c`](examples/quickstart/quickstart.c) にあります。中心部分は次のとおりです:

```c
#include <rerinku/rerinku_device.h>

rerinku_device_config_t config;
rerinku_device_config_init(&config);            /* 既定値 + struct_size */
config.endpoint = "http://192.168.1.10:8080";   /* Rerinku View */
config.sn       = "CAM-0001";                   /* このデバイスのシリアル番号 */

rerinku_device_callbacks_t callbacks = {
    .struct_size         = sizeof(callbacks),
    .on_status           = on_status,           /* 接続中 / オンライン / オフライン */
    .on_stream_start     = on_stream_start,     /* 視聴あり:エンコーダーを動かす */
    .on_stream_stop      = on_stream_stop,      /* 視聴なし:エンコーダーは休止できる */
    .on_keyframe_request = on_keyframe_request, /* 例:RK_MPI_VENC_RequestIDR */
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
rerinku_device_start(device);                   /* すぐに戻る */

/* エンコーダースレッド:エンコード済みのアクセスユニットごとに呼ぶ */
rerinku_device_write(device, 0, data, len, pts_us);

/* 終了時: */
rerinku_device_stop(device);
rerinku_device_destroy(device);
```

**3. リンク。** CMake の場合:

```cmake
find_package(rerinku_device 0.5 CONFIG REQUIRED)  # -DCMAKE_PREFIX_PATH=<sdk>
target_link_libraries(camera PRIVATE rerinku::device)          # 静的ライブラリ
# または rerinku::device_shared                                 # 共有ライブラリ
```

pkg-config の場合(Makefile ビルド、Buildroot、ベンダー SDK 向け):

```sh
export PKG_CONFIG_PATH=<sdk>/lib/pkgconfig
cc -std=c11 $(pkg-config --cflags rerinku_device) camera.c \
   "$(pkg-config --variable=libdir rerinku_device)/librerinku_device.a" -pthread   # 静的
cc -std=c11 $(pkg-config --cflags rerinku_device) camera.c \
   $(pkg-config --libs rerinku_device)                                              # 共有
```

`examples/quickstart/` には両方のビルド方法(`CMakeLists.txt`、`Makefile`)が用意されており、SDK とともに `share/rerinku_device/examples/` にインストールされます。

**4. 実行。** プラットフォームに接続します。quickstart はエンコーダーの代わりに動画ファイルを繰り返し再生します:

```sh
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=25 -t 10 -c:v libx264 \
       -profile:v baseline -g 50 -bf 0 -x264-params aud=1 clip.h264
./quickstart http://192.168.1.10:8080 QUICKSTART-001 clip.h264
```

デバイスは登録されるとすぐにプラットフォームに表示され、開くと視聴できます。より詳しいプログラムは [`examples/device_example.c`](examples/device_example.c) です。H.264/H.265 ファイルと G.711 音声に加え、コマンドとスナップショット要求を扱います。

## API の規約

API は、依存している C ライブラリ(mbedTLS、libjuice、libsrtp)の慣習に従います:

- **名前。** 関数と型は `rerinku_` で、マクロと列挙値は `RERINKU_` で始まります。型は `_t` で、コールバック型は `_cb` で終わります。列挙値には型の名前が付きます(`rerinku_stream_state_t` → `RERINKU_STREAM_STATE_*`。唯一の例外は `RERINKU_OK`)。ライブラリ全体の関数は `rerinku_*`、デバイスに対する関数は `rerinku_device_*` で、第 1 引数がデバイスです。
- **エラー。** 失敗する可能性のある関数は `rerinku_err_t` を返します。`RERINKU_OK`(0)または負の `RERINKU_ERR_*` で、説明は `rerinku_strerror()` で取得できます。`rerinku_device_write()` は代わりに `rerinku_write_result_t` を返します。結果の多く(視聴者がいない、キーフレーム待ち)は正常な状態だからです。
- **オブジェクト。** `rerinku_device_t` は不透明なハンドルです。`create` / `destroy`、`start` / `stop` で扱います。
- **バージョン付きの構造体。** 設定、チャンネル、コールバックの構造体は `struct_size` で始まります。古いヘッダーでビルドしたプログラムは小さい構造体を渡し、足りないフィールドは既定値のままになります。後から追加されたフィールドは、古いライブラリでは無視されます。
- **バージョン。** `<rerinku/rerinku_version.h>` にはヘッダーの `RERINKU_VERSION_MAJOR`、`_MINOR`、`_PATCH`、`_STRING`、`RERINKU_VERSION_NUMBER`(0xMMmmpp)があります。`rerinku_version()` と `rerinku_version_number()` は、実際にリンクされたライブラリのバージョンを返します。プロジェクトは[セマンティックバージョニング](https://semver.org/)に従います。0.x の間は、マイナーリリースで API/ABI が変わることがあり、その場合は soname も変わります(`librerinku_device.so.0.5`)。詳しくは [CHANGELOG.md](CHANGELOG.md) をご覧ください。
- **エクスポート。** エクスポートされる関数には `RERINKU_API` が付いています。それ以外はすべて非公開です。

## API の使い方

**ライフサイクル。** `rerinku_device_start()` はすぐに戻ります。登録、MQTT 接続、障害からの復旧はバックグラウンドで動作し、バックオフ(1 秒から倍々で最大 30 秒)を挟みながら無期限に再試行します。そのため、カメラがネットワークやサーバーより先に起動しても、後からオンラインになります。`on_status` は接続中 / オンライン / オフラインを通知します。プラットフォームがメディアを必要とすると `push_sfu` を送ります。SDK は `on_stream_start` を呼び、WHIP セッションを開いてキーフレームを要求します。必要なくなると `on_stream_stop` を呼びます。サーバーがメディアセッションを切断した場合(再起動、ネットワーク断)は、アプリケーションの手を煩わせずに再構築します。登録時にプラットフォームが発行するデバイストークンには有効期限があります(Rerinku View:24 時間)。SDK は有効期間の 4 分の 3 が過ぎたときに再登録します。これはサーバーの時計で計算するので、デバイスの時計が 1970 年のままでも問題ありません。WHIP 要求やスナップショットのアップロードが 401 で拒否された場合もすぐに再登録し、その要求を再試行します。

**スレッド。** すべての関数はどのスレッドからでも呼び出せます。`rerinku_device_write()` はパケット化とキューへの追加だけを行い、ネットワークには触れません。視聴者がいないときは `RERINKU_WRITE_RESULT_NOT_STREAMING` を返し、コストはほとんどかかりません。コールバックは SDK のスレッドで実行されます(`on_keyframe_request` と `on_stream_state` はメディアセッションのスレッド)。短く保ち、コールバック内から `rerinku_device_stop()` / `_destroy()` を呼ばないでください。SDK は独自に 3 つのスレッド(エージェント、コマンド処理、MQTT)を動かし、ストリーミング中はさらに 2 つ(ICE、メディアセッション)を動かします。

**コマンドとスナップショットは非同期です。** `on_command` には `rerinku_device_reply_command()` / `_fail_command()` で、`on_snapshot` には `rerinku_device_reply_snapshot()` で応答します。すぐに応答しても、後からどのスレッドで応答してもかまいません。ハードウェア JPEG エンコーダーの準備ができた時点で返せます。

**イベントはキューに入りません。** デバイスがオフラインの間、`rerinku_device_send_event()` は `RERINKU_ERR_NETWORK` を返します。重要なイベントは保持しておき、`on_status` がオンラインを通知してから再送してください。

### フレームの入力

`rerinku_device_write()` の 1 回の呼び出しで、1 つの完全なアクセスユニット(1 枚の画像のすべての NAL ユニット)または 1 つの音声フレームを、マイクロ秒単位のキャプチャ時刻とともに渡します(単調増加であれば起点は任意で、たとえば Rockchip MPI の `u64PTS` をそのまま使えます)。

H.264/H.265 チャンネルの既定は `RERINKU_BITSTREAM_FORMAT_AUTO` で、アクセスユニットごとに形式を判別します。3 バイト、4 バイト、または混在したスタートコードの Annex-B、AVC/HEVC の長さプレフィックス付きサンプル(1/2/4 バイト長)、単独の生 NAL に対応します。入力はすべて検証され、4 バイトの Annex-B に正規化されます(ゼロコピーの高速パス)。SPS/PPS(および H.265 の VPS)はキャッシュされ、これらを省略した IDR の前に補われるので、新しい受信側は必ずデコード可能なランダムアクセスポイントを受け取れます。無効な入力は `RERINKU_WRITE_RESULT_INVALID_FRAME` を返し、RTP には流れません。

コーデック:H.264、H.265(H.265 に対応しないブラウザーでは再生できません。Web の視聴者向けには H.264 を送信してください)、Opus、G.711 µ-law/A-law。

## メモリとサイズ

計測条件:RV1103(Cortex-A7 1.2 GHz)、`MinSizeRel`、640×360 H.264 約 0.8 Mbit/s と G.711A(サンプル自身の 1 MB のテスト動画を除く):

| | |
|---|---|
| `librerinku_device.so`、ストリップ後 | 0.39 MB(C ライブラリのみ必要) |
| サンプルプログラム、静的リンク、ストリップ後 | 0.39 MB |
| RSS アイドル時(登録済み、MQTT オンライン) | 約 0.8 MB |
| RSS ストリーミング時 | 約 1.8 MB |
| CPU ストリーミング時 | 1 コアの約 6% |
| スレッド | アイドル時 3、ストリーミング時 5 |

ストリーミング後、アロケーターは解放したメモリを次回のために保持します(RSS はストリーミング時に近い値のままです)。メモリは再利用され、リークではありません。

`rerinku_device_config_t` の調整項目:

- `gop_cache_max_bytes`(既定はビデオチャンネルごとに 1 MiB):SDK は現在の GOP を保持し、新しい視聴者がすぐに映像を見られるようにします。メモリの少ないデバイスでは 0 に設定して `on_keyframe_request` を実装してください。エンコーダーの IDR から映像が始まります。
- `nack_history_packets`(既定はビデオチャンネルごとに 512、1 つ約 1.2 KB):再送要求に応えるために保持する送信済みパケットです。64 MB のデバイスには 128 から 256 が適しています。

`librerinku_device.so` のサイズの内訳(RV1103):mbedTLS 約 160 KB、SDK 約 80 KB、libjuice 約 60 KB、libsrtp 約 25 KB、coreMQTT と cJSON 約 10 KB。mbedTLS は `cmake/mbedtls_config.h` に基づいてビルドされ、SDK が使う機能だけを有効にしています。ECDHE + AES-GCM の TLS/DTLS 1.2、ECDSA P-256/P-384 と RSA の証明書検証、DTLS-SRTP 拡張です。TLS 1.3 とエラー文字列テーブルは含みません(エラーは `mbedTLS -0x....` と表示されます)。

## ビルド

CMake 3.21 以上と C11 コンパイラーが必要です。初回の構成時に、バージョン固定された依存関係をダウンロードします。Linux とその他の POSIX システムのみ対応します(pthreads、BSD ソケット)。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --component sdk --strip --prefix <dir>
```

`sdk` コンポーネントが成果物です:

```
include/rerinku/rerinku_device.h       API
include/rerinku/rerinku_version.h      バージョンマクロ(生成)
lib/librerinku_device.a                SDK とすべての依存関係を 1 つにまとめた静的ライブラリ
lib/librerinku_device.so*              同じ内容の共有ライブラリ(soname librerinku_device.so.0.5)
lib/cmake/rerinku_device/              find_package(rerinku_device) → rerinku::device[_shared]
lib/pkgconfig/rerinku_device.pc        pkg-config rerinku_device(再配置可能)
share/rerinku_device/examples/         quickstart/、device_example.c
share/doc/rerinku_device/              README、CHANGELOG、LICENSE、NOTICE
```

`librerinku_device.a` は SDK とすべての依存関係を 1 つのオブジェクトにまとめたもので、グローバルシンボルは `rerinku_*` だけです。ファームウェアは 1 つのファイルをリンクするだけで済み、ファームウェア自身の mbedTLS や cJSON などと SDK が衝突することもありません。CMake プロジェクト内では、ソースツリーを直接使うこともできます。`add_subdirectory(rerinku-device-sdk)` のうえで `rerinku::device` をリンクしてください。

### クロスコンパイル

ターゲット用の CMake ツールチェーンファイルを渡します。Rockchip RV1103/RV1106 用のファイルが含まれています(ベンダーツールチェーン `arm-rockchip830-linux-uclibcgnueabihf`):

```sh
cmake -S . -B build-rv1106 -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rv1106-uclibc.cmake \
      -DRV1106_TOOLCHAIN_DIR=<sdk>/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf
cmake --build build-rv1106 -j
```

ほかのチップでは、このファイルをコピーしてコンパイラーのプレフィックスと CPU フラグを変更してください。ライブラリはファームウェアのツールチェーンでビルドする必要があります(C ライブラリと浮動小数点 ABI を一致させてください)。Luckfox Pico ボードでは、[rerinku-luckfox-pico](https://github.com/rerinku/rerinku-luckfox-pico) がこれらの手順をベンダー公式のビルドコンテナーにまとめています。

### Android

ソースツリーは変更なしで NDK でビルドできます。AGP の CMake で `add_subdirectory()` し(NDK 27、API 26 以上、16 KB ページ、C++ ランタイム不要)、C API を小さな JNI レイヤーで包みます。MediaCodec の出力をそのまま渡せます。AUTO のビットストリーム形式は、Android の Annex-B アクセスユニットをそのまま受け付けます。Android は信頼ストアをキーストアに保持しているので、`https://` / `mqtts://` を使うときはシステムのルート証明書を PEM ファイルに書き出し、`ca_file` として渡してください。

### TLS

`https://` のエンドポイントと `mqtts://` のブローカーは mbedTLS を使い、サーバー証明書とホスト名を検証します。検証には `ca_file`(PEM)、または最初に見つかったシステムの証明書バンドル(`/etc/ssl/certs/ca-certificates.crt` など)を使います。必要なルート証明書だけのファイルにすると、完全なバンドルが消費する RAM を節約できます。証明書の有効期間の確認には正しい時計(NTP)が必要です。`tls_insecure = 1` は検証を無効にします。開発時のみ使用してください。

## 仕組み

1. **登録** — `POST <endpoint>/` に `X-Rerinku: true` ヘッダーと `{sn, name, deviceId, model, fwVersion, secret}` を送ります。プラットフォームは WHIP URL、MQTT ブローカー、ICE サーバー、デバイストークンを返します。プラットフォームがデバイスキーを必須にしている場合は、`secret = base64(HMAC-SHA256(デバイスキー, sn))` です。
2. **制御** — MQTT 3.1.1(coreMQTT)、トピックは `rerinku/d/<sn>/{cmd,status,ack,data}` です。コマンドの受信、保持されるステータスとオフラインの Last Will、コマンドの確認応答、デバイスイベントを扱います。デバイスのログインは、ユーザー名が `<sn>`、パスワードがデバイストークン、クライアント ID が `rerinku-dev-<sn>` です。Rerinku View のブローカーはこれ以外を受け付けず、デバイスは自分の 4 つのトピックだけを使えます(コマンドはプラットフォームからのみ届きます)。ログインが拒否されるのは、たいていデバイスのオフライン中にトークンが期限切れになったためです。SDK は再登録し、新しいトークンで再接続します(最大で 1 分に 1 回)。ステータスは接続のたびに送信され、デバイスの静的な説明を含みます。名前、モデル、ファームウェア、チャンネルカタログ、`ext_json`(SoC、CPU、メモリ、センサーなどの自由形式の情報)で、プラットフォームが保存します。
3. **メディア** — `push_sfu` を受けると、1 回の WHIP POST で、BUNDLE と rtcp-mux を使う送信専用の offer をすべての ICE 候補とともに送ります。プラットフォームの SFU が応答します。ICE は libjuice、DTLS-SRTP の鍵交換は mbedTLS(どちらの DTLS ロールにも対応し、相手は SDP のフィンガープリントで認証)、SRTP は libsrtp が担当します。RTP:H.264(RFC 6184 シングル NAL / FU-A)、H.265(RFC 7798 シングル NAL / FU)、G.711、Opus。映像はペーシングされます(上限 48 Mbit/s、平滑化するのはキーフレームのバーストのみ)。NACK には履歴から応答し、PLI/FIR は `on_keyframe_request` になり、送信者レポートは毎秒送信します。`stop` でセッションを閉じます(WHIP DELETE)。

   **複数ストリームのオンデマンド送信。** `stream_id` の異なるチャンネル(カメラの `main` と `sub` ストリーム)はすべて 1 つのセッションで宣言されますが、メディアを送るのはプラットフォームが現在必要としているストリームだけです。`push_sfu` がそれらを `{"streams": ["main"], "rev": …}` のように指定します。これは差分ではなく完全な集合です。セッション中に届いた `push_sfu` は、再ネゴシエーションせずにチャンネルのオン/オフだけを切り替えます。送信を始めるチャンネルはキーフレームから始まり(そのチャンネルの `on_keyframe_request` が発火します)、不要なチャンネルに書き込んだフレームは `RERINKU_WRITE_RESULT_NOT_STREAMING` を返し、そのチャンネルへのキーフレーム要求は破棄されます。`rev` はコマンドごとに増え、最後に適用したものより古いコマンドは無視されます。`streams` がない場合(古いプラットフォーム)や、デバイスのストリームがひとつも指定されていない場合は、すべてのチャンネルを送信します。ストリーミング中、音声チャンネルは常に送信します。カメラの音声はすべてのストリームで使われるためです。ステータスには送信中のストリームが含まれます(`"streams"`)。
4. **スナップショット** — `get_snapshot` → `on_snapshot` → デバイストークンを付けて JPEG を `/api/v1/devices/<sn>/snapshot` に `POST` します。
5. **オフライン中の録画** — `rerinku_device_upload_recording()` は、デバイスがプラットフォームに接続できない間に録画した MP4(H.264/H.265、AAC は任意)を、デバイストークンを付けて `POST /api/v1/devices/<sn>/recordings?start=<ms>&stream=<id>` にストリーム送信します。Rerinku View は録画された時刻の録画として保存します。この呼び出しはブロックするので、オンラインのときに自分のスレッドから呼んでください。
6. **実行状態** — `get_runtime` → `on_runtime_info`:温度、CPU 負荷、メモリなど、デバイスが計測する情報に要求時のみ応答します。誰かがデバイスを開いている間、プラットフォームは数秒ごとに問い合わせます。それ以外は何も送信しません。

組み込みコマンド:`push_sfu`、`stop`、`get_runtime`、`get_snapshot`。それ以外はすべて `on_command` に渡されます。

### デバイス情報

2 種類あり、経路も別です:

| | 静的(`ext_json`、設定) | リアルタイム(`on_runtime_info`、コールバック) |
|---|---|---|
| 送信のタイミング | ステータスとともに、接続のたび | プラットフォームの要求時(`get_runtime`) |
| 保存 | あり、デバイスとともに保存 | なし、受信したまま表示 |
| プラットフォームがラベルを付けるキー | `manufacturer`、`soc`、`cpu`、`cpuCores`、`cpuMHz`、`memoryKB`、`sensor`、`os`、`mac` | `tempC`、`cpuPct`、`memTotalKB`、`memAvailableKB`、`uptimeSec`、`load1` |

どちらもデバイスが自由に決められる JSON オブジェクトです。プラットフォームが知らないキーはそのままの名前で表示されるので、プラットフォームを変更せずに、ファンの回転数、バッテリー残量、Wi-Fi の信号強度などを追加できます。

## サードパーティーコンポーネント

| コンポーネント | バージョン | 用途 | ライセンス |
|---|---|---|---|
| [libjuice](https://github.com/paullouisageneau/libjuice) | 1.6.2 | ICE(STUN/TURN) | MPL-2.0 |
| [libsrtp](https://github.com/cisco/libsrtp) | 2.7.0 | SRTP | BSD-3-Clause |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | 3.6.2 | DTLS-SRTP、HTTPS、MQTTS | Apache-2.0 |
| [coreMQTT](https://github.com/FreeRTOS/coreMQTT) | 2.3.1 | MQTT クライアント | MIT |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.18 | JSON | MIT |

## 既知の制限

- 送信専用です(WHIP)。再生、trickle ICE、データチャンネルには対応していません。
- H.265 は対応する受信側でのみ再生できます(Safari、一部のプラットフォームの最近の Chrome)。
- POSIX のみ対応です。Windows 版はありません。

## ライセンス

Apache License 2.0。[LICENSE](LICENSE) をご覧ください。サードパーティーコンポーネントにはそれぞれのライセンスが適用されます。[NOTICE](NOTICE) をご覧ください。libjuice は MPL-2.0 です。これを含むバイナリを配布する場合は、そのソースを入手できるようにする必要があります(ここでは変更していません)。
