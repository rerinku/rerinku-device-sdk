#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

uint64_t rk_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint64_t rk_now_ms(void) { return rk_now_us() / 1000u; }

uint64_t rk_wall_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

void rk_sleep_ms(uint64_t ms) {
    struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

void rk_random(void *buf, size_t len) {
    uint8_t *p = buf;
#if defined(__linux__) && defined(SYS_getrandom)
    while (len > 0) {
        long n = syscall(SYS_getrandom, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        p += n;
        len -= (size_t)n;
    }
    if (len == 0)
        return;
#endif
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    while (fd >= 0 && len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        p += n;
        len -= (size_t)n;
    }
    if (fd >= 0)
        close(fd);
    /* Last resort, only reached without getrandom and /dev/urandom. */
    for (size_t i = 0; i < len; ++i)
        p[i] = (uint8_t)(rk_now_us() >> (i % 8) ^ (uintptr_t)p);
}

uint32_t rk_random_u32(void) {
    uint32_t v;
    rk_random(&v, sizeof(v));
    return v;
}

char *rk_strdup(const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out)
        memcpy(out, s, n + 1);
    return out;
}

char *rk_strndup(const char *s, size_t n) {
    if (!s)
        return NULL;
    char *out = malloc(n + 1);
    if (out) {
        memcpy(out, s, n);
        out[n] = '\0';
    }
    return out;
}

char *rk_sprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return NULL;
    char *out = malloc((size_t)n + 1);
    if (!out)
        return NULL;
    va_start(ap, fmt);
    vsnprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return out;
}

void rk_strlcpy(char *dst, const char *src, size_t size) {
    if (size == 0)
        return;
    size_t n = src ? strlen(src) : 0;
    if (n >= size)
        n = size - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

bool rk_streq(const char *a, const char *b) {
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

bool rk_buf_reserve(rk_buf *b, size_t extra) {
    if (extra > SIZE_MAX - b->len - 1)
        return false;
    size_t need = b->len + extra + 1; /* room for a terminator */
    if (need <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need)
        cap = cap > SIZE_MAX / 2 ? need : cap * 2;
    uint8_t *p = realloc(b->data, cap);
    if (!p)
        return false;
    b->data = p;
    b->cap = cap;
    return true;
}

bool rk_buf_append(rk_buf *b, const void *data, size_t len) {
    if (len == 0)
        return true;
    if (!rk_buf_reserve(b, len))
        return false;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return true;
}

bool rk_buf_append_str(rk_buf *b, const char *s) {
    return s ? rk_buf_append(b, s, strlen(s)) : true;
}

bool rk_buf_appendf(rk_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !rk_buf_reserve(b, (size_t)n))
        return false;
    va_start(ap, fmt);
    vsnprintf((char *)b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return true;
}

bool rk_buf_terminate(rk_buf *b) {
    if (!rk_buf_reserve(b, 0))
        return false;
    b->data[b->len] = '\0';
    return true;
}

void rk_buf_free(rk_buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

char *rk_buf_steal_str(rk_buf *b) {
    if (!rk_buf_terminate(b))
        return NULL;
    char *s = (char *)b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return s;
}

void rk_cond_init(pthread_cond_t *cond) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
#if !defined(__APPLE__)
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
}

bool rk_cond_wait_until(pthread_cond_t *cond, pthread_mutex_t *mu, uint64_t deadline_ms) {
#if defined(__APPLE__)
    uint64_t now = rk_now_ms();
    uint64_t wait_ms = deadline_ms > now ? deadline_ms - now : 0;
    uint64_t wall = rk_wall_us() / 1000u + wait_ms;
    struct timespec ts = {(time_t)(wall / 1000u), (long)(wall % 1000u) * 1000000L};
#else
    struct timespec ts = {(time_t)(deadline_ms / 1000u), (long)(deadline_ms % 1000u) * 1000000L};
#endif
    return pthread_cond_timedwait(cond, mu, &ts) != ETIMEDOUT;
}

char *rk_base64url_decode(const char *in, size_t len) {
    char *out = malloc(len * 3 / 4 + 4);
    if (!out)
        return NULL;
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = in[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else if (c == '=') break;
        else {
            free(out);
            return NULL;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (char)((acc >> bits) & 0xff);
        }
    }
    out[o] = '\0';
    return out;
}
