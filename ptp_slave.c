/*
 * PTP timeReceiver (IEEE 1588-2008), szoftveres idobelyegzessel.
 *
 * Ket mod:
 *   PTP_MODE_INTERNAL  sajat ptp4l folyamatot inditunk (-S szoftveres idobelyegzes,
 *                      igy a ptp4l kozvetlenul a rendszerorat [CLOCK_REALTIME] hangolja)
 *   PTP_MODE_EXTERNAL  mar futo ptp4l szolgaltatast hasznalunk; az allapotot
 *                      percenkent tobbszor a "pmc" (management message, A.2.3) kerdezi le
 *
 * AES67 A melleklet (media profile) vevooldali beallitasai:
 *   domainNumber (SDP-bol vagy kezzel), announceReceiptTimeout = 3,
 *   logMinDelayReqInterval = 0, delay request-response (E2E), UDP/IPv4,
 *   timeReceiver-only (clockClass 255), DSCP: EF (46) az ora-uzenetekre (6.2, 1. tabl.)
 *
 * A media ora (5. fejezet) a TAI idoskalan fut: 1970-01-01 00:00:00 TAI-kor 0.
 */
#define _GNU_SOURCE
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#ifndef CLOCK_TAI
#define CLOCK_TAI 11
#endif

static FILE     *g_out = NULL;
static pid_t     g_pid = -1;
static pthread_t g_thread;
static volatile int  g_running = 0;
static int           g_mode = PTP_MODE_INTERNAL;
static int           g_domain = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static long     g_offset = 0;
static int      g_state_ok = 0;           /* s2 (internal) / gmPresent (external) */
static struct timespec g_last_update = {0, 0};
static char     g_gmid[32] = "";
static long     g_lock_thr_ns = 2000000L;
static int      g_tai_offset = 37;        /* TAI - UTC (2017 ota 37 s) */
static int      g_verbose = 0;

void aes67_set_rt_priority(int prio, const char *who)
{
    struct sched_param sp = { .sched_priority = prio };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        static int warned = 0;
        if (!warned++)
            fprintf(stderr, "Megjegyzes: valos ideju prioritas nem allithato (%s). "
                            "Rootkent vagy rtprio limittel futtatva stabilabb.\n", who);
    }
}

int aes67_thread_create(pthread_t *t, void *(*fn)(void *), void *arg)
{
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    int rc = pthread_create(t, NULL, fn, arg);     /* az uj szal orokli a maszkot */
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return rc;
}

void aes67_child_prepare(void)
{
    /* csak async-signal-safe hivasok: fork utan, exec elott futunk */
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3U, ~0U, 0U) == 0) return;
#endif
    for (int fd = 3; fd < 1024; fd++) close(fd);
}

static void update(long off, int ok)
{
    pthread_mutex_lock(&g_lock);
    g_offset = off;
    g_state_ok = ok;
    clock_gettime(CLOCK_MONOTONIC, &g_last_update);
    pthread_mutex_unlock(&g_lock);
}

/* "001b21.fffe.123456" -> "00-1B-21-FF-FE-12-34-56" */
static void set_gmid(const char *s)
{
    char hex[17]; int n = 0;
    for (; *s && n < 16; s++) {
        if (*s == '.' || *s == '-' || *s == ':') continue;
        if (!isxdigit((unsigned char)*s)) break;
        hex[n++] = (char)toupper((unsigned char)*s);
    }
    if (n != 16) return;
    char out[32]; int o = 0;
    for (int i = 0; i < 16; i += 2) {
        if (i) out[o++] = '-';
        out[o++] = hex[i]; out[o++] = hex[i + 1];
    }
    out[o] = 0;
    pthread_mutex_lock(&g_lock);
    memcpy(g_gmid, out, (size_t)o + 1);
    pthread_mutex_unlock(&g_lock);
}

/* ---------------------------------------------------------- */
/* Belso mod: ptp4l -m kimenetenek feldolgozasa                */

