/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 SlakkyStar
 * Copyright (C) 2026 Claude (Anthropic)
 */

#include "df_pcc_relay.h"

static int (*g_log_write)(int, const char *, const char *);

static void log_init(void)
{
    void *h = dlopen("liblog.so", RTLD_NOW);
    if (h) g_log_write = (int (*)(int, const char *, const char *))dlsym(h, "__android_log_write");
    if (!g_log_write) fprintf(stderr, TAG ": liblog.so not usable, logging to stderr\n");
}

static void logmsg(int prio, const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log_write) {
        g_log_write(prio, TAG, buf);
    } else {
        static const char pc[] = {'?', '?', 'V', 'D', 'I', 'W', 'E'};
        fprintf(stderr, "%c/" TAG ": %s\n", pc[prio], buf);
    }
}

static int g_strength = DEFAULT_STRENGTH;
static int g_strength_explicit = 0;
static int g_verbose;

static volatile sig_atomic_t g_sig_stop;
static volatile sig_atomic_t g_sig_no;
static volatile sig_atomic_t g_logcat_pid;

static int g_lastmode = -1;
static int g_first_logged;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static struct color    g_pending;
static int             g_has_pending;
static int             g_quit;

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR && !g_sig_stop) ;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int parse_long(const char *s, long *out) {
    char *end;
    if (!*s) return 0;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end) return 0;
    *out = v;
    return 1;
}

static int prop_get(const char *name, char *val) {
#ifdef __ANDROID__
    return __system_property_get(name, val);
#else
    const char *e = getenv("DF_PCC_TEST_PROP");
    (void)name;
    if (!e) { val[0] = 0; return 0; }
    snprintf(val, 92, "%s", e);
    return (int)strlen(val);
#endif
}

static void micro(char *dst, size_t n, long long v) {
    snprintf(dst, n, "%lld.%06lld", v / 1000000, v % 1000000);
}

static void identity_matrix(struct color *o) {
    for (int i = 0; i < 16; i++) {
        if (i == 15) snprintf(o->c[i], sizeof o->c[i], "1");
        else if (i % 5 == 0) snprintf(o->c[i], sizeof o->c[i], "1.000000");
        else snprintf(o->c[i], sizeof o->c[i], "0");
    }
}

static void eye_color(long val, int strength, struct color *o) {
    identity_matrix(o);
    if (val <= 0) return;

    int dynamic_strength = strength;
    if (!g_strength_explicit) {
        if (val < PAPER_MODE_MIN) return;
        if (val >= PAPER_MODE_MAX) {
            dynamic_strength = 100;
        } else {
            dynamic_strength = (int)(((val - PAPER_MODE_MIN) * 100) / (PAPER_MODE_MAX - PAPER_MODE_MIN));
        }
    }

    long long e = 58000LL * dynamic_strength / 100;
    long long g = 921569 - 4973LL * e / 1000;
    long long b = 901961 - 10126LL * e / 1000;
    
    micro(o->c[5], sizeof o->c[5], g);
    micro(o->c[10], sizeof o->c[10], b);
}

static pid_t spawn_child(const char *path, char *const argv[], int out_fd, int pdeath) {
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid != 0) return pid;

    if (pdeath) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(1);
    }
    signal(SIGPIPE, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);

    int nul = open("/dev/null", O_RDWR);
    if (nul < 0) _exit(126);
    dup2(nul, 0);
    dup2(nul, 2);
    dup2(out_fd >= 0 ? out_fd : nul, 1);
    execv(path, argv);
    _exit(127);
}

static int reap(pid_t pid, int *status) {
    for (;;) {
        pid_t r = waitpid(pid, status, 0);
        if (r == pid) return 0;
        if (r < 0 && errno == EINTR) {
            if (g_sig_stop) kill(pid, SIGKILL);
            continue;
        }
        return -1;
    }
}

