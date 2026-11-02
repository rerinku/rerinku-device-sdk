/* Internal helpers shared by every SDK module: time, growable buffers,
 * strings, random bytes and monotonic condition-variable waits. */
#ifndef RK_COMMON_H
#define RK_COMMON_H

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RK_MIN(a, b) ((a) < (b) ? (a) : (b))
#define RK_MAX(a, b) ((a) > (b) ? (a) : (b))
#define RK_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Monotonic clock (never jumps with NTP); wall clock only for RTCP NTP time
 * and token expiry. */
uint64_t rk_now_ms(void);
uint64_t rk_now_us(void);
uint64_t rk_wall_us(void);

void rk_sleep_ms(uint64_t ms);

/* Cryptographically random bytes (getrandom / /dev/urandom). */
void rk_random(void *buf, size_t len);
uint32_t rk_random_u32(void);

/* NULL-safe strdup; NULL in gives NULL out. */
char *rk_strdup(const char *s);
char *rk_strndup(const char *s, size_t n);
/* printf into a freshly allocated string (NULL on failure). */
char *rk_sprintf(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
/* Copy into a fixed buffer, always NUL-terminated. */
void rk_strlcpy(char *dst, const char *src, size_t size);
bool rk_streq(const char *a, const char *b); /* NULL-safe */

/* Growable byte buffer. Zero-initialize ({0}) before use. */
typedef struct rk_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} rk_buf;

bool rk_buf_reserve(rk_buf *b, size_t extra);
bool rk_buf_append(rk_buf *b, const void *data, size_t len);
bool rk_buf_append_str(rk_buf *b, const char *s);
bool rk_buf_appendf(rk_buf *b, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* NUL-terminate without counting the terminator in len. */
bool rk_buf_terminate(rk_buf *b);
void rk_buf_free(rk_buf *b);
/* Hand the bytes to the caller (NUL-terminated); the buffer is reset. */
char *rk_buf_steal_str(rk_buf *b);

/* Condition variable whose timed waits use the monotonic clock. */
void rk_cond_init(pthread_cond_t *cond);
/* Wait until signalled or until the monotonic deadline (ms) passes.
 * Returns false on timeout. */
bool rk_cond_wait_until(pthread_cond_t *cond, pthread_mutex_t *mu, uint64_t deadline_ms);

/* Base64url (no padding required) decode into a NUL-terminated string. */
char *rk_base64url_decode(const char *in, size_t len);

#endif /* RK_COMMON_H */
