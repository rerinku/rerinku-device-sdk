/* SDK logging: one function, a level and a short subsystem tag. Lines go to
 * the handler installed with rerinku_set_log_callback(), or to stdout/stderr as
 *     HH:MM:SS LVL tag     | message */
#ifndef RK_LOG_H
#define RK_LOG_H

#include "rerinku/rerinku_device.h"

#include <stdbool.h>

/* Route libjuice's logger into ours; idempotent, call before ICE use. */
void rk_log_init(void);
bool rk_log_enabled(int level);
void rk_log_write(int level, const char *tag, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define RLOG_AT(level, tag, ...)                                                  \
    do {                                                                          \
        if (rk_log_enabled(level))                                                \
            rk_log_write(level, tag, __VA_ARGS__);                                \
    } while (0)
#define RLOG_DEBUG(tag, ...) RLOG_AT(RERINKU_LOG_LEVEL_DEBUG, tag, __VA_ARGS__)
#define RLOG_INFO(tag, ...) RLOG_AT(RERINKU_LOG_LEVEL_INFO, tag, __VA_ARGS__)
#define RLOG_WARN(tag, ...) RLOG_AT(RERINKU_LOG_LEVEL_WARN, tag, __VA_ARGS__)
#define RLOG_ERR(tag, ...) RLOG_AT(RERINKU_LOG_LEVEL_ERROR, tag, __VA_ARGS__)

#endif /* RK_LOG_H */
