/*
 * camdvd - read the recorded sectors of an unfinalized (or finalized) DVD
 * straight off the drive, ddrescue style, with a resumable map file.
 *
 *   camdvd list
 *   camdvd info  [-d DRIVE]
 *   camdvd rip   [-d DRIVE] -o IMAGE [options]
 *   camdvd eject [-d DRIVE]
 *
 * DRIVE is a drive index from 'list', a BSD name like disk4, or
 * sim:<config> for the test simulator.
 */
#include "drive.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SECTOR 2048u
#define VERSION "1.0"

/* ------------------------------------------------------------------ util */

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static double now_s(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "camdvd: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static const char *profile_name(uint16_t p) {
    switch (p) {
    case 0x0000: return "no disc / unknown";
    case 0x0008: return "CD-ROM";
    case 0x0009: return "CD-R";
    case 0x000A: return "CD-RW";
    case 0x0010: return "DVD-ROM";
    case 0x0011: return "DVD-R (sequential)";
    case 0x0012: return "DVD-RAM";
    case 0x0013: return "DVD-RW (restricted overwrite)";
    case 0x0014: return "DVD-RW (sequential)";
    case 0x0015: return "DVD-R DL (sequential)";
    case 0x0016: return "DVD-R DL (jump)";
    case 0x001A: return "DVD+RW";
    case 0x001B: return "DVD+R";
    case 0x002B: return "DVD+R DL";
    case 0x0040: return "BD-ROM";
    case 0x0041: return "BD-R";
    case 0x0043: return "BD-RE";
    default:     return "other";
    }
}

static const char *sense_text(const cmd_result_t *r) {
    static char buf[160];
    if (r->result == CMD_TRANSPORT) {
        snprintf(buf, sizeof buf, "no response from drive (error 0x%08x)", (unsigned)r->os_error);
        return buf;
    }
    const char *t = "unknown";
    if (r->sk == 0x02 && r->asc == 0x3A) t = "no disc in drive";
    else if (r->sk == 0x02 && r->asc == 0x04) t = "drive not ready";
    else if (r->sk == 0x02 && r->asc == 0x30) t = "disc format not readable by this drive";
    else if (r->sk == 0x03 && r->asc == 0x11) t = "unrecovered read error";
    else if (r->sk == 0x03 && r->asc == 0x02) t = "no seek complete";
    else if (r->sk == 0x03 && r->asc == 0x31) t = "medium format corrupted";
    else if (r->sk == 0x03 && r->asc == 0x57) t = "unable to recover table of contents";
    else if (r->sk == 0x03) t = "medium error";
    else if (r->sk == 0x04) t = "drive hardware error";
    else if (r->sk == 0x05 && r->asc == 0x21) t = "address out of range (unrecorded area)";
    else if (r->sk == 0x05 && r->asc == 0x64) t = "illegal mode for this track";
    else if (r->sk == 0x05 && r->asc == 0x20) t = "command not supported";
    else if (r->sk == 0x05 && r->asc == 0x24) t = "invalid field in command";
    else if (r->sk == 0x05) t = "illegal request";
    else if (r->sk == 0x06) t = "unit attention (disc changed or drive reset)";
    snprintf(buf, sizeof buf, "%s [%X/%02X/%02X]", t, r->sk, r->asc, r->ascq);
    return buf;
}

/* --------------------------------------------------------- drive helpers */

static drive_t *open_drive(const char *spec) {
    char err[512] = {0};
    drive_t *d = NULL;
    if (spec && strncmp(spec, "sim:", 4) == 0) {
        d = sim_open(spec + 4, err, sizeof err);
    } else {
#ifdef __APPLE__
        d = mac_open(spec, err, sizeof err);
#else
        snprintf(err, sizeof err, "only sim:<config> drives are supported on this platform");
#endif
    }
    if (!d) die("%s", err);
    return d;
}

static cmd_result_t simple_cmd(drive_t *d, const uint8_t *cdb, int len, void *buf, uint32_t n) {
    return d->cmd(d, cdb, len, buf, n, buf && n ? XFER_IN : XFER_NONE, 15000);
}

