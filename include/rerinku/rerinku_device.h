/**
 * @file rerinku_device.h
 * @brief Rerinku Device SDK — the C API.
 *
 * Connects a camera's firmware to Rerinku View: the device
 * registers with the platform, stays reachable for commands over MQTT, and
 * pushes its hardware encoder's output over WebRTC (WHIP) when someone watches
 * or records. The SDK does not capture or encode; the firmware feeds already
 * encoded frames through rerinku_device_write().
 *
 * @code
 * rerinku_device_config_t cfg;
 * rerinku_device_config_init(&cfg);
 * cfg.endpoint = "http://192.168.1.10:8080";
 * cfg.sn       = "CAM-0001";
 *
 * rerinku_device_callbacks_t cb = { .struct_size = sizeof(cb) };
 * cb.on_keyframe_request = ask_encoder_for_idr;
 *
 * rerinku_device_t *dev;
 * rerinku_device_create(&cfg, &cb, &dev);
 * rerinku_device_add_channel(dev, &main_video);
 * rerinku_device_start(dev);                       // returns immediately
 * // encoder thread: rerinku_device_write(dev, channel_id, data, len, pts_us);
 * rerinku_device_stop(dev);
 * rerinku_device_destroy(dev);
 * @endcode
 *
 * @par Naming
 * - Functions and types start with `rerinku_`, macros and enum values with
 *   `RERINKU_`. Types end in `_t`, callback types in `_cb`.
 * - Enum values carry their type's name: rerinku_stream_state_t has
 *   `RERINKU_STREAM_STATE_*`. The one exception is #RERINKU_OK.
 * - Library-wide functions are `rerinku_*`; functions on a device are
 *   `rerinku_device_*` and take the device first.
 *
 * @par Errors
 * Functions that can fail return #rerinku_err_t: #RERINKU_OK (0) or a
 * negative `RERINKU_ERR_*` code; rerinku_strerror() describes it.
 * rerinku_device_write() returns a #rerinku_write_result_t instead: most of
 * its outcomes (nobody watching, waiting for a keyframe) are normal.
 *
 * @par Threading
 * Every function may be called from any thread unless noted. Callbacks run on
 * SDK threads; keep them short and never call rerinku_device_stop() or
 * rerinku_device_destroy() from inside one.
 *
 * @par Memory
 * Strings passed in are copied; the caller keeps ownership of everything it
 * passes, including frame data (valid only during the call).
 *
 * @par Versioned structs
 * Configuration, channel and callback structs start with `struct_size`. A
 * program built against an older header passes a smaller struct: the fields
 * it does not know keep their defaults. Fields added by a newer header are
 * ignored by an older library.
 */
#ifndef RERINKU_DEVICE_H
#define RERINKU_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#include <rerinku/rerinku_version.h>

/** @brief Marks the functions the library exports.
 *
 * Linux builds export exactly these (`-fvisibility=hidden` plus a version
 * script). Windows is not supported yet; its branch keeps the header portable. */
#ifndef RERINKU_API
#if defined(_WIN32)
#if defined(RERINKU_BUILDING_LIBRARY)
#define RERINKU_API __declspec(dllexport)
#elif defined(RERINKU_SHARED)
#define RERINKU_API __declspec(dllimport)
#else
#define RERINKU_API
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define RERINKU_API __attribute__((visibility("default")))
#else
#define RERINKU_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- enumerations ------------------------------------------------------- */

/** @brief Result of the functions that can fail. */
typedef enum rerinku_err {
    RERINKU_OK = 0,
    RERINKU_ERR_INVALID_ARGUMENT = -1,
    /** Not allowed now (e.g. adding a channel after start, an upload before
     *  the device has registered). */
    RERINKU_ERR_INVALID_STATE = -2,
    RERINKU_ERR_NO_MEMORY = -3,
    RERINKU_ERR_INTERNAL = -4,
    /** The platform will never accept this (e.g. an upload that is not an
     *  MP4): do not retry. */
    RERINKU_ERR_REJECTED = -5,
    /** Not delivered now (offline, platform unreachable): try again later. */
    RERINKU_ERR_NETWORK = -6,
    /** The platform does not offer this (e.g. it takes no uploads). */
    RERINKU_ERR_NOT_SUPPORTED = -7
} rerinku_err_t;

/** @brief What a channel carries. */
typedef enum rerinku_media_kind {
    RERINKU_MEDIA_KIND_VIDEO = 0,
    RERINKU_MEDIA_KIND_AUDIO = 1
} rerinku_media_kind_t;

