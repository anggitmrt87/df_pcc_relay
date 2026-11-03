/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 SlakkyStar
 * Copyright (C) 2026 Claude (Anthropic)
 */

/*
 * Relays lime displayfeature colour values from logcat to SurfaceFlinger
 */

#include "df_pcc_relay.h"

static int (*g_log_write)(int, const char *, const char *);

static void log_init(void)
{
    void *h = dlopen("liblog.so", RTLD_NOW);
    if (h)
        g_log_write = (int (*)(int, const char *, const char *))dlsym(h, "__android_log_write");
    if (!g_log_write)
        fprintf(stderr, TAG ": liblog.so not usable, logging to stderr\n");
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
static int g_strength_explicit = 0; /* Flag: 1 jika user memberikan --strength */
static int g_verbose;

static volatile sig_atomic_t g_sig_stop;
static volatile sig_atomic_t g_sig_no;
static volatile sig_atomic_t g_logcat_pid;

static int g_lastmode = -1; /* LASTMODE from the script */
static int g_first_logged;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static struct color    g_pending;
static int             g_has_pending;
static int             g_quit;

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR && !g_sig_stop)
        ;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int parse_long(const char *s, long *out)
{
    char *end;
    if (!*s) return 0;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end) return 0;
    *out = v;
    return 1;
}

static int prop_get(const char *name, char *val)
{
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

static void micro(char *dst, size_t n, long long v)
{
    snprintf(dst, n, "%lld.%06lld", v / 1000000, v % 1000000);
}

/* Same integer maths as eye_triple() in the shell script */
static void eye_color(long val, int strength, struct color *o)
{
    /* Jika value 0, berarti OFF */
    if (val <= 0) {
        snprintf(o->c[0], sizeof o->c[0], "1.000000");
        snprintf(o->c[1], sizeof o->c[1], "1.000000");
        snprintf(o->c[2], sizeof o->c[2], "1.000000");
        return;
    }

    int dynamic_strength = strength;
    
    /* Jika user TIDAK memberikan --strength secara eksplisit, gunakan nilai UI (50-255) */
    if (!g_strength_explicit) {
        if (val < PAPER_MODE_MIN) {
            snprintf(o->c[0], sizeof o->c[0], "1.000000");
            snprintf(o->c[1], sizeof o->c[1], "1.000000");
            snprintf(o->c[2], sizeof o->c[2], "1.000000");
            return;
        }
        if (val >= PAPER_MODE_MAX) {
            dynamic_strength = 100;
        } else {
            /* Mapping dari (50..255) ke (0..100) */
            dynamic_strength = (int)(((val - PAPER_MODE_MIN) * 100) / (PAPER_MODE_MAX - PAPER_MODE_MIN));
        }
    }

    long long e = 58000LL * dynamic_strength / 100;
    long long g = 921569 - 4973LL * e / 1000;
    long long b = 901961 - 10126LL * e / 1000;
    
    snprintf(o->c[0], sizeof o->c[0], "1.000000");
    micro(o->c[1], sizeof o->c[1], g);
    micro(o->c[2], sizeof o->c[2], b);
}

static int color_equal(const struct color *a, const struct color *b)
{
    return !strcmp(a->c[0], b->c[0]) && !strcmp(a->c[1], b->c[1]) && !strcmp(a->c[2], b->c[2]);
}

static pid_t spawn_child(const char *path, char *const argv[], int out_fd, int pdeath)
{
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid != 0)
        return pid;

    /* child: async-signal-safe calls only from here */
    if (pdeath) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent)
            _exit(1);
    }
    signal(SIGPIPE, SIG_DFL); /* SIG_IGN survives exec */
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL); /* applier thread masks sigs */

    int nul = open("/dev/null", O_RDWR);
    if (nul < 0) _exit(126);
    dup2(nul, 0);
    dup2(nul, 2);
    dup2(out_fd >= 0 ? out_fd : nul, 1);
    execv(path, argv);
    _exit(127);
}