/* Wait for spin-up. Returns 0 when ready, -1 with *last set otherwise. */
static int wait_ready(drive_t *d, cmd_result_t *last) {
    uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0};
    double t0 = now_s();
    for (;;) {
        cmd_result_t r = simple_cmd(d, cdb, 6, NULL, 0);
        if (r.result == CMD_OK) return 0;
        *last = r;
        if (r.result == CMD_CHECK && r.sk == 0x06) continue;                  /* unit attention */
        if (r.result == CMD_CHECK && r.sk == 0x02 && r.asc == 0x3A) return -1; /* no disc */
        if (now_s() - t0 > 40) return -1;
        sleep_ms(500);
    }
}

typedef struct { uint32_t start, count; } extent_t;

typedef struct {
    char     vendor[64];
    uint16_t profile;
    int      have_disc_info;
    int      disc_status;           /* 0 empty, 1 incomplete, 2 complete, 3 other */
    uint32_t capacity;              /* sectors from READ CAPACITY, 0 if none */
    int      ntracks;
    struct { uint32_t n, start, size, nwa, lra; int blank, rt, nwa_v, lra_v; extent_t ext; } tr[100];
    extent_t ext[100];
    int      next;
} disc_t;

static void add_extent(disc_t *dc, uint32_t start, uint32_t count) {
    if (!count || dc->next >= 100) return;
    dc->ext[dc->next].start = start;
    dc->ext[dc->next].count = count;
    dc->next++;
}