/** @brief Encoding of a channel. Video: H.264, H.265. Audio: Opus, G.711. */
typedef enum rerinku_codec {
    RERINKU_CODEC_H264 = 0,
    RERINKU_CODEC_H265 = 1,
    RERINKU_CODEC_OPUS = 2,
    RERINKU_CODEC_G711U = 3,
    RERINKU_CODEC_G711A = 4
} rerinku_codec_t;

/** @brief How H.264/H.265 access units are laid out.
 *
 * AUTO recognizes each access unit independently (Annex-B with 3- or 4-byte
 * start codes, 1/2/4-byte length prefixes, or a single raw NAL); the explicit
 * formats validate strictly. */
typedef enum rerinku_bitstream_format {
    RERINKU_BITSTREAM_FORMAT_AUTO = 0,
    RERINKU_BITSTREAM_FORMAT_ANNEX_B = 1,
    RERINKU_BITSTREAM_FORMAT_LENGTH_PREFIXED = 2,
    RERINKU_BITSTREAM_FORMAT_RAW_NAL = 3
} rerinku_bitstream_format_t;

/** @brief The device's connection to the platform (control plane), reported
 *  through rerinku_device_callbacks_t::on_status. */
typedef enum rerinku_device_status {
    RERINKU_DEVICE_STATUS_STOPPED = 0,
    /** Registering with the platform / connecting to MQTT. */
    RERINKU_DEVICE_STATUS_CONNECTING = 1,
    /** Reachable for commands. */
    RERINKU_DEVICE_STATUS_ONLINE = 2,
    /** Connection lost; reconnecting with backoff. */
    RERINKU_DEVICE_STATUS_OFFLINE = 3
} rerinku_device_status_t;

/** @brief State of the media session (WebRTC), reported through
 *  rerinku_device_callbacks_t::on_stream_state. */
typedef enum rerinku_stream_state {
    RERINKU_STREAM_STATE_NEW = 0,
    RERINKU_STREAM_STATE_CONNECTING = 1,
    RERINKU_STREAM_STATE_CONNECTED = 2,
    RERINKU_STREAM_STATE_FAILED = 3,
    RERINKU_STREAM_STATE_CLOSED = 4,
    RERINKU_STREAM_STATE_DISCONNECTED = 5
} rerinku_stream_state_t;

/** @brief Outcome of rerinku_device_write(). Only SENT means the frame is on
 *  its way; none of the others is an error to report. */
typedef enum rerinku_write_result {
    RERINKU_WRITE_RESULT_SENT = 0,
    /** Nobody is watching or recording: dropped, cheaply. */
    RERINKU_WRITE_RESULT_NOT_STREAMING = 1,
    RERINKU_WRITE_RESULT_UNKNOWN_CHANNEL = 2,
    /** A media session is being set up: dropped. */
    RERINKU_WRITE_RESULT_CONNECTING = 3,
    /** Not a valid access unit / audio frame: never reaches the network. */
    RERINKU_WRITE_RESULT_INVALID_FRAME = 4,
    /** The link cannot keep up (send queue full) or memory ran out: dropped.
     *  Video resumes at the next keyframe, which the SDK requests. */
    RERINKU_WRITE_RESULT_DROPPED = 5,
    /** A new session starts on a keyframe: inter frames before it are dropped. */
    RERINKU_WRITE_RESULT_WAITING_FOR_KEYFRAME = 6
} rerinku_write_result_t;

/** @brief Severity of an SDK log line. */
typedef enum rerinku_log_level {
    RERINKU_LOG_LEVEL_ERROR = 0,
    RERINKU_LOG_LEVEL_WARN = 1,
    RERINKU_LOG_LEVEL_INFO = 2,
    RERINKU_LOG_LEVEL_DEBUG = 3
} rerinku_log_level_t;

/* ---- library -------------------------------------------------------------- */

/** @brief Version of the library actually linked, "MAJOR.MINOR.PATCH".
 *  Compare with #RERINKU_VERSION_STRING to detect a header/library mismatch. */
RERINKU_API const char *rerinku_version(void);

/** @brief Version of the library actually linked, as #RERINKU_VERSION_NUMBER
 *  (0xMMmmpp). */
RERINKU_API uint32_t rerinku_version_number(void);

/** @brief A short English description of @p err ("invalid argument"). */
RERINKU_API const char *rerinku_strerror(rerinku_err_t err);