static int run_capture(const char *path, char *const argv[], char *out, size_t outsz, int *status) {
    int p[2];
    out[0] = 0;
    if (pipe2(p, O_CLOEXEC) < 0) { LOGE("pipe2: %s", strerror(errno)); return -1; }
    pid_t pid = spawn_child(path, argv, p[1], 0);
    close(p[1]);
    if (pid < 0) {
        LOGE("fork (%s): %s", path, strerror(errno));
        close(p[0]);
        return -1;
    }
    size_t len = 0;
    char tmp[256];
    for (;;) {
        ssize_t n = read(p[0], tmp, sizeof tmp);
        if (n < 0 && errno == EINTR) { if (g_sig_stop) kill(pid, SIGKILL); continue; }
        if (n <= 0) break;
        if (len + 1 < outsz) {
            size_t c = (size_t)n < outsz - 1 - len ? (size_t)n : outsz - 1 - len;
            memcpy(out + len, tmp, c);
            len += c;
            out[len] = 0;
        }
    }
    close(p[0]);
    return reap(pid, status);
}

static const char *stale_reason(pid_t pid) {
    char path[48], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;

    const char *a0 = buf;
    size_t l0 = strlen(a0);
    const char *a1 = (l0 + 1 < (size_t)n) ? buf + l0 + 1 : "";
    const char *b0 = base_name(a0), *b1 = base_name(a1);

    if (!strcmp(b0, "df_pcc_relay")) return "df_pcc_relay binary";
    if (!strcmp(b0, "df_pcc_relay.sh") || (!strcmp(b1, "df_pcc_relay.sh") && (!strcmp(b0, "sh") || !strcmp(b0, "mksh") || !strcmp(b0, "bash")))) return "df_pcc_relay.sh script";
    if (!strcmp(b0, "logcat")) {
        for (const char *p = buf; p < buf + n; p += strlen(p) + 1)
            if (strstr(p, "DisplayFeatureManagerService:D")) return "displayfeature logcat reader";
    }
    return NULL;
}

static int kill_stale(void) {
    DIR *d = opendir("/proc");
    if (!d) { LOGE("single-instance: opendir /proc: %s", strerror(errno)); return -1; }
    int killed = 0;
    pid_t self = getpid();
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char *end;
        long pid = strtol(de->d_name, &end, 10);
        if (*end || pid <= 0 || (pid_t)pid == self) continue;
        const char *why = stale_reason((pid_t)pid);
        if (!why) continue;
        if (kill((pid_t)pid, SIGKILL) == 0) {
            killed++;
            LOGI("single-instance: killed stale pid=%ld (%s)", pid, why);
        } else {
            LOGW("single-instance: cannot kill pid=%ld (%s): %s", pid, why, strerror(errno));
        }
    }
    closedir(d);
    return killed;
}

static int single_instance(void) {
    int killed = kill_stale();
    LOGI("single-instance: stale processes killed: %d", killed < 0 ? 0 : killed);

    int fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOGW("single-instance: cannot open %s: %s (continuing without flock)", LOCK_PATH, strerror(errno));
        return 0;
    }
    for (int i = 0; i < 30; i++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            char pidbuf[24];
            int l = snprintf(pidbuf, sizeof pidbuf, "%d\n", (int)getpid());
            if (ftruncate(fd, 0) == 0 && write(fd, pidbuf, (size_t)l) < 0) { }
            LOGI("single-instance: lock %s acquired (try %d)", LOCK_PATH, i + 1);
            return 0;
        }
        if (errno != EWOULDBLOCK && errno != EINTR) {
            LOGW("single-instance: flock: %s (continuing without lock)", strerror(errno));
            return 0;
        }
        sleep_ms(100);
    }
    LOGE("single-instance: %s is held by another live instance, exiting", LOCK_PATH);
    close(fd);
    return -1;
}

static int sf_found(const char *out) {
    return strstr(out, "found") && !strstr(out, "not found");
}

