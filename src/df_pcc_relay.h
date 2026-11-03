/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 SlakkyStar
 * Copyright (C) 2026 Claude (Anthropic)
 */

#ifndef DF_PCC_RELAY_H
#define DF_PCC_RELAY_H

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define TAG                "df_pcc_relay"
#define VERSION            "v10-spam-mode"
#define PROP_STRENGTH      "persist.df_pcc.strength"

#define PAPER_MODE_MIN     50
#define PAPER_MODE_MAX     255

#ifndef SERVICE_BIN
#define SERVICE_BIN        "/system/bin/service"
#endif
#ifndef LOGCAT_BIN
#define LOGCAT_BIN         "/system/bin/logcat"
#endif
#ifndef LOCK_PATH
#define LOCK_PATH          "/dev/df_pcc_relay.lock"
#endif

#define DEFAULT_STRENGTH   50
#define THROTTLE_MS        30
#define RESTART_DELAY_MS   1500
#define SF_POLL_MS         1000
#define SF_LOG_EVERY_S     5
#define LINE_MAX_LEN       8192

enum { L_DEBUG = 3, L_INFO = 4, L_WARN = 5, L_ERROR = 6 };

struct color { char c[16][24]; };

static void logmsg(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGD(...) logmsg(L_DEBUG, __VA_ARGS__)
#define LOGI(...) logmsg(L_INFO,  __VA_ARGS__)
#define LOGW(...) logmsg(L_WARN,  __VA_ARGS__)
#define LOGE(...) logmsg(L_ERROR, __VA_ARGS__)

#endif