/**
 * @brief Receives SDK log lines.
 *
 * Called from several SDK threads, possibly at once.
 * @param user    The pointer given to rerinku_set_log_callback().
 * @param level   Severity.
 * @param tag     Subsystem ("agent", "mqtt", "whip", ...).
 * @param message One line without a trailing newline, valid only during the call.
 */
typedef void (*rerinku_log_cb)(void *user, rerinku_log_level_t level, const char *tag,
                               const char *message);

/** @brief Route SDK log lines into the firmware's own logging (syslog, a ring
 *  buffer). NULL restores the default printer (stdout/stderr). */
RERINKU_API void rerinku_set_log_callback(rerinku_log_cb callback, void *user);

/** @brief Drop lines below @p level (default #RERINKU_LOG_LEVEL_INFO). */
RERINKU_API void rerinku_set_log_level(rerinku_log_level_t level);

/* ---- configuration ---------------------------------------------------------- */

/** @brief An opaque device: one camera's connection to the platform. */
typedef struct rerinku_device rerinku_device_t;

/** @brief Device configuration. Initialize with rerinku_device_config_init(). */
typedef struct rerinku_device_config {
    /** sizeof(rerinku_device_config_t); set by rerinku_device_config_init(). */
    size_t struct_size;

    /** Platform base URL, e.g. "http://192.168.1.10:8080". Required. */
    const char *endpoint;
    /** Unique serial number: the device's identity and routing key on the
     *  platform. Required; no '/', '+' or '#'. */
    const char *sn;
    /** base64(HMAC-SHA256(device key, sn)) when the platform enforces device
     *  keys; else NULL. */
    const char *secret;
    /** Display name (NULL = sn). */
    const char *name;
    /** Hardware identity, reported as-is (NULL = none). */
    const char *device_id;
    /** Model, for display (NULL = none). */
    const char *model;
    /** Firmware version, for display (NULL = none). */
    const char *fw_version;
    /**
     * Further static facts as a JSON object of the device's choosing (NULL =
     * none). Sent with the status on every connect; the platform stores it and
     * lists it in the device's information panel. Keys it labels itself:
     * manufacturer, soc, cpu, cpuCores, cpuMHz, memoryKB, sensor, os, mac; any
     * other key is listed under its own name. Invalid JSON or a non-object makes
     * rerinku_device_create() fail.
     * @code {"soc":"Rockchip RV1103","cpu":"Cortex-A7","cpuCores":1,"memoryKB":33652} @endcode
     */
    const char *ext_json;

    /** CA bundle (PEM) for https:// endpoints and mqtts:// brokers; NULL = the
     *  first system bundle found (/etc/ssl/certs/ca-certificates.crt, ...). A
     *  file holding only the roots you need saves the RAM a full bundle costs. */
    const char *ca_file;
    /** 1 = skip certificate verification. Development only. */
    int tls_insecure;

    /** Per video channel: the SDK keeps the current GOP (at most this many
     *  bytes) so a new viewer starts on a keyframe at once. 0 disables the
     *  cache; then implement on_keyframe_request. Default 1 MiB. */
    uint32_t gop_cache_max_bytes;
    /** Sent packets (~1.2 KB each) kept per track to answer retransmission
     *  requests. Default 512; 128-256 suits small-memory devices. */
    uint32_t nack_history_packets;

    /** A lost media session is rebuilt with backoff doubling from 1 s up to
     *  this cap. Default 30. */
    uint32_t stream_reconnect_max_delay_sec;
    /** Give up after this many consecutive failed rebuilds and fire
     *  on_stream_stop("connection_lost"). 0 (default) = never give up. */
    uint32_t stream_reconnect_max_attempts;
} rerinku_device_config_t;

/** @brief Zero @p config, set struct_size and the defaults documented above. */
RERINKU_API void rerinku_device_config_init(rerinku_device_config_t *config);

/** @brief One encoded elementary stream the device can send. */
typedef struct rerinku_device_channel {
    /** sizeof(rerinku_device_channel_t). */
    size_t struct_size;
    /** Your id; passed back to rerinku_device_write() and on_keyframe_request. */
    uint32_t channel_id;
    rerinku_media_kind_t kind;
    rerinku_codec_t codec;
    /** Video only. AUTO accepts Annex-B, length-prefixed and raw NAL input. */
    rerinku_bitstream_format_t bitstream_format;
    /** E.g. "main", "sub", "audio" (NULL = none). */
    const char *label;
    /** Groups tracks into one viewable stream: give the main video and its
     *  audio the same stream_id, the sub stream another. NULL = "rerinku". */
    const char *stream_id;
    /** SDP fmtp override (NULL = the codec's default). */
    const char *codec_parameters;

    /** Declared parameters, shown on the platform; 0 = unknown. Video: */
    uint32_t width, height, fps, bitrate_bps;
    /** Audio: */
    uint32_t sample_rate, audio_channels, sample_bits;
} rerinku_device_channel_t;