static int is_noteworthy(const char *l)
{
    static const char *keys[] = { "FAULTY", "failed", "error", "Error", "No such",
                                  "timed out", "to SLAVE", "to TIME_RECEIVER",
                                  "to LISTENING", "to UNCALIBRATED", "not supported",
                                  "unknown option", "Operation not permitted",
                                  "nem indithato", NULL };
    for (int i = 0; keys[i]; i++) if (strstr(l, keys[i])) return 1;
    return 0;
}

/* A port allapota a ptp4l naplojabol. A SLAVE (ujabb nevvel TIME_RECEIVER) allapot azt
 * jelenti, hogy a szervo lockolt; minden mas allapot (LISTENING, UNCALIBRATED, FAULTY,
 * MASTER) nem. */
static int g_port_slave = 0;

static void parse_port_state(const char *l)
{
    const char *p = strstr(l, " to ");
    if (!p || !strstr(l, "port ")) return;
    p += 4;
    if (!strncmp(p, "SLAVE", 5) || !strncmp(p, "TIME_RECEIVER", 13) || !strncmp(p, "CLIENT", 6))
        g_port_slave = 1;
    else if (strstr(l, " on "))            /* LISTENING, UNCALIBRATED, FAULTY, MASTER, ... */
        g_port_slave = 0;
}

static void *internal_thread(void *arg)
{
    (void)arg;
    char line[512];
    while (g_running && g_out && fgets(line, sizeof(line), g_out)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        int show = g_verbose;

        /* 1) mintankenti sor (ha a Sync ritkabb, mint a summary_interval):
         *    "... master offset -42 s2 freq -12345 path delay 6789"
         * 2) osszegzo sor (gyakori Sync eseten, pl. AES67 logSyncInterval = -3):
         *    "... rms 176564 max 278133 freq +3587 +/- 0 delay 90839 +/- 0" */
        char *p;
        long off, mx; int st;
        if ((p = strstr(line, " offset ")) && sscanf(p + 8, "%ld s%d", &off, &st) == 2) {
            update(off, st == 2 && g_port_slave);
        } else if ((p = strstr(line, " rms ")) && sscanf(p + 5, "%ld max %ld", &off, &mx) == 2) {
            update(off, g_port_slave);
        } else {
            parse_port_state(line);
            if ((p = strstr(line, "best master clock ")) ||
                (p = strstr(line, "best timeTransmitter clock ")))
                set_gmid(strrchr(p, ' ') + 1);
            if (strstr(line, " to ") && strstr(line, "port ")) {
                pthread_mutex_lock(&g_lock);
                g_state_ok = g_state_ok && g_port_slave;
                pthread_mutex_unlock(&g_lock);
            }
            show = show || is_noteworthy(line);
        }
        if (show) fprintf(stderr, "ptp4l: %s\n", line);
    }
    update(0, 0);
    return NULL;
}