static int cmp_ext(const void *a, const void *b) {
    const extent_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static void normalize_extents(extent_t *e, int *n) {
    qsort(e, (size_t)*n, sizeof *e, cmp_ext);
    int o = 0;
    for (int i = 0; i < *n; i++) {
        if (o && e[i].start <= e[o - 1].start + e[o - 1].count) {
            uint32_t end = e[i].start + e[i].count;
            if (end > e[o - 1].start + e[o - 1].count) e[o - 1].count = end - e[o - 1].start;
        } else {
            e[o++] = e[i];
        }
    }
    *n = o;
}

static int discover(drive_t *d, disc_t *dc, int verbose) {
    memset(dc, 0, sizeof *dc);
    cmd_result_t r;

    uint8_t inq[96] = {0};
    uint8_t c_inq[6] = {0x12, 0, 0, 0, 96, 0};
    r = simple_cmd(d, c_inq, 6, inq, sizeof inq);
    if (r.result == CMD_OK) {
        char v[29]; memcpy(v, inq + 8, 28); v[28] = 0;
        for (int i = 27; i >= 0 && v[i] == ' '; i--) v[i] = 0;
        snprintf(dc->vendor, sizeof dc->vendor, "%s", v);
    }

    cmd_result_t last;
    if (wait_ready(d, &last) != 0) {
        fprintf(stderr, "camdvd: drive is not ready: %s\n", sense_text(&last));
        return -1;
    }

    uint8_t cfg[8] = {0};
    uint8_t c_cfg[10] = {0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0};
    r = simple_cmd(d, c_cfg, 10, cfg, sizeof cfg);
    if (r.result == CMD_OK) dc->profile = (uint16_t)(cfg[6] << 8 | cfg[7]);

    uint8_t cap[8] = {0};
    uint8_t c_cap[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    r = simple_cmd(d, c_cap, 10, cap, sizeof cap);
    if (r.result == CMD_OK && get32(cap) > 0) dc->capacity = get32(cap) + 1;

    uint8_t di[34] = {0};
    uint8_t c_di[10] = {0x51, 0, 0, 0, 0, 0, 0, 0, 34, 0};
    r = simple_cmd(d, c_di, 10, di, sizeof di);
    int first = 0, lastt = 0;
    if (r.result == CMD_OK) {
        dc->have_disc_info = 1;
        dc->disc_status = di[2] & 3;
        first = di[3];
        lastt = (di[11] << 8) | di[6];
        if (first < 1) first = 1;
        if (lastt > 99) lastt = 99;
    } else if (verbose) {
        fprintf(stderr, "note: READ DISC INFORMATION failed: %s\n", sense_text(&r));
    }

    for (int t = first; t >= 1 && t <= lastt && dc->ntracks < 100; t++) {
        uint8_t ti[36] = {0};
        uint8_t c_ti[10] = {0x52, 0x01, 0, 0, 0, 0, 0, 0, 36, 0};
        put32(c_ti + 2, (uint32_t)t);
        r = simple_cmd(d, c_ti, 10, ti, sizeof ti);
        if (r.result != CMD_OK) continue;
        int k = dc->ntracks++;
        dc->tr[k].n = (uint32_t)t;
        dc->tr[k].rt = (ti[6] & 0x80) != 0;
        dc->tr[k].blank = (ti[6] & 0x40) != 0;
        dc->tr[k].nwa_v = (ti[7] & 0x01) != 0;
        dc->tr[k].lra_v = (ti[7] & 0x02) != 0;
        dc->tr[k].start = get32(ti + 8);
        dc->tr[k].nwa = get32(ti + 12);
        dc->tr[k].size = get32(ti + 24);
        dc->tr[k].lra = get32(ti + 28);

        uint32_t s = dc->tr[k].start, cnt = 0;
        if (dc->tr[k].blank) cnt = 0;
        else if (dc->tr[k].lra_v && dc->tr[k].lra >= s) cnt = dc->tr[k].lra - s + 1;
        else if (dc->tr[k].nwa_v && dc->tr[k].nwa > s) cnt = dc->tr[k].nwa - s;
        else if (!dc->tr[k].nwa_v) cnt = dc->tr[k].size;
        dc->tr[k].ext.start = s;
        dc->tr[k].ext.count = cnt;
        add_extent(dc, s, cnt);
    }

    /* DVD-RAM and other rewritable formats are random access: read it all. */
    if ((dc->profile == 0x0012 || dc->profile == 0x0013 || dc->profile == 0x001A ||
         dc->profile == 0x0043 || dc->next == 0) && dc->capacity) {
        dc->next = 0;
        add_extent(dc, 0, dc->capacity);
    }
    normalize_extents(dc->ext, &dc->next);
    return 0;
}

static void print_disc(const disc_t *dc, const char *drive_name) {
    printf("Drive:        %s\n", dc->vendor[0] ? dc->vendor : drive_name);
    printf("Disc type:    %s (profile 0x%04X)\n", profile_name(dc->profile), dc->profile);
    if (dc->have_disc_info) {
        const char *st[] = {"empty", "incomplete (not finalized)", "complete (finalized)", "other"};
        printf("Disc status:  %s\n", st[dc->disc_status & 3]);
    } else {
        printf("Disc status:  unknown (drive did not report it)\n");
    }
    if (dc->capacity) printf("Capacity:     %u sectors\n", dc->capacity);
    else printf("Capacity:     not reported (normal for an unfinalized disc)\n");
    for (int i = 0; i < dc->ntracks; i++) {
        const char *kind = dc->tr[i].blank ? (dc->tr[i].rt ? "reserved, blank" : "blank")
                         : dc->tr[i].nwa_v ? "open (recording stopped here)" : "closed";
        printf("Track %-2u      start %-8u size %-8u %s", dc->tr[i].n, dc->tr[i].start,
               dc->tr[i].size, kind);
        if (dc->tr[i].ext.count)
            printf("  -> recorded %u sectors (%.1f MB)", dc->tr[i].ext.count,
                   dc->tr[i].ext.count * (double)SECTOR / 1e6);
        printf("\n");
    }
    uint64_t total = 0;
    for (int i = 0; i < dc->next; i++) total += dc->ext[i].count;
    printf("To read:      %llu sectors (%.1f MB) in %d area(s)\n", (unsigned long long)total,
           total * (double)SECTOR / 1e6, dc->next);
    if (!total)
        printf("Nothing recorded was found. If the camera showed footage on this disc, try "
               "'camdvd rip --probe', or a different drive.\n");
}

/* ------------------------------------------------------------- the map */

enum { ST_OUT = 0, ST_UNTRIED = '?', ST_GOOD = '+', ST_RETRY = '/', ST_BAD = '-', ST_END = 'e' };

typedef struct {
    char      path[4096];
    uint8_t  *st;           /* status per sector, indexed by LBA */
    uint32_t  limit;        /* array size = highest extent end */
    extent_t  ext[100];
    int       next;
    uint16_t  profile;
    char      drive[128];
} map_t;

static void map_alloc(map_t *m) {
    m->limit = 0;
    for (int i = 0; i < m->next; i++)
        if (m->ext[i].start + m->ext[i].count > m->limit) m->limit = m->ext[i].start + m->ext[i].count;
    m->st = calloc(m->limit ? m->limit : 1, 1);
    if (!m->st) die("out of memory for %u sectors", m->limit);
    for (int i = 0; i < m->next; i++)
        memset(m->st + m->ext[i].start, ST_UNTRIED, m->ext[i].count);
}

static int map_load(map_t *m) {
    FILE *f = fopen(m->path, "r");
    if (!f) return 0;
    char line[512];
    m->next = 0;
    typedef struct { uint32_t s, c; char ch; } run_t;
    size_t nruns = 0, cap = 1024;
    run_t *runs = malloc(cap * sizeof *runs);
    while (fgets(line, sizeof line, f)) {
        unsigned a, b; char ch; unsigned p;
        if (sscanf(line, "extent %u %u", &a, &b) == 2 && m->next < 100) {
            m->ext[m->next].start = a; m->ext[m->next].count = b; m->next++;
        } else if (sscanf(line, "# profile 0x%x", &p) == 1) {
            m->profile = (uint16_t)p;
        } else if (sscanf(line, "run %u %u %c", &a, &b, &ch) == 3) {
            if (nruns == cap) { cap *= 2; runs = realloc(runs, cap * sizeof *runs); }
            runs[nruns].s = a; runs[nruns].c = b; runs[nruns].ch = ch; nruns++;
        }
    }
    fclose(f);
    map_alloc(m);
    for (size_t i = 0; i < nruns; i++) {
        if (runs[i].s >= m->limit) continue;
        uint32_t c = runs[i].c;
        if (runs[i].s + c > m->limit) c = m->limit - runs[i].s;
        char ch = runs[i].ch;
        if (ch != '?' && ch != '+' && ch != '/' && ch != '-' && ch != 'e') ch = '?';
        memset(m->st + runs[i].s, ch, c);
    }
    free(runs);
    return 1;
}

static void map_save(const map_t *m) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", m->path);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "\ncamdvd: cannot write map %s: %s\n", tmp, strerror(errno)); return; }
    fprintf(f, "# camdvd map v1. Statuses: + read, ? not tried, / failed once, - unreadable, e past readable end\n");
    fprintf(f, "# profile 0x%04X %s\n", m->profile, profile_name(m->profile));
    fprintf(f, "# drive %s\n", m->drive);
    for (int i = 0; i < m->next; i++) fprintf(f, "extent %u %u\n", m->ext[i].start, m->ext[i].count);
    for (int i = 0; i < m->next; i++) {
        uint32_t s = m->ext[i].start, e = s + m->ext[i].count;
        uint32_t a = s;
        while (a < e) {
            uint32_t b = a;
            while (b < e && m->st[b] == m->st[a]) b++;
            fprintf(f, "run %u %u %c\n", a, b - a, m->st[a]);
            a = b;
        }
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(tmp, m->path) != 0)
        fprintf(stderr, "\ncamdvd: cannot replace map %s: %s\n", m->path, strerror(errno));
}

typedef struct { uint64_t total, good, untried, retry, bad, end; } counts_t;

static counts_t map_counts(const map_t *m) {
    counts_t c; memset(&c, 0, sizeof c);
    for (int i = 0; i < m->next; i++)
        for (uint32_t l = m->ext[i].start; l < m->ext[i].start + m->ext[i].count; l++) {
            c.total++;
            switch (m->st[l]) {
            case ST_GOOD: c.good++; break;
            case ST_UNTRIED: c.untried++; break;
            case ST_RETRY: c.retry++; break;
            case ST_BAD: c.bad++; break;
            case ST_END: c.end++; break;
            }
        }
    return c;
}

/* ---------------------------------------------------------------- ripping */

typedef struct {
    drive_t *d;
    map_t   *m;
    int      fd;
    uint8_t *buf;
    int      block;
    int      retries;
    int      probe;
    int      transport_streak;
    int      dropped;
    double   last_save, last_print, t_start;
    uint64_t bytes_this_run;
    const char *pass_name;
    uint32_t cur;
    int      tty;
} rip_t;

enum { RD_OK = 0, RD_MEDIA = 1, RD_RANGE = 2, RD_TRANSPORT = -1 };

static int read_sectors(rip_t *R, uint32_t lba, uint32_t cnt, cmd_result_t *out) {
    uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    put32(cdb + 2, lba);
    cdb[7] = (uint8_t)(cnt >> 8); cdb[8] = (uint8_t)cnt;
    int attention = 0, notready = 0;
    for (;;) {
        cmd_result_t r = R->d->cmd(R->d, cdb, 10, R->buf, cnt * SECTOR, XFER_IN, 30000);
        *out = r;
        if (r.result == CMD_OK) {
            R->transport_streak = 0;
            return RD_OK;
        }
        if (r.result == CMD_TRANSPORT) {
            R->transport_streak++;
            if (R->transport_streak >= 4) { R->dropped = 1; return RD_TRANSPORT; }
            sleep_ms(1500);
            continue;
        }
        R->transport_streak = 0;
        if (r.sk == 0x06 && attention++ < 3) continue;
        if (r.sk == 0x02 && r.asc == 0x3A) { R->dropped = 1; return RD_TRANSPORT; }
        if (r.sk == 0x02 && r.asc == 0x04 && notready++ < 40) { sleep_ms(500); continue; }
        if (r.sk == 0x05 && (r.asc == 0x21 || r.asc == 0x64 || r.asc == 0x63)) return RD_RANGE;
        return RD_MEDIA;
    }
}

static void progress(rip_t *R, int force) {
    double t = now_s();
    if (!force && t - R->last_print < (R->tty ? 0.5 : 10.0)) return;
    R->last_print = t;
    counts_t c = map_counts(R->m);
    double pct = c.total ? 100.0 * (double)(c.good + c.bad + c.end) / (double)c.total : 100.0;
    double el = t - R->t_start;
    double mbs = el > 0.5 ? R->bytes_this_run / 1e6 / el : 0;
    fprintf(stderr, "%s[%s] LBA %-8u %5.1f%%  read %.1f MB  unreadable %llu  pending %llu  %.1f MB/s%s",
            R->tty ? "\r" : "", R->pass_name, R->cur, pct, c.good * (double)SECTOR / 1e6,
            (unsigned long long)c.bad, (unsigned long long)(c.untried + c.retry), mbs,
            R->tty ? "   " : "\n");
    fflush(stderr);
}

static void maybe_save(rip_t *R) {
    double t = now_s();
    if (t - R->last_save >= 5.0) { map_save(R->m); R->last_save = t; }
}

static int write_out(rip_t *R, uint32_t lba, uint32_t cnt) {
    off_t off = (off_t)lba * SECTOR;
    size_t len = (size_t)cnt * SECTOR;
    const uint8_t *p = R->buf;
    while (len) {
        ssize_t w = pwrite(R->fd, p, len, off);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; len -= (size_t)w; off += w;
    }
    return 0;
}

static void mark(map_t *m, uint32_t lba, uint32_t cnt, uint8_t st) { memset(m->st + lba, st, cnt); }

/* Block pass over untried sectors. allow_skip enables jumping over damage. */
static int block_pass(rip_t *R, int allow_skip) {
    map_t *m = R->m;
    for (int i = 0; i < m->next && !g_stop; i++) {
        uint32_t s = m->ext[i].start, e = s + m->ext[i].count;
        int streak = 0, range_streak = 0;
        uint32_t lba = s;
        while (lba < e && !g_stop) {
            if (m->st[lba] != ST_UNTRIED) { lba++; continue; }
            uint32_t B = (uint32_t)R->block;
            uint32_t cnt = B - (lba % B);
            if (lba + cnt > e) cnt = e - lba;
            for (uint32_t k = 1; k < cnt; k++)
                if (m->st[lba + k] != ST_UNTRIED) { cnt = k; break; }
            R->cur = lba;
            cmd_result_t r;
            int rc = read_sectors(R, lba, cnt, &r);
            if (rc == RD_TRANSPORT) return -1;
            if (rc == RD_OK) {
                if (write_out(R, lba, cnt) != 0) die("write to image failed: %s", strerror(errno));
                mark(m, lba, cnt, ST_GOOD);
                R->bytes_this_run += (uint64_t)cnt * SECTOR;
                streak = 0; range_streak = 0;
                lba += cnt;
            } else if (rc == RD_RANGE) {
                /* The block may straddle the end of the recording: sort it
                 * out sector by sector in the scrape pass. */
                mark(m, lba, cnt, cnt > 1 ? ST_RETRY : ST_END);
                lba += cnt;
                if (++range_streak >= 4 || R->probe) {
                    /* The drive says nothing is recorded from here on. */
                    for (uint32_t l = lba; l < e; l++)
                        if (m->st[l] == ST_UNTRIED) m->st[l] = ST_END;
                    break;
                }
            } else {
                mark(m, lba, cnt, ST_RETRY);
                lba += cnt;
                range_streak = 0;
                if (allow_skip && ++streak >= 2) {
                    uint32_t skip = B << (streak > 8 ? 8 : streak);
                    if (skip > 4096) skip = 4096;
                    lba += skip;   /* left untried, picked up by the next pass */
                }
            }
            progress(R, 0);
            maybe_save(R);
        }
    }
    return 0;
}

static void set_read_speed(drive_t *d, uint16_t kbs) {
    uint8_t cdb[12] = {0xBB, 0, (uint8_t)(kbs >> 8), (uint8_t)kbs, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
    (void)simple_cmd(d, cdb, 12, NULL, 0);
}

static int scrape_pass(rip_t *R) {
    map_t *m = R->m;
    for (int i = 0; i < m->next && !g_stop; i++) {
        uint32_t s = m->ext[i].start, e = s + m->ext[i].count;
        for (uint32_t lba = s; lba < e && !g_stop; lba++) {
            if (m->st[lba] != ST_RETRY) continue;
            R->cur = lba;
            int done = 0;
            for (int a = 0; a < R->retries && !g_stop; a++) {
                cmd_result_t r;
                int rc = read_sectors(R, lba, 1, &r);
                if (rc == RD_TRANSPORT) return -1;
                if (rc == RD_OK) {
                    if (write_out(R, lba, 1) != 0) die("write to image failed: %s", strerror(errno));
                    m->st[lba] = ST_GOOD;
                    R->bytes_this_run += SECTOR;
                    done = 1;
                    break;
                }
                if (rc == RD_RANGE) { m->st[lba] = ST_END; done = 1; break; }
            }
            if (!done && !g_stop) m->st[lba] = ST_BAD;
            progress(R, 0);
            maybe_save(R);
        }
    }
    return 0;
}

/* A resumed map must belong to the disc in the drive. Re-read one sector
 * that was read before and compare it with the image. */
static int same_disc(rip_t *R) {
    map_t *m = R->m;
    for (int i = 0; i < m->next; i++)
        for (uint32_t l = m->ext[i].start; l < m->ext[i].start + m->ext[i].count; l++) {
            if (m->st[l] != ST_GOOD) continue;
            /* use a sector with content, not padding */
            uint8_t have[SECTOR];
            if (pread(R->fd, have, SECTOR, (off_t)l * SECTOR) != (ssize_t)SECTOR) return 0;
            int nonzero = 0;
            for (unsigned k = 0; k < SECTOR; k++) if (have[k]) { nonzero = 1; break; }
            if (!nonzero) continue;
            cmd_result_t r;
            if (read_sectors(R, l, 1, &r) != RD_OK) continue;   /* try another sector */
            return memcmp(have, R->buf, SECTOR) == 0;
        }
    return 1;   /* nothing read yet, nothing to compare */
}

static void usage(void) {
    fprintf(stderr,
        "camdvd %s - recover video from unfinalized camcorder DVDs\n\n"
        "  camdvd list                       show optical drives\n"
        "  camdvd info  [-d DRIVE]           show disc type, tracks and recorded areas\n"
        "  camdvd rip   [-d DRIVE] -o IMAGE  read recorded sectors into IMAGE (resumable)\n"
        "  camdvd eject [-d DRIVE]\n\n"
        "rip options:\n"
        "  --retries N      single-sector attempts on damaged areas (default 4)\n"
        "  --block N        sectors per read, multiple of 16 recommended (default 16)\n"
        "  --no-scrape      skip the slow sector-by-sector pass\n"
        "  --retry-bad      on resume, try unreadable sectors again (new drive, cleaned disc)\n"
        "  --range A-B      read LBA A..B inclusive instead of what the drive reports (repeatable)\n"
        "  --probe          ignore track info and read forward from LBA 0 until the recording ends\n"
        "  --force          resume even if the disc does not match the existing map\n\n"
        "DRIVE is an index from 'camdvd list' (default 0) or a name like disk4.\n"
        "Exit codes: 0 everything read, 2 finished with unreadable sectors, 3 drive stopped\n"
        "responding, 130 interrupted, 1 other error.\n", VERSION);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    const char *cmd = argv[1];
    const char *drive_spec = "0";
    const char *out = NULL;
    int retries = 4, block = 16, scrape = 1, retry_bad = 0, probe = 0, force = 0;
    extent_t ranges[100]; int nranges = 0;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "-d") || !strcmp(a, "--drive")) && i + 1 < argc) drive_spec = argv[++i];
        else if ((!strcmp(a, "-o") || !strcmp(a, "--output")) && i + 1 < argc) out = argv[++i];
        else if (!strcmp(a, "--retries") && i + 1 < argc) retries = atoi(argv[++i]);
        else if (!strcmp(a, "--block") && i + 1 < argc) block = atoi(argv[++i]);
        else if (!strcmp(a, "--no-scrape")) scrape = 0;
        else if (!strcmp(a, "--retry-bad")) retry_bad = 1;
        else if (!strcmp(a, "--probe")) probe = 1;
        else if (!strcmp(a, "--force")) force = 1;
        else if (!strcmp(a, "--range") && i + 1 < argc && nranges < 100) {
            unsigned x, y;
            if (sscanf(argv[++i], "%u-%u", &x, &y) != 2 || y < x) die("bad --range %s", argv[i]);
            ranges[nranges].start = x; ranges[nranges].count = y - x + 1; nranges++;
        } else usage();
    }
    if (retries < 1) retries = 1;
    if (block < 1 || block > 32) die("--block must be 1..32");

    if (!strcmp(cmd, "list")) {
#ifdef __APPLE__
        return mac_list() > 0 ? 0 : 1;
#else
        printf("(no native drive support on this platform; use -d sim:<config>)\n");
        return 0;
#endif
    }

    if (!strcmp(cmd, "eject")) {
        drive_t *d = open_drive(drive_spec);
        uint8_t allow[6] = {0x1E, 0, 0, 0, 0, 0};
        uint8_t ej[6] = {0x1B, 0, 0, 0, 0x02, 0};
        simple_cmd(d, allow, 6, NULL, 0);
        cmd_result_t r = simple_cmd(d, ej, 6, NULL, 0);
        d->close(d);
        if (r.result != CMD_OK) die("eject failed: %s", sense_text(&r));
        return 0;
    }

    if (!strcmp(cmd, "info")) {
        drive_t *d = open_drive(drive_spec);
        disc_t dc;
        int rc = discover(d, &dc, 1);
        if (rc == 0) print_disc(&dc, d->name);
        d->close(d);
        return rc == 0 ? 0 : 1;
    }

    if (strcmp(cmd, "rip") != 0) usage();
    if (!out) die("rip needs -o IMAGE");

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGHUP, on_sigint);

    drive_t *d = open_drive(drive_spec);
    disc_t dc;
    if (discover(d, &dc, 1) != 0) { d->close(d); return 1; }

    map_t m;
    memset(&m, 0, sizeof m);
    snprintf(m.path, sizeof m.path, "%s.map", out);
    snprintf(m.drive, sizeof m.drive, "%.127s", dc.vendor[0] ? dc.vendor : d->name);
    m.profile = dc.profile;

    int resumed = map_load(&m);
    if (resumed) {
        if (m.profile && dc.profile && m.profile != dc.profile && !force) {
            d->close(d);
            die("%s was made from a %s disc but this is a %s disc. Use a new output name, "
                "or --force if you are sure.", m.path, profile_name(m.profile), profile_name(dc.profile));
        }
        m.profile = dc.profile;
        if (retry_bad)
            for (uint32_t l = 0; l < m.limit; l++)
                if (m.st[l] == ST_BAD || m.st[l] == ST_END) m.st[l] = ST_RETRY;
        /* a newer drive may report more recorded area than the first one did */
        extent_t add[200]; int na = 0;
        for (int i = 0; i < m.next; i++) add[na++] = m.ext[i];
        const extent_t *src = nranges ? ranges : dc.ext;
        int nsrc = nranges ? nranges : dc.next;
        for (int i = 0; i < nsrc && na < 200; i++) add[na++] = src[i];
        normalize_extents(add, &na);
        if (na > 100) na = 100;
        int grew = na != m.next;
        for (int i = 0; !grew && i < na; i++)
            if (add[i].start != m.ext[i].start || add[i].count != m.ext[i].count) grew = 1;
        if (grew) {
            uint8_t *old = m.st; uint32_t oldlim = m.limit;
            memcpy(m.ext, add, sizeof(extent_t) * (size_t)na); m.next = na;
            map_alloc(&m);
            for (uint32_t l = 0; l < oldlim && l < m.limit; l++)
                if (old[l] != ST_OUT && m.st[l] != ST_OUT) m.st[l] = old[l];
            free(old);
        }
    } else {
        if (nranges) { memcpy(m.ext, ranges, sizeof(extent_t) * (size_t)nranges); m.next = nranges; }
        else if (probe) { m.ext[0].start = 0; m.ext[0].count = dc.capacity ? dc.capacity : 2295104; m.next = 1; }
        else { memcpy(m.ext, dc.ext, sizeof(extent_t) * (size_t)dc.next); m.next = dc.next; }
        normalize_extents(m.ext, &m.next);
        if (m.next == 0) {
            print_disc(&dc, d->name);
            d->close(d);
            die("the drive reports no recorded area. Try --probe, or a different drive.");
        }
        map_alloc(&m);
    }

    int fd = open(out, O_RDWR | O_CREAT, 0644);
    if (fd < 0) die("cannot open %s: %s", out, strerror(errno));

    rip_t R;
    memset(&R, 0, sizeof R);
    R.d = d; R.m = &m; R.fd = fd; R.block = block; R.retries = retries; R.probe = probe;
    R.buf = malloc((size_t)32 * SECTOR);
    R.tty = isatty(2);
    R.t_start = R.last_save = now_s();

    print_disc(&dc, d->name);
    if (resumed) {
        if (!same_disc(&R) && !force) {
            d->close(d); close(fd);
            die("the disc in the drive does not match %s. Use a new output name for a new disc.", out);
        }
        counts_t c = map_counts(&m);
        printf("Resuming: %llu of %llu sectors already read.\n",
               (unsigned long long)c.good, (unsigned long long)c.total);
    }
    fflush(stdout);

    int rc = 0;
    set_read_speed(d, 0xFFFF);
    R.pass_name = "pass 1/3 fast";
    rc = block_pass(&R, 1);
    if (rc == 0 && !g_stop) { R.pass_name = "pass 2/3 fill"; rc = block_pass(&R, 0); }
    if (rc == 0 && !g_stop && scrape) {
        counts_t c = map_counts(&m);
        if (c.retry) {
            set_read_speed(d, 2770);   /* about 2x DVD speed: slower reads recover more */
            R.pass_name = "pass 3/3 scrape";
            rc = scrape_pass(&R);
            set_read_speed(d, 0xFFFF);
        }
    }
    progress(&R, 1);
    if (R.tty) fprintf(stderr, "\n");
    map_save(&m);
    fsync(fd);
    close(fd);
    d->close(d);

    counts_t c = map_counts(&m);
    printf("\nRead %llu of %llu sectors (%.1f MB).", (unsigned long long)c.good,
           (unsigned long long)c.total, c.good * (double)SECTOR / 1e6);
    if (c.end) printf(" %llu sectors past the readable end were skipped.", (unsigned long long)c.end);
    printf("\n");
    if (c.bad || c.retry) {
        printf("Unreadable: %llu sectors (%.2f MB). Damaged spots become a brief glitch in the video.\n",
               (unsigned long long)(c.bad + c.retry), (c.bad + c.retry) * (double)SECTOR / 1e6);
        int shown = 0;
        for (int i = 0; i < m.next && shown < 10; i++) {
            uint32_t s = m.ext[i].start, e = s + m.ext[i].count, a = s;
            while (a < e && shown < 10) {
                if (m.st[a] == ST_BAD || m.st[a] == ST_RETRY) {
                    uint32_t b = a; while (b < e && (m.st[b] == ST_BAD || m.st[b] == ST_RETRY)) b++;
                    printf("  LBA %u-%u\n", a, b - 1); shown++; a = b;
                } else a++;
            }
        }
    }

    if (g_stop) { printf("Stopped. Run the same command again to resume.\n"); return 130; }
    if (R.dropped) {
        printf("The drive stopped responding or the disc was removed. Reconnect it and run the "
               "same command again to resume.\n");
        return 3;
    }
    if (c.untried) { printf("%llu sectors still pending; run again to continue.\n", (unsigned long long)c.untried); return 2; }
    return (c.bad || c.retry) ? 2 : 0;
}