/* ---- callbacks ------------------------------------------------------------------ */

/**
 * @brief What the platform asks of the device. All optional.
 *
 * Set struct_size = sizeof(rerinku_device_callbacks_t); `user` is passed back
 * unchanged.
 */
typedef struct rerinku_device_callbacks {
    size_t struct_size;
    void *user;

    /** The connection to the platform changed (connecting/online/offline/stopped). */
    void (*on_status)(void *user, rerinku_device_status_t status);

    /** The media session changed state. Runs on the session's thread. */
    void (*on_stream_state)(void *user, rerinku_stream_state_t state);

    /** Someone wants the stream: start the encoders and feed frames. Return 0
     *  to accept, non-zero to refuse (the platform sees the request fail). */
    int (*on_stream_start)(void *user);

    /** Nobody needs the stream any more ("command"), the session could not be
     *  started ("start_failed") or recovered ("connection_lost"), or the SDK is
     *  stopping ("shutdown"). The encoders may idle. */
    void (*on_stream_stop)(void *user, const char *reason);

    /** Ask the encoder for an IDR now (e.g. RK_MPI_VENC_RequestIDR): a viewer
     *  joined, lost the picture, or a session just opened. Throttled per
     *  channel. Runs on a network thread: flag the encoder, do not block. */
    void (*on_keyframe_request)(void *user, uint32_t channel_id);

    /** A platform command the SDK does not handle itself (reboot, PTZ, ...).
     *  @p params_json is the command's params object. Answer with
     *  rerinku_device_reply_command() / rerinku_device_fail_command(), now or
     *  later from any thread. */
    void (*on_command)(void *user, const char *command_id, const char *action,
                       const char *params_json);

    /** The platform wants a thumbnail. Capture a JPEG and pass it to
     *  rerinku_device_reply_snapshot(@p request_id, ...), now or later from
     *  any thread (e.g. when the hardware JPEG encoder delivers). */
    void (*on_snapshot)(void *user, const char *request_id);

    /**
     * Live state for the platform, which asks every few seconds while someone
     * looks at the device. Write a JSON object into @p buf (at most @p cap
     * bytes including the terminating NUL) and return its length; 0 fails the
     * request. Runs on the command thread: a slow answer delays other commands.
     * Keys the platform renders itself: tempC, cpuPct, memTotalKB,
     * memAvailableKB, uptimeSec, load1; any other key is listed under its own name.
     * @code {"tempC":63.6,"cpuPct":11.2,"memTotalKB":33652,"memAvailableKB":15304} @endcode
     */
    size_t (*on_runtime_info)(void *user, char *buf, size_t cap);
} rerinku_device_callbacks_t;

/* ---- lifecycle -------------------------------------------------------------------- */

/**
 * @brief Create a device. @p config and @p callbacks are copied.
 * @param callbacks May be NULL.
 * @param[out] out_device The new device; destroy it with rerinku_device_destroy().
 * @return #RERINKU_OK, or #RERINKU_ERR_INVALID_ARGUMENT (missing endpoint/sn,
 *         invalid ext_json), #RERINKU_ERR_NO_MEMORY.
 */
RERINKU_API rerinku_err_t rerinku_device_create(const rerinku_device_config_t *config,
                                                const rerinku_device_callbacks_t *callbacks,
                                                rerinku_device_t **out_device);

/** @brief Register a channel. Only before rerinku_device_start().
 *  @return #RERINKU_OK, #RERINKU_ERR_INVALID_ARGUMENT (bad kind/codec pair,
 *          duplicate id), #RERINKU_ERR_INVALID_STATE (already started). */
RERINKU_API rerinku_err_t rerinku_device_add_channel(rerinku_device_t *device,
                                                     const rerinku_device_channel_t *channel);

/**
 * @brief Go online in the background: register with the platform, connect,
 *        wait for commands.
 *
 * Returns at once. Network trouble is retried forever with backoff and
 * reported through on_status, so a camera that boots before the network or
 * the platform is up simply comes online later.
 * @return #RERINKU_OK, or #RERINKU_ERR_INVALID_STATE (no channels, already started).
 */
RERINKU_API rerinku_err_t rerinku_device_start(rerinku_device_t *device);