static int reap(pid_t pid, int *status)
{
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

static int run_capture(const char *path, char *const argv[], char *out, size_t outsz, int *status)
{
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

static const char *stale_reason(pid_t pid)
{
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

    if (!strcmp(b0, "df_pcc_relay"))
        return "df_pcc_relay binary";
    if (!strcmp(b0, "df_pcc_relay.sh") ||
        (!strcmp(b1, "df_pcc_relay.sh") && (!strcmp(b0, "sh") || !strcmp(b0, "mksh") || !strcmp(b0, "bash"))))
        return "df_pcc_relay.sh script";
    if (!strcmp(b0, "logcat")) {
        for (const char *p = buf; p < buf + n; p += strlen(p) + 1)
            if (strstr(p, "DisplayFeatureManagerService:D"))
                return "displayfeature logcat reader";
    }
    return NULL;
}

static int kill_stale(void)
{
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

static int single_instance(void)
{
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

static int sf_found(const char *out)
{
    return strstr(out, "found") && !strstr(out, "not found");
}

static int wait_for_sf(void)
{
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

static void do_apply(const struct color *c)
{
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
        argv[n++] = i == 0  ? (char *)c->c[0]
                  : i == 5  ? (char *)c->c[1]
                  : i == 10 ? (char *)c->c[2]
                  : i == 15 ? "1" : "0";
    }
    argv[n] = NULL;

    double t0 = now_ms();
    pid_t pid = spawn_child(SERVICE_BIN, argv, -1, 0);
    if (pid < 0) {
        LOGE("APPLY %s %s %s: fork failed: %s", c->c[0], c->c[1], c->c[2], strerror(errno));
        return;
    }
    int st = 0;
    if (reap(pid, &st) < 0) {
        LOGE("APPLY %s %s %s: waitpid(%d) failed: %s", c->c[0], c->c[1], c->c[2], (int)pid, strerror(errno));
        return;
    }
    double dt = now_ms() - t0;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
        LOGI("APPLY %s %s %s rc=0 t=%.1fms", c->c[0], c->c[1], c->c[2], dt);
    else if (WIFEXITED(st))
        LOGE("APPLY %s %s %s rc=%d%s t=%.1fms", c->c[0], c->c[1], c->c[2], WEXITSTATUS(st),
             WEXITSTATUS(st) == 127 ? " (exec failed)" : "", dt);
    else
        LOGE("APPLY %s %s %s killed by signal %d t=%.1fms", c->c[0], c->c[1], c->c[2],
             WTERMSIG(st), dt);
}

static void *applier_main(void *arg)
{
    (void)arg;
    struct color last, cur;
    int have_last = 0;
    memset(&last, 0, sizeof last);
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_quit && !(g_has_pending && (!have_last || !color_equal(&g_pending, &last))))
            pthread_cond_wait(&g_cv, &g_mu);
        if (g_quit) { pthread_mutex_unlock(&g_mu); break; }
        cur = g_pending;
        pthread_mutex_unlock(&g_mu);

        do_apply(&cur);
        last = cur;
        have_last = 1;
        sleep_ms(THROTTLE_MS);
    }
    return NULL;
}

static void publish(const struct color *c)
{
    pthread_mutex_lock(&g_mu);
    g_pending = *c;
    g_has_pending = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void note_first(const char *what)
{
    if (!g_first_logged) {
        g_first_logged = 1;
        LOGI("first event accepted: %s", what);
    }
}

static int token(const char *s, char *dst, size_t n)
{
    size_t i = 0;
    while (s[i] && s[i] != ' ' && s[i] != '\t') {
        if (i + 1 >= n) { dst[0] = 0; return 0; }
        dst[i] = s[i];
        i++;
    }
    dst[i] = 0;
    return 1;
}

static void handle_effect(const char *mp, const char *vp)
{
    char m[32], v[32];
    int m_ok = token(mp, m, sizeof m);
    int v_ok = token(vp, v, sizeof v);
    if (g_verbose) LOGD("IN setScreenEffect mode=%s value=%s", m_ok ? m : "?", v_ok ? v : "?");
    if (!m_ok) { LOGE("parse: bad mode token in setScreenEffect, line skipped"); return; }

    if (!strcmp(m, "3")) {
        long val;
        if (!v_ok || !parse_long(v, &val)) {
            LOGE("parse: mode=3 value is not a number ('%s'), line skipped", v_ok ? v : "<too long>");
            return;
        }
        
        /* BUGFIX: Jika eye care dimatikan (0 atau di bawah minimum), lepas gembok g_lastmode */
        if (val <= 0 || (!g_strength_explicit && val < PAPER_MODE_MIN)) {
            if (g_lastmode == 3) {
                g_lastmode = 0;
            }
            struct color c;
            eye_color(0, g_strength, &c);
            char what[96];
            snprintf(what, sizeof what, "setScreenEffect mode=3 value=%ld (OFF)", val);
            note_first(what);
            if (g_verbose)
                LOGD("eye care OFF -> %s %s %s", c.c[0], c.c[1], c.c[2]);
            publish(&c);
            return;
        }

        g_lastmode = 3;
        struct color c;
        eye_color(val, g_strength, &c);
        char what[96];
        snprintf(what, sizeof what, "setScreenEffect mode=3 value=%ld", val);
        note_first(what);
        if (g_verbose)
            LOGD("eye care ON val=%ld -> %s %s %s", val, c.c[0], c.c[1], c.c[2]);
        publish(&c);
    } else if (!strcmp(m, "0") || !strcmp(m, "1") || !strcmp(m, "2") || !strcmp(m, "23")) {
        g_lastmode = atoi(m);
        char what[96];
        snprintf(what, sizeof what, "setScreenEffect mode=%s", m);
        note_first(what);
    }
}

static int parse_triple(const char *s, struct color *out)
{
    const char *p = s;
    for (int i = 0; i < 3; i++) {
        while (*p == ' ') p++;
        char *end;
        errno = 0;
        double d = strtod(p, &end);
        if (end == p || errno || !isfinite(d)) return 0;
        int n = snprintf(out->c[i], sizeof out->c[i], "%.6f", d);
        if (n < 0 || (size_t)n >= sizeof out->c[i]) return 0;
        p = end;
        if (i < 2) {
            while (*p == ' ') p++;
            if (*p != ',') return 0;
            p++;
        }
    }
    return 1;
}

static void handle_pcc(const char *rest)
{
    if (g_verbose) LOGD("IN PccData value %s (LASTMODE=%d)", rest, g_lastmode);
    if (g_lastmode == 3) return;
    struct color c;
    if (!parse_triple(rest, &c)) {
        LOGE("parse: bad PccData triple: '%s', line skipped", rest);
        return;
    }
    char what[96];
    snprintf(what, sizeof what, "PccData %s %s %s", c.c[0], c.c[1], c.c[2]);
    note_first(what);
    publish(&c);
}

static void handle_line(const char *line)
{
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

static void pump(int fd)
{
    static char buf[LINE_MAX_LEN];
    size_t len = 0;
    int discard = 0;
    for (;;) {
        ssize_t n = read(fd, buf + len, sizeof buf - 1 - len);
        if (n < 0) {
            if (errno == EINTR) { if (g_sig_stop) return; continue; }
            LOGE("read(logcat pipe): %s", strerror(errno));
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
            if (!discard) LOGW("logcat line longer than %d bytes dropped", LINE_MAX_LEN);
            discard = 1;
            rem = 0;
        }
        memmove(buf, start, rem);
        len = rem;
    }
}

static void reader_loop(void)
{
    char *const argv[] = { "logcat", "-b", "main", "-b", "system", "-v", "raw", "-T", "1",
                           "displayfeature:D", "DisplayFeatureManagerService:D", "*:S", NULL };
    int starts = 0;
    while (!g_sig_stop) {
        int p[2];
        if (pipe2(p, O_CLOEXEC) < 0) {
            LOGE("pipe2: %s", strerror(errno));
            sleep_ms(RESTART_DELAY_MS);
            continue;
        }
        pid_t pid = spawn_child(LOGCAT_BIN, argv, p[1], 1);
        close(p[1]);
        if (pid < 0) {
            LOGE("fork (logcat): %s", strerror(errno));
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
            } else if (WIFEXITED(st)) {
                LOGW("logcat reader pid=%d exited with status %d / pipe closed", (int)pid, WEXITSTATUS(st));
            } else {
                LOGW("logcat reader pid=%d terminated by signal %d / pipe closed", (int)pid, WTERMSIG(st));
            }
        } else {
            LOGE("waitpid(logcat %d): %s", (int)pid, strerror(errno));
        }
        if (g_sig_stop) break;
        LOGW("restarting logcat reader in %d ms", RESTART_DELAY_MS);
        sleep_ms(RESTART_DELAY_MS);
    }
}

static void on_signal(int sig)
{
    g_sig_no = sig;
    g_sig_stop = 1;
    pid_t p = (pid_t)g_logcat_pid;
    if (p > 0) kill(p, SIGKILL);
}

static void sanitize_stdio(void)
{
    for (int fd = 0; fd <= 2; fd++)
        if (fcntl(fd, F_GETFD) < 0) {
            int n = open("/dev/null", O_RDWR);
            (void)n;
        }
}

static void usage(void)
{
    fprintf(stderr, "usage: " TAG " [--strength N] [-v]\n"
                    "  --strength N   eye care strength 0..100 (default %d, or property " PROP_STRENGTH ")\n"
                    "  -v             verbose: log every incoming setScreenEffect / PccData\n",
            DEFAULT_STRENGTH);
}

int main(int argc, char **argv)
{
    sanitize_stdio();
    log_init();

    const char *strength_arg = NULL;
    int bad_arg = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-v")) {
            g_verbose = 1;
        } else if (!strcmp(a, "--strength")) {
            if (i + 1 < argc) strength_arg = argv[++i];
            else { LOGE("argument --strength needs a value"); bad_arg = 1; }
        } else if (!strncmp(a, "--strength=", 11)) {
            strength_arg = a + 11;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else {
            LOGW("unknown argument '%s' ignored", a);
        }
    }

    char ctx[128] = "?";
    int cfd = open("/proc/self/attr/current", O_RDONLY | O_CLOEXEC);
    if (cfd >= 0) {
        ssize_t n = read(cfd, ctx, sizeof ctx - 1);
        ctx[n > 0 ? n : 0] = 0;
        close(cfd);
    }
    LOGI("start " VERSION " pid=%d uid=%d ppid=%d verbose=%d ctx=%s",
         (int)getpid(), (int)getuid(), (int)getppid(), g_verbose, ctx);

    const char *src = "default";
    long sv;
    if (strength_arg) {
        if (parse_long(strength_arg, &sv) && sv >= 0 && sv <= 100) {
            g_strength = (int)sv;
            src = "argument";
            g_strength_explicit = 1; /* Kunci nilai dari argumen */
        } else {
            LOGE("invalid --strength '%s' (need integer 0..100), ignored", strength_arg);
        }
    } else if (bad_arg) {
        /* already logged */
    }
    if (!strcmp(src, "default")) {
        char pv[128] = "";
        if (prop_get(PROP_STRENGTH, pv) > 0) {
            if (parse_long(pv, &sv) && sv >= 0 && sv <= 100) {
                g_strength = (int)sv;
                src = "property";
                g_strength_explicit = 1; /* Kunci nilai dari property */
            } else {
                LOGE("invalid property " PROP_STRENGTH "='%s' (need integer 0..100), ignored", pv);
            }
        }
    }
    {
        struct color on;
        eye_color(PAPER_MODE_MAX, g_strength, &on);
        LOGI("STRENGTH=%d (source: %s) -> eye care ON = %s %s %s", g_strength, src,
             on.c[0], on.c[1], on.c[2]);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (single_instance() < 0)
        return 1;

    if (wait_for_sf() < 0) {
        LOGI("interrupted by signal %d while waiting for SurfaceFlinger, exiting", (int)g_sig_no);
        return 0;
    }

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
    if (err) {
        LOGE("pthread_create: %s", strerror(err));
        return 1;
    }
    LOGI("applier thread started (throttle %d ms)", THROTTLE_MS);

    reader_loop();

    if (g_sig_no)
        LOGI("received signal %d (%s), shutting down", (int)g_sig_no, strsignal((int)g_sig_no));
    pthread_mutex_lock(&g_mu);
    g_quit = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_join(th, NULL);
    LOGI("exit");
    return 0;
}