static int wait_for_sf(void) {
    char *const argv[] = { "service", "check", "SurfaceFlinger", NULL };
    double t0 = now_ms(), last_log = 0;
    int tries = 0, logged = 0;
    for (;;) {
        if (g_sig_stop) return -1;
        char out[256];
        int st = -1;
        int rc = run_capture(SERVICE_BIN, argv, out, sizeof out, &st);
        tries++;
        if (rc == 0 && sf_found(out)) {
            LOGI("SurfaceFlinger ready (checks=%d, waited %.1f s)", tries, (now_ms() - t0) / 1000.0);
            return 0;
        }
        double t = now_ms();
        if (!logged) {
            LOGI("waiting for SurfaceFlinger ('service check' every %d ms)", SF_POLL_MS);
            logged = 1;
            last_log = t;
        } else if (t - last_log >= SF_LOG_EVERY_S * 1000.0) {
            char *nl = strchr(out, '\n');
            if (nl) *nl = 0;
            LOGI("still waiting for SurfaceFlinger (checks=%d, %.0f s, last: %s, exit=%d)",
                 tries, (t - t0) / 1000.0, out[0] ? out : "<no output>",
                 rc == 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1);
            last_log = t;
        }
        sleep_ms(SF_POLL_MS);
    }
}

static void do_apply(const struct color *c) {
    char *argv[3 + 1 + 2 + 32 + 1];
    int n = 0;
    argv[n++] = "service";
    argv[n++] = "call";
    argv[n++] = "SurfaceFlinger";
    argv[n++] = "1015";
    argv[n++] = "i32";
    argv[n++] = "1";
    for (int i = 0; i < 16; i++) {
        argv[n++] = "f";
        argv[n++] = (char *)c->c[i];
    }
    argv[n] = NULL;

    if (g_verbose) {
        char dbg[512] = {0};
        for(int i=6; i<38; i+=2) {
            if(argv[i+1]) {
                strncat(dbg, argv[i+1], sizeof(dbg)-strlen(dbg)-1);
                strncat(dbg, " ", sizeof(dbg)-strlen(dbg)-1);
            }
        }
        LOGI("MATRIX SENT: %s", dbg);
    }

    char out[256] = {0};
    int st = 0;
    int rc = run_capture(SERVICE_BIN, argv, out, sizeof out, &st);

    if (rc == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        char *nl = strchr(out, '\n');
        if (nl) *nl = 0;
        if (g_verbose) LOGI("APPLY OK: %s", out[0] ? out : "no output");
    } else {
        LOGE("APPLY FAILED rc=%d", rc);
    }
}

static void *applier_main(void *arg) {
    (void)arg;
    struct color cur;
    
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_quit && !g_has_pending)
            pthread_cond_wait(&g_cv, &g_mu);
            
        if (g_quit) { pthread_mutex_unlock(&g_mu); break; }
        
        cur = g_pending;
        g_has_pending = 0;
        pthread_mutex_unlock(&g_mu);

        for (int i = 0; i < 100; i++) {
            if (g_quit || g_has_pending) break;
            do_apply(&cur);
            sleep_ms(20);
        }
    }
    return NULL;
}