/** @brief Stop streaming, publish offline and join every SDK thread. Blocks
 *  briefly (well under a second; up to a network timeout while a connection
 *  attempt is in flight). The device can be started again. */
RERINKU_API void rerinku_device_stop(rerinku_device_t *device);

/** @brief Stop if needed and free the device. NULL is allowed. */
RERINKU_API void rerinku_device_destroy(rerinku_device_t *device);

/** @brief Name of a device status ("online"), for logs. */
RERINKU_API const char *rerinku_device_status_name(rerinku_device_status_t status);

/* ---- media ------------------------------------------------------------------------- */

/**
 * @brief Feed one encoded frame.
 *
 * Video: one complete access unit (all NAL units of a picture; SPS/PPS may
 * precede keyframes and are restored before keyframes that omit them). Audio:
 * one Opus or G.711 frame.
 *
 * Never blocks on the network. Cheap while nobody is watching
 * (#RERINKU_WRITE_RESULT_NOT_STREAMING), so feeding continuously is fine;
 * idling the encoder on on_stream_stop saves power.
 *
 * @param channel_id The channel's id from rerinku_device_add_channel().
 * @param pts_us     Capture time in microseconds, monotonic per channel, any
 *                   origin (e.g. Rockchip MPI's u64PTS as is).
 */
RERINKU_API rerinku_write_result_t rerinku_device_write(rerinku_device_t *device,
                                                        uint32_t channel_id,
                                                        const uint8_t *data, size_t len,
                                                        uint64_t pts_us);

/* ---- replies and events -------------------------------------------------------------- */

/** @brief Answer a command from on_command with success.
 *  @param result_json Response data (JSON) or NULL. A second answer to the
 *         same command is ignored. */
RERINKU_API rerinku_err_t rerinku_device_reply_command(rerinku_device_t *device,
                                                       const char *command_id,
                                                       const char *result_json);

/** @brief Answer a command from on_command with failure. @p error is a short
 *  message shown on the platform. */
RERINKU_API rerinku_err_t rerinku_device_fail_command(rerinku_device_t *device,
                                                      const char *command_id,
                                                      const char *error);

/** @brief Answer on_snapshot with a JPEG (copied). @p jpeg NULL or @p len 0
 *  fails the request ("no frame available"). */
RERINKU_API rerinku_err_t rerinku_device_reply_snapshot(rerinku_device_t *device,
                                                        const char *request_id,
                                                        const uint8_t *jpeg, size_t len);

/**
 * @brief Hand the platform a recording the device made on its own.
 *
 * For footage recorded while the platform was unreachable ("seamless" /
 * SD-card recording): filed with the platform's recordings at
 * @p start_unix_ms. Blocks until the platform has taken the file (it is
 * streamed, never loaded whole): call it from a thread of your own, never
 * from a callback, and only while online. A refused device token is renewed
 * and the upload retried once.
 *
 * @param path          An MP4 with H.264 or H.265 video and optional AAC audio.
 * @param stream_id     The channels' stream_id; NULL = the default stream.
 * @param start_unix_ms Wall clock of the file's first video frame, in ms.
 * @return #RERINKU_OK; #RERINKU_ERR_REJECTED (not an MP4, no video: do not
 *         retry); #RERINKU_ERR_NETWORK (not delivered: retry later);
 *         #RERINKU_ERR_NOT_SUPPORTED (the platform takes no uploads);
 *         #RERINKU_ERR_INVALID_STATE (the device has not registered yet).
 */
RERINKU_API rerinku_err_t rerinku_device_upload_recording(rerinku_device_t *device,
                                                          const char *path,
                                                          const char *stream_id,
                                                          uint64_t start_unix_ms);

/**
 * @brief Send a device-initiated event to the platform (motion, alarm, progress...).
 *
 * @param action       Event name, e.g. "motion".
 * @param payload_json Embedded as JSON when it parses, else as a string.
 * @return #RERINKU_OK once the event is on its way; #RERINKU_ERR_NETWORK while
 *         the device is not connected to the platform — the event was NOT sent
 *         and the SDK does not queue it: keep it and send it again after
 *         on_status reports #RERINKU_DEVICE_STATUS_ONLINE if it matters;
 *         #RERINKU_ERR_INVALID_STATE before rerinku_device_start().
 */
RERINKU_API rerinku_err_t rerinku_device_send_event(rerinku_device_t *device, const char *action,
                                                    const char *payload_json);

#ifdef __cplusplus
}
#endif

#endif /* RERINKU_DEVICE_H */
