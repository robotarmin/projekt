/*
 * PTP timeReceiver (IEEE 1588-2008, AES67 4. es A melleklet) es a media ora (5.).
 *
 * Sajat ptp4l (alapeset):
 *   ptp4l -4 -E -s: UDP/IPv4, delay request-response, csak timeReceiver (clockClass 255),
 *   announceReceiptTimeout 3, logMinDelayReqInterval 0, DSCP EF (46) (6.2, 1. tablazat).
 *   - Ha az interfesz hardveres idobelyegzest tud (ethtool -T: hardware-transmit/receive
 *     es PTP Hardware Clock), ptp4l -H a kartya orajat (PHC) hangolja, es a media orat
 *     kozvetlenul a PHC-bol olvassuk: a rendszerorahoz nem nyulunk, phc2sys nem kell.
 *   - Kulonben ptp4l -S (linreg szervo, ez tuori jobban a szoftveres zajt) a rendszerorat
 *     hangolja, a media ora a CLOCK_REALTIME + TAI-UTC (kernel offset, vagy 37 s).
 * Kulso ptp4l (-x): pmc-vel kerdezzuk (management, A.2.3); a media ora a rendszerora
 *   (hardveres ptp4l mellett ehhez phc2sys kell).
 */
#define _GNU_SOURCE
#include "aes67.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <sys/wait.h>
#include <net/if.h>
#include <linux/ethtool.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>

#define FD_TO_CLOCKID(fd) ((clockid_t)((((unsigned)~(fd)) << 3) | 3))

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static volatile int g_run;
static pid_t  g_pid = -1;
static FILE  *g_out;
static int    g_phc_fd = -1;                 /* hardveres mod: a PHC */
static int    g_have_thread;
static int    g_domain, g_verbose, g_slave, g_ok;
static long   g_lock_ns, g_offset;
static time_t g_updated;
static char   g_gmid[24];
static int    g_tai_default = 37;            /* TAI - UTC, ha a kernel nem tudja */

int thread_start(pthread_t *t, void *(*fn)(void *), void *arg, int prio)
{
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);   /* mlockall mellett ne foglaljon 8 MB-ot */
    int rc = EPERM;
    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&at, SCHED_FIFO);
        pthread_attr_setschedparam(&at, &sp);
        if ((rc = pthread_create(t, &at, fn, arg)) == EPERM)
            fprintf(stderr, "Megjegyzes: valos ideju prioritas nem allithato (rootkent stabilabb).\n");
        pthread_attr_setinheritsched(&at, PTHREAD_INHERIT_SCHED);
    }
    if (rc == EPERM) rc = pthread_create(t, &at, fn, arg);
    pthread_attr_destroy(&at);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return rc;
}

/* Gyerekfolyamat inditasa; a stdout/stderr kimenete *out. A fajlleirok CLOEXEC-ek. */
static pid_t spawn(char *argv[], FILE **out)
{
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execvp(argv[0], argv);
        char path[64];                       /* Debianon /usr/sbin nincs a felhasznalo PATH-jan */
        snprintf(path, sizeof(path), "/usr/sbin/%s", argv[0]);
        execv(path, argv);
        fprintf(stderr, "%s nem indithato: %s (linuxptp telepitve?)\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(pfd[1]);
    if (pid < 0 || !(*out = fdopen(pfd[0], "r"))) {
        close(pfd[0]);
        if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); }
        return -1;
    }
    return pid;
}

static void set_state(long off, int ok)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    pthread_mutex_lock(&g_lock);
    g_offset = off;
    g_ok = ok;
    g_updated = t.tv_sec;
    pthread_mutex_unlock(&g_lock);
}

/* "00e04c.fffe.680f01" -> "00-E0-4C-FF-FE-68-0F-01" (az SDP formaja) */
static void set_gmid(const char *s)
{
    char hex[17], out[24];
    int n = 0;
    for (; *s && n < 16; s++)
        if (strchr("0123456789abcdefABCDEF", *s)) hex[n++] = *s;
        else if (!strchr(".-:", *s)) break;
    if (n != 16) return;
    for (int i = 0; i < 8; i++)
        snprintf(out + i * 3, 4, "%c%c%s", hex[2 * i], hex[2 * i + 1], i < 7 ? "-" : "");
    for (char *q = out; *q; q++) if (*q >= 'a') *q -= 32;
    pthread_mutex_lock(&g_lock);
    memcpy(g_gmid, out, sizeof(out));
    pthread_mutex_unlock(&g_lock);
}

