#include "log.h"

#include <juice/juice.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static volatile int g_level = RERINKU_LOG_LEVEL_INFO;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static rerinku_log_cb g_handler;
static void *g_user;

static const char *level_name(int level) {
    switch (level) {
    case RERINKU_LOG_LEVEL_ERROR: return "ERR";
    case RERINKU_LOG_LEVEL_WARN:  return "WRN";
    case RERINKU_LOG_LEVEL_INFO:  return "INF";
    default:                return "DBG";
    }
}

/* The lock also serialises the default printer, so lines from the SDK's
 * threads never interleave, and a handler swap never races a line in flight. */
static void emit(int level, const char *tag, const char *msg) {
    pthread_mutex_lock(&g_mu);
    if (g_handler) {
        g_handler(g_user, (rerinku_log_level_t)level, tag, msg);
    } else {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char ts[16];
        strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
        FILE *out = level <= RERINKU_LOG_LEVEL_WARN ? stderr : stdout;
        fprintf(out, "%s %s %-7s | %s\n", ts, level_name(level), tag, msg);
        fflush(out);
    }
    pthread_mutex_unlock(&g_mu);
}

bool rk_log_enabled(int level) { return level <= g_level; }

void rk_log_write(int level, const char *tag, const char *fmt, ...) {
    if (!rk_log_enabled(level))
        return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emit(level, tag, msg);
}

/* libjuice (ICE) logs through us so the application sees one log stream. */
static void juice_log(juice_log_level_t level, const char *message) {
    int ours = RERINKU_LOG_LEVEL_DEBUG;
    if (level >= JUICE_LOG_LEVEL_ERROR)
        ours = RERINKU_LOG_LEVEL_ERROR;
    else if (level == JUICE_LOG_LEVEL_WARN)
        ours = RERINKU_LOG_LEVEL_WARN;
    if (rk_log_enabled(ours))
        emit(ours, "ice", message);
}

static void apply_juice_level(int level) {
    juice_set_log_handler(juice_log);
    /* libjuice's info level is chatty; only debug runs see it. */
    juice_set_log_level(level >= RERINKU_LOG_LEVEL_DEBUG ? JUICE_LOG_LEVEL_INFO
                                                   : JUICE_LOG_LEVEL_WARN);
}

static pthread_once_t g_juice_once = PTHREAD_ONCE_INIT;
static void juice_init_once(void) { apply_juice_level(g_level); }

void rk_log_init(void) { pthread_once(&g_juice_once, juice_init_once); }

void rerinku_set_log_callback(rerinku_log_cb fn, void *user) {
    pthread_mutex_lock(&g_mu);
    g_handler = fn;
    g_user = fn ? user : NULL;
    pthread_mutex_unlock(&g_mu);
}

void rerinku_set_log_level(rerinku_log_level_t level) {
    if (level < RERINKU_LOG_LEVEL_ERROR)
        level = RERINKU_LOG_LEVEL_ERROR;
    if (level > RERINKU_LOG_LEVEL_DEBUG)
        level = RERINKU_LOG_LEVEL_DEBUG;
    g_level = level;
    rk_log_init();
    apply_juice_level(level);
}