static void publish(const struct color *c) {
    pthread_mutex_lock(&g_mu);
    g_pending = *c;
    g_has_pending = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void note_first(const char *what) {
    if (!g_first_logged) {
        g_first_logged = 1;
        LOGI("first event accepted: %s", what);
    }
}

static int token(const char *s, char *dst, size_t n) {
    size_t i = 0;
    while (s[i] && s[i] != ' ' && s[i] != '\t') {
        if (i + 1 >= n) { dst[0] = 0; return 0; }
        dst[i] = s[i];
        i++;
    }
    dst[i] = 0;
    return 1;
}

static void handle_effect(const char *mp, const char *vp) {
    char m[32], v[32];
    int m_ok = token(mp, m, sizeof m);
    int v_ok = token(vp, v, sizeof v);
    if (g_verbose) LOGD("IN setScreenEffect mode=%s value=%s", m_ok ? m : "?", v_ok ? v : "?");
    if (!m_ok) return;

    if (!strcmp(m, "3")) {
        long val;
        if (!v_ok || !parse_long(v, &val)) return;
        
        if (val <= 0 || (!g_strength_explicit && val < PAPER_MODE_MIN)) {
            if (g_lastmode == 3) g_lastmode = 0;
            struct color c;
            eye_color(0, g_strength, &c);
            note_first("setScreenEffect mode=3 (OFF)");
            publish(&c);
            return;
        }
        g_lastmode = 3;
        struct color c;
        eye_color(val, g_strength, &c);
        note_first("setScreenEffect mode=3");
        publish(&c);
    } else if (!strcmp(m, "0") || !strcmp(m, "1") || !strcmp(m, "2") || !strcmp(m, "23")) {
        g_lastmode = atoi(m);
    }
}

static int parse_matrix(const char *s, struct color *out) {
    double vals[16];
    int count = 0;
    const char *p = s;
    
    while (*p && count < 16) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        char *end;
        errno = 0;
        double d = strtod(p, &end);
        if (end == p || errno || !isfinite(d)) return 0;
        vals[count++] = d;
        p = end;
    }
    
    if (count == 3) {
        identity_matrix(out);
        snprintf(out->c[0], sizeof out->c[0], "%.6f", vals[0]);
        snprintf(out->c[5], sizeof out->c[5], "%.6f", vals[1]);
        snprintf(out->c[10], sizeof out->c[10], "%.6f", vals[2]);
        return 1;
    } else if (count == 9) {
        identity_matrix(out);
        snprintf(out->c[0], sizeof out->c[0], "%.6f", vals[0]);
        snprintf(out->c[1], sizeof out->c[1], "%.6f", vals[1]);
        snprintf(out->c[2], sizeof out->c[2], "%.6f", vals[2]);
        snprintf(out->c[4], sizeof out->c[4], "%.6f", vals[3]);
        snprintf(out->c[5], sizeof out->c[5], "%.6f", vals[4]);
        snprintf(out->c[6], sizeof out->c[6], "%.6f", vals[5]);
        snprintf(out->c[8], sizeof out->c[8], "%.6f", vals[6]);
        snprintf(out->c[9], sizeof out->c[9], "%.6f", vals[7]);
        snprintf(out->c[10], sizeof out->c[10], "%.6f", vals[8]);
        return 1;
    } else if (count == 16) {
        for (int i = 0; i < 16; i++) {
            if (vals[i] == 0.0) snprintf(out->c[i], sizeof out->c[i], "0");
            else if (vals[i] == 1.0) snprintf(out->c[i], sizeof out->c[i], "1");
            else snprintf(out->c[i], sizeof out->c[i], "%.6f", vals[i]);
        }
        return 1;
    }
    return 0;
}

static void handle_pcc(const char *rest) {
    if (g_verbose) LOGD("IN PccData value %s", rest);
    struct color c;
    if (!parse_matrix(rest, &c)) return;
    note_first("PccData");
    publish(&c);
}

static void handle_line(const char *line) {
    if (strstr(line, "displayfeature") || strstr(line, "DisplayFeatureManager") || strstr(line, "Pcc")) {
        if (g_verbose) LOGI("RAW LOG: %s", line);
    }
    const char *p = strstr(line, "setScreenEffect displayId=");
    if (p) {
        const char *mp = strstr(p, "mode=");
        const char *vp = mp ? strstr(mp, "value=") : NULL;
        if (mp && vp) { handle_effect(mp + 5, vp + 6); return; }
    }
    p = strstr(line, "PccData value");
    if (p) {
        p += strlen("PccData value");
        while (*p == ' ') p++;
        handle_pcc(p);
    }
}

static void pump(int fd) {
    static char buf[LINE_MAX_LEN];
    size_t len = 0;
    int discard = 0;
    for (;;) {
        ssize_t n = read(fd, buf + len, sizeof buf - 1 - len);
        if (n < 0) {
            if (errno == EINTR) { if (g_sig_stop) return; continue; }
            return;
        }
        if (n == 0) return;
        len += (size_t)n;

        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', (size_t)(buf + len - start))) != NULL) {
            *nl = 0;
            if (nl > start && nl[-1] == '\r') nl[-1] = 0;
            if (!discard) handle_line(start);
            discard = 0;
            start = nl + 1;
        }
        size_t rem = (size_t)(buf + len - start);
        if (discard || rem >= sizeof buf - 1) {
            discard = 1;
            rem = 0;
        }
        memmove(buf, start, rem);
        len = rem;
    }
}