/* ptp4l -m kimenete: "master offset -42 s2 freq ...", gyakori Sync-nel osszegzes:
 * "rms 176 max 278 freq ...", allapot: "port 1 (eth0): UNCALIBRATED to SLAVE on ...". */
static void *ptp4l_thread(void *arg)
{
    (void)arg;
    static const char *notable[] = { " to ", "FAULT", "fail", "rror", "timed out",
                                     "not supported", "No such", "denied", NULL };
    char line[512], *p;
    long off, mx;
    int st;
    while (fgets(line, sizeof(line), g_out)) {
        line[strcspn(line, "\n")] = 0;
        int show = g_verbose;
        if ((p = strstr(line, " offset ")) && sscanf(p + 8, "%ld s%d", &off, &st) == 2)
            set_state(off, st == 2 && g_slave);
        else if ((p = strstr(line, " rms ")) && sscanf(p + 5, "%ld max %ld", &off, &mx) == 2)
            set_state(off, g_slave);
        else {
            if ((p = strstr(line, "port 1")) && (p = strstr(p, " to "))) {
                g_slave = !strncmp(p + 4, "SLAVE", 5) || !strncmp(p + 4, "TIME_RECEIVER", 13) ||
                          !strncmp(p + 4, "CLIENT", 6);
                if (!g_slave) set_state(0, 0);
            }
            if ((p = strstr(line, "best master clock ")) ||
                (p = strstr(line, "best timeTransmitter clock ")))
                set_gmid(strrchr(p, ' ') + 1);
            for (int i = 0; notable[i] && !show; i++) show = strstr(line, notable[i]) != NULL;
        }
        if (show) fprintf(stderr, "ptp4l: %s\n", line);
    }
    set_state(0, 0);
    return NULL;
}

/* Kulso ptp4l: masodpercenkent "pmc GET TIME_STATUS_NP" (rootkent a /var/run/ptp4l,
 * egyebkent a csak olvashato /var/run/ptp4lro kezelo-socketen). */
static void *pmc_thread(void *arg)
{
    (void)arg;
    char dom[16], uds[64], line[256], *p;
    snprintf(dom, sizeof(dom), "%d", g_domain);
    snprintf(uds, sizeof(uds), "/tmp/aes67rx-pmc.%d", (int)getpid());
    char *argv[] = { "pmc", "-u", "-b", "0", "-d", dom, "-i", uds, "-s",
                     geteuid() ? "/var/run/ptp4lro" : "/var/run/ptp4l",
                     "GET TIME_STATUS_NP", NULL };
    while (g_run) {
        FILE *f;
        pid_t pid = spawn(argv, &f);
        long off = 0;
        int have = 0, present = 0;
        if (pid > 0) {
            while (fgets(line, sizeof(line), f)) {
                if ((p = strstr(line, "master_offset")) || (p = strstr(line, "timeTransmitter_offset")))
                    have = sscanf(strpbrk(p, " \t"), "%ld", &off) == 1;
                else if ((p = strstr(line, "gmPresent")))
                    present = strstr(p, "true") != NULL;
                else if ((p = strstr(line, "gmIdentity")))
                    set_gmid(p + 10 + strspn(p + 10, " \t"));
            }
            fclose(f);
            waitpid(pid, NULL, 0);
        }
        if (have) set_state(off, present);
        for (int i = 0; i < 10 && g_run; i++) usleep(100000);
    }
    unlink(uds);
    return NULL;
}

/* A hardveres idobelyegzeshez tartozo PTP ora (/dev/ptpN) sorszama, vagy -1. */
static int phc_index(const char *iface)
{
    struct ethtool_ts_info ti = { .cmd = ETHTOOL_GET_TS_INFO };
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
    ifr.ifr_data = (char *)&ti;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int rc = fd < 0 ? -1 : ioctl(fd, SIOCETHTOOL, &ifr);
    if (fd >= 0) close(fd);
    const unsigned need = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE |
                          SOF_TIMESTAMPING_RAW_HARDWARE;
    return rc == 0 && (ti.so_timestamping & need) == need ? ti.phc_index : -1;
}

