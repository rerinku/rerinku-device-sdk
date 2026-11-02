# Changelog

All notable changes to the Rerinku Device SDK. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/): while the version is 0.x, a minor
release (0.5 → 0.6) may change the API or ABI, and the shared library's soname
changes with it (`librerinku_device.so.0.5`); a patch release never does.

## [0.5.0] - unreleased

First public release.

### Added

- `<rerinku/rerinku_version.h>`: `RERINKU_VERSION_MAJOR/MINOR/PATCH`,
  `RERINKU_VERSION_STRING`, `RERINKU_VERSION_NUMBER` (0xMMmmpp), and
  `rerinku_version_number()` for the library actually linked.
- `RERINKU_API` on every exported function; the library is built with
  hidden visibility.
- CMake package: `find_package(rerinku_device 0.5 CONFIG)` provides
  `rerinku::device` (static) and `rerinku::device_shared`.
- pkg-config file `rerinku_device.pc`, relocatable (an unpacked SDK works
  wherever it lands).
- `examples/quickstart/`: the smallest complete integration, with a CMake and a
  pkg-config build.
- MQTT login: the device connects with username = SN, password = its device
  token, client id `rerinku-dev-<sn>`; a refused login renews the token.
- Several streams per device, sent on demand: `push_sfu` may carry
  `"streams"` (the stream ids to send, the complete set) and `"rev"`; while
  the session is up it only switches channels on or off, a channel that
  starts sending begins at a keyframe, and frames of unwanted channels are not
  sent. Without `"streams"` every channel is sent, as before. The status
  lists the streams being sent.
- `rerinku_device_send_event()` reports what happened to the event:
  `RERINKU_ERR_NETWORK` while offline, `RERINKU_ERR_INVALID_STATE` before start.

### Changed

- The library is `librerinku_device` (`.a` / `.so`); the soname carries the
  minor version while 0.x.
- Naming: enum values carry their type's name, errors are library-wide, and
  functions return their real types.

  | 0.4 | 0.5 |
  |---|---|
  | `rerinku_device_err_t`, `RERINKU_DEVICE_OK`, `RERINKU_DEVICE_ERR_*` | `rerinku_err_t`, `RERINKU_OK`, `RERINKU_ERR_*` |
  | `rerinku_device_strerror()` | `rerinku_strerror()` |
  | `RERINKU_MEDIA_VIDEO` / `_AUDIO` | `RERINKU_MEDIA_KIND_VIDEO` / `_AUDIO` |
  | `rerinku_media_codec_t` | `rerinku_codec_t` |
  | `RERINKU_BITSTREAM_*` | `RERINKU_BITSTREAM_FORMAT_*` |
  | `rerinku_state_t`, `RERINKU_STATE_*` | `rerinku_stream_state_t`, `RERINKU_STREAM_STATE_*` |
  | `RERINKU_DEVICE_STOPPED` … `_OFFLINE` | `RERINKU_DEVICE_STATUS_STOPPED` … `_OFFLINE` |
  | `rerinku_device_status_string()` | `rerinku_device_status_name()` |
  | `RERINKU_WRITE_*` | `RERINKU_WRITE_RESULT_*` |
  | `RERINKU_WRITE_NOT_STARTED` | `RERINKU_WRITE_RESULT_NOT_STREAMING` |
  | `RERINKU_WRITE_CHANNEL_NOT_OPEN` | `RERINKU_WRITE_RESULT_CONNECTING` |
  | `RERINKU_WRITE_INVALID_SAMPLE` | `RERINKU_WRITE_RESULT_INVALID_FRAME` |
  | `RERINKU_WRITE_SEND_FAILED` | `RERINKU_WRITE_RESULT_DROPPED` |
  | `RERINKU_LOG_*` | `RERINKU_LOG_LEVEL_*` |
  | `rerinku_log_fn`, `rerinku_set_log_handler()` | `rerinku_log_cb`, `rerinku_set_log_callback()` |

  Numeric values are unchanged.
- The header is documented in Doxygen format.

### Removed

- `RERINKU_DEVICE_API_VERSION` and `rerinku_device_api_version()`: the
  version macros and `struct_size` cover what they did.