static void reader_loop(void) {
    char *const argv[] = { "logcat", "-b", "main", "-b", "system", "-v", "raw", "-T", "1",
                           "displayfeature:D", "DisplayFeatureManagerService:D", "*:S", NULL };
    int starts = 0;
    while (!g_sig_stop) {
        int p[2];
        if (pipe2(p, O_CLOEXEC) < 0) {
            sleep_ms(RESTART_DELAY_MS);
            continue;
        }
        pid_t pid = spawn_child(LOGCAT_BIN, argv, p[1], 1);
        close(p[1]);
        if (pid < 0) {
            close(p[0]);
            sleep_ms(RESTART_DELAY_MS);
            continue;
        }
        g_logcat_pid = pid;
        if (g_sig_stop) kill(pid, SIGKILL);
        LOGI("logcat reader started pid=%d (start #%d)", (int)pid, ++starts);

        pump(p[0]);
        close(p[0]);

        g_logcat_pid = 0;
        kill(pid, SIGKILL);
        int st = 0;
        if (reap(pid, &st) == 0) {
            if (g_sig_stop) {
                LOGI("logcat reader pid=%d stopped", (int)pid);
            }
        }
        if (g_sig_stop) break;
        sleep_ms(RESTART_DELAY_MS);
    }
}

static void on_signal(int sig) {
    g_sig_no = sig;
    g_sig_stop = 1;
    pid_t p = (pid_t)g_logcat_pid;
    if (p > 0) kill(p, SIGKILL);
}

static void sanitize_stdio(void) {
    for (int fd = 0; fd <= 2; fd++)
        if (fcntl(fd, F_GETFD) < 0) {
            int n = open("/dev/null", O_RDWR);
            (void)n;
        }
}

static void usage(void) {
    fprintf(stderr, "usage: " TAG " [--strength N] [-v]\n");
}

int main(int argc, char **argv) {
    sanitize_stdio();
    log_init();

    const char *strength_arg = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-v")) {
            g_verbose = 1;
        } else if (!strcmp(a, "--strength")) {
            if (i + 1 < argc) strength_arg = argv[++i];
        } else if (!strncmp(a, "--strength=", 11)) {
            strength_arg = a + 11;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        }
    }

    char ctx[128] = "?";
    int cfd = open("/proc/self/attr/current", O_RDONLY | O_CLOEXEC);
    if (cfd >= 0) {
        ssize_t n = read(cfd, ctx, sizeof ctx - 1);
        ctx[n > 0 ? n : 0] = 0;
        close(cfd);
    }
    LOGI("start " VERSION " pid=%d verbose=%d", (int)getpid(), g_verbose);

    const char *src = "default";
    long sv;
    if (strength_arg) {
        if (parse_long(strength_arg, &sv) && sv >= 0 && sv <= 100) {
            g_strength = (int)sv;
            src = "argument";
            g_strength_explicit = 1;
        }
    }
    if (!strcmp(src, "default")) {
        char pv[128] = "";
        if (prop_get(PROP_STRENGTH, pv) > 0) {
            if (parse_long(pv, &sv) && sv >= 0 && sv <= 100) {
                g_strength = (int)sv;
                src = "property";
                g_strength_explicit = 1;
            }
        }
    }
    {
        struct color on;
        eye_color(PAPER_MODE_MAX, g_strength, &on);
        LOGI("STRENGTH=%d -> eye care ON = %s %s %s", g_strength, on.c[0], on.c[5], on.c[10]);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (single_instance() < 0) return 1;
    if (wait_for_sf() < 0) return 0;

    pthread_t th;
    pthread_attr_t attr;
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGTERM);
    sigaddset(&blk, SIGINT);
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_sigmask(SIG_BLOCK, &blk, &old);
    int err = pthread_create(&th, &attr, applier_main, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&attr);
    if (err) return 1;

    reader_loop();

    if (g_sig_no)
        LOGI("received signal %d (%s), shutting down", (int)g_sig_no, strsignal((int)g_sig_no));
    pthread_mutex_lock(&g_mu);
    g_quit = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(th, NULL);
    return 0;
}