static int start_internal(const char *iface)
{
    char dom[40], dscp_e[32], dscp_g[32];
    snprintf(dom, sizeof(dom), "--domainNumber=%d", g_domain);
    snprintf(dscp_e, sizeof(dscp_e), "--dscp_event=46");
    snprintf(dscp_g, sizeof(dscp_g), "--dscp_general=46");

    char *argv[32]; int n = 0;
    argv[n++] = "ptp4l";
    argv[n++] = "-S";                    /* szoftveres idobelyegzes -> CLOCK_REALTIME */
    argv[n++] = "-s";                    /* timeReceiver-only */
    argv[n++] = "-4";                    /* UDP/IPv4 (4.0, A.1) */
    argv[n++] = "-E";                    /* delay request-response (A.2.3) */
    argv[n++] = "-m";
    argv[n++] = "-i"; argv[n++] = (char *)iface;
    argv[n++] = dom;
    argv[n++] = "--announceReceiptTimeout=3";
    argv[n++] = "--logMinDelayReqInterval=0";
    argv[n++] = dscp_e;
    argv[n++] = dscp_g;
    /* Szoftveres idobelyegzesnel (~100 us zaj) a linreg szervo sokkal gyorsabban es
     * stabilabban lockol, mint az alapertelmezett PI, amely ket szomszedos mintabol
     * becsul frekvenciat, es zajos mintaknal ujra meg ujra ujraindul.
     * Feluliras: AES67_PTP_SERVO=pi */
    static char servo[40];
    const char *sv = getenv("AES67_PTP_SERVO");
    snprintf(servo, sizeof(servo), "--clock_servo=%s", (sv && *sv) ? sv : "linreg");
    argv[n++] = servo;

    char *extra = NULL;
    const char *e = getenv("AES67_PTP4L_ARGS");      /* pl. "--step_threshold=0.001" */
    if (e && *e) {
        extra = strdup(e);
        char *save = NULL;
        for (char *t = strtok_r(extra, " ", &save); t && n < 30; t = strtok_r(NULL, " ", &save))
            argv[n++] = t;
    }
    argv[n] = NULL;

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) { perror("pipe"); free(extra); return -1; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(pfd[0]); close(pfd[1]); free(extra); return -1; }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(pfd[1], 1); dup2(pfd[1], 2);
        aes67_child_prepare();
        execvp("ptp4l", argv);
        fprintf(stderr, "ptp4l nem indithato: %s (telepitve van a linuxptp?)\n", strerror(errno));
        _exit(127);
    }
    close(pfd[1]);
    free(extra);
    g_out = fdopen(pfd[0], "r");
    if (!g_out) { close(pfd[0]); kill(pid, SIGTERM); waitpid(pid, NULL, 0); return -1; }
    g_pid = pid;
    return 0;
}

/* ---------------------------------------------------------- */
/* Kulso mod: pmc 'GET TIME_STATUS_NP'                          */

/* pmc inditasa popen helyett fork/exec-kel: a gyerek nem orokli a socketeket, az ALSA
 * eszkozt es a munkaszal letiltott jelmaszkjat. */
static FILE *pmc_open(pid_t *pid)
{
    char dom[16];
    snprintf(dom, sizeof(dom), "%d", g_domain);
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) return NULL;
    pid_t p = fork();
    if (p < 0) { close(pfd[0]); close(pfd[1]); return NULL; }
    if (p == 0) {
        dup2(pfd[1], 1); dup2(pfd[1], 2);
        aes67_child_prepare();
        execlp("pmc", "pmc", "-u", "-b", "0", "-d", dom, "GET TIME_STATUS_NP", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    FILE *f = fdopen(pfd[0], "r");
    if (!f) { close(pfd[0]); kill(p, SIGTERM); waitpid(p, NULL, 0); return NULL; }
    *pid = p;
    return f;
}

static void *external_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        pid_t pmc_pid = -1;
        FILE *f = pmc_open(&pmc_pid);
        long off = 0; int have_off = 0, gm_present = 0;
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char *p;
                if ((p = strstr(line, "master_offset")) ||
                    (p = strstr(line, "timeTransmitter_offset"))) {
                    p = strpbrk(p, " \t");
                    if (p) { off = strtol(p, NULL, 10); have_off = 1; }
                } else if ((p = strstr(line, "gmPresent"))) {
                    gm_present = strstr(p, "true") != NULL;
                } else if ((p = strstr(line, "gmIdentity"))) {
                    p = strpbrk(p, " \t");
                    while (p && isspace((unsigned char)*p)) p++;
                    if (p) set_gmid(p);
                }
            }
            fclose(f);
            waitpid(pmc_pid, NULL, 0);
        }
        if (have_off) update(off, gm_present);
        for (int i = 0; i < 10 && g_running; i++) usleep(100000);
    }
    return NULL;
}

/* ---------------------------------------------------------- */