int ptp_start(const char *iface, int domain, int external, long lock_ns, int verbose)
{
    g_domain = domain;
    g_lock_ns = lock_ns;
    g_verbose = verbose;
    g_slave = 0;
    g_gmid[0] = 0;
    set_state(0, 0);
    const char *e = getenv("AES67_TAI_OFFSET");
    if (e && *e) g_tai_default = atoi(e);

    g_run = 1;
    if (external) {
        printf("PTP: kulso ptp4l (pmc), domain %d\n", domain);
        g_have_thread = thread_start(&g_thread, pmc_thread, NULL, 0) == 0;
        if (!g_have_thread) g_run = 0;
        return g_have_thread ? 0 : -1;
    }

    int phc = phc_index(iface);
    if (phc >= 0) {
        char dev[32];
        snprintf(dev, sizeof(dev), "/dev/ptp%d", phc);
        g_phc_fd = open(dev, O_RDONLY | O_CLOEXEC);
        if (g_phc_fd < 0) fprintf(stderr, "PTP: %s nem nyithato meg, szoftveres mod\n", dev);
    }
    printf("PTP: ptp4l %s idobelyegzessel, %s, domain %d\n",
           g_phc_fd >= 0 ? "hardveres" : "szoftveres", iface, domain);

    char dom[32], *argv[40];
    int n = 0;
    snprintf(dom, sizeof(dom), "--domainNumber=%d", domain);
    char *fixed[] = { "ptp4l", "-i", (char *)iface, "-4", "-E", "-s", "-m", "-q",
                      g_phc_fd >= 0 ? "-H" : "-S", dom, "--announceReceiptTimeout=3",
                      "--logMinDelayReqInterval=0", "--dscp_event=46", "--dscp_general=46",
                      g_phc_fd >= 0 ? NULL : "--clock_servo=linreg" };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]) && fixed[i]; i++) argv[n++] = fixed[i];
    char *extra = strdup(getenv("AES67_PTP4L_ARGS") ? getenv("AES67_PTP4L_ARGS") : "");
    for (char *s, *t = strtok_r(extra, " ", &s); t && n < 39; t = strtok_r(NULL, " ", &s))
        argv[n++] = t;
    argv[n] = NULL;

    g_pid = spawn(argv, &g_out);
    free(extra);
    g_have_thread = g_pid > 0 && thread_start(&g_thread, ptp4l_thread, NULL, 0) == 0;
    if (!g_have_thread) {
        fprintf(stderr, "A ptp4l nem indithato.\n");
        g_run = 0;
        ptp_stop();
        return -1;
    }
    usleep(400000);
    if (waitpid(g_pid, NULL, WNOHANG) == g_pid) {
        g_pid = -1;
        fprintf(stderr, "A ptp4l azonnal kilepett: rossz interfesz, nincs root jog, vagy mar "
                        "fut egy ptp4l (akkor: -x).\n");
        ptp_stop();
        return -1;
    }
    return 0;
}

void ptp_stop(void)
{
    g_run = 0;
    if (g_pid > 0) { kill(g_pid, SIGTERM); waitpid(g_pid, NULL, 0); g_pid = -1; }
    if (g_have_thread) pthread_join(g_thread, NULL);
    g_have_thread = 0;
    if (g_out) { fclose(g_out); g_out = NULL; }
    if (g_phc_fd >= 0) { close(g_phc_fd); g_phc_fd = -1; }
    set_state(0, 0);
}

int ptp_locked(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    pthread_mutex_lock(&g_lock);
    int ok = g_ok && t.tv_sec - g_updated < 4 && labs(g_offset) < g_lock_ns;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

long ptp_offset(void)
{
    pthread_mutex_lock(&g_lock);
    long v = g_offset;
    pthread_mutex_unlock(&g_lock);
    return v;
}

int ptp_gmid(char *out, size_t n)
{
    pthread_mutex_lock(&g_lock);
    int rc = g_gmid[0] && n > strlen(g_gmid) ? 0 : -1;
    if (!rc) strcpy(out, g_gmid);
    pthread_mutex_unlock(&g_lock);
    return rc;
}

static uint64_t samples(const struct timespec *ts, uint32_t rate)
{
    return (uint64_t)ts->tv_sec * rate + (uint64_t)ts->tv_nsec * rate / 1000000000u;
}

uint64_t media_now(uint32_t rate)
{
    struct timespec ts;
    if (g_phc_fd >= 0 && clock_gettime(FD_TO_CLOCKID(g_phc_fd), &ts) == 0)
        return samples(&ts, rate);           /* a PHC a PTP idoskalan (TAI) jar */
    struct timex tx = { 0 };
    adjtimex(&tx);                           /* csak lekerdezes: kernel TAI-UTC */
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += tx.tai > 0 ? tx.tai : g_tai_default;
    return samples(&ts, rate);
}

uint64_t raw_now(uint32_t rate)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return samples(&ts, rate);
}