int ptp_slave_start(const char *iface, int domain, int mode)
{
    if (g_running) return -1;
    if (!iface || !*iface || strlen(iface) >= 16) return -1;
    for (const char *c = iface; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.' && *c != '@')
            return -1;

    const char *e = getenv("AES67_TAI_OFFSET");
    if (e && *e) g_tai_offset = atoi(e);

    g_domain = (domain >= 0 && domain <= 127) ? domain : 0;   /* A.2.2 tartomany */
    g_mode = mode;
    pthread_mutex_lock(&g_lock);
    g_gmid[0] = 0; g_state_ok = 0; g_offset = 0;
    g_last_update.tv_sec = 0;
    pthread_mutex_unlock(&g_lock);

    if (mode == PTP_MODE_INTERNAL && start_internal(iface) != 0) return -1;

    g_running = 1;
    if (aes67_thread_create(&g_thread,
                            mode == PTP_MODE_INTERNAL ? internal_thread : external_thread, NULL) != 0) {
        g_running = 0;
        if (g_pid > 0) { kill(g_pid, SIGTERM); waitpid(g_pid, NULL, 0); g_pid = -1; }
        if (g_out) { fclose(g_out); g_out = NULL; }
        return -1;
    }

    if (mode == PTP_MODE_INTERNAL) {
        /* ha a ptp4l azonnal kilep (rossz interface, mar fut egy masik), jelezzuk */
        usleep(400000);
        int st;
        if (waitpid(g_pid, &st, WNOHANG) == g_pid) {
            g_pid = -1;
            fprintf(stderr, "ptp4l azonnal kilepett. Rossz interface, hianyzo jogosultsag, "
                            "vagy mar fut egy ptp4l (akkor: -x kapcsolo).\n");
            ptp_slave_stop();
            return -1;
        }
    }
    return 0;
}

void ptp_slave_stop(void)
{
    if (!g_running && g_pid <= 0) return;
    g_running = 0;
    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        waitpid(g_pid, NULL, 0);
        g_pid = -1;
    }
    pthread_join(g_thread, NULL);
    if (g_out) { fclose(g_out); g_out = NULL; }
    update(0, 0);
}

void ptp_slave_set_lock_threshold(long ns) { if (ns > 0) g_lock_thr_ns = ns; }
void ptp_slave_set_verbose(int on) { g_verbose = on; }

bool ptp_slave_is_locked(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&g_lock);
    long age = now.tv_sec - g_last_update.tv_sec;
    bool ok = g_last_update.tv_sec != 0 && age < 4 && g_state_ok &&
              labs(g_offset) < g_lock_thr_ns;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

long ptp_slave_get_offset_ns(void)
{
    pthread_mutex_lock(&g_lock);
    long v = g_offset;
    pthread_mutex_unlock(&g_lock);
    return v;
}

int ptp_slave_get_gmid(char *out, size_t n)
{
    int rc = -1;
    pthread_mutex_lock(&g_lock);
    if (g_gmid[0] && n > strlen(g_gmid)) { strcpy(out, g_gmid); rc = 0; }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/*
 * TAI ido. Ha a kernel TAI offsetje be van allitva (ptp4l/phc2sys megteszi),
 * a CLOCK_TAI helyes. Ha 0 (CLOCK_TAI == CLOCK_REALTIME), a TAI-UTC
 * kulonbseget mi adjuk hozza (AES67_TAI_OFFSET, alapertelmezetten 37 s).
 */
int ptp_slave_get_tai(struct timespec *ts)
{
    if (!ts) return -1;
    struct timespec rt;
    if (clock_gettime(CLOCK_TAI, ts) != 0) {
        if (clock_gettime(CLOCK_REALTIME, ts) != 0) return -1;
        ts->tv_sec += g_tai_offset;
        return 0;
    }
    clock_gettime(CLOCK_REALTIME, &rt);
    long d = (long)(ts->tv_sec - rt.tv_sec);
    if (d > -2 && d < 2) ts->tv_sec += g_tai_offset;
    return 0;
}

uint64_t ptp_media_clock_now(uint32_t rate)
{
    struct timespec ts;
    if (ptp_slave_get_tai(&ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * rate + (uint64_t)ts.tv_nsec * rate / 1000000000ULL;
}

uint64_t raw_media_clock_now(uint32_t rate)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * rate + (uint64_t)ts.tv_nsec * rate / 1000000000ULL;
}
