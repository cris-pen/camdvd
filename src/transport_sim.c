/*
 * camdvd - simulated drive for testing the rip logic without hardware.
 *
 * Config file (one directive per line, '#' comments):
 *   image   <path>                 backing image, LBA n at byte n*2048
 *   vendor  <text>                 INQUIRY vendor/product string
 *   profile <hex>                  current MMC profile, e.g. 0x0011 (DVD-R)
 *   disc_status <0|1|2>            empty / incomplete / complete
 *   capacity <sectors>             READ CAPACITY result (0 = reports nothing)
 *   track <n> <start> <size> blank            reserved, never written
 *   track <n> <start> <size> nwa <lba>        incomplete: recorded up to nwa-1
 *   track <n> <start> <size> full             complete track
 *   bad   <first> <last>                      permanently unreadable
 *   flaky <first> <last> <fail_count>         fails N times, then reads
 *   wedge_after <reads>                       transport error after N reads
 *   readable <first> <last>                   readable but not reported in track info
 */
#include "drive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SIM_MAX_TRACKS 32
#define SIM_MAX_RANGES 256

typedef struct { uint32_t n, start, size, nwa; int kind; } sim_track_t; /* kind 0 blank,1 inc,2 full */
typedef struct { uint32_t a, b; int fails; int *count; } sim_range_t;

typedef struct {
    FILE *img;
    char vendor[64];
    uint16_t profile;
    int disc_status;
    uint32_t capacity;
    sim_track_t tracks[SIM_MAX_TRACKS]; int ntracks;
    sim_range_t bad[SIM_MAX_RANGES]; int nbad;
    sim_range_t flaky[SIM_MAX_RANGES]; int nflaky;
    long wedge_after; long reads;
    sim_range_t readable[SIM_MAX_RANGES]; int nreadable;
} sim_t;

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static cmd_result_t check(uint8_t sk, uint8_t asc, uint8_t ascq) {
    cmd_result_t r; memset(&r, 0, sizeof r);
    r.result = CMD_CHECK; r.sk = sk; r.asc = asc; r.ascq = ascq;
    return r;
}

static int recorded(sim_t *s, uint32_t lba) {
    for (int i = 0; i < s->ntracks; i++) {
        sim_track_t *t = &s->tracks[i];
        if (lba < t->start || lba >= t->start + t->size) continue;
        if (t->kind == 0) return 0;
        if (t->kind == 1) return lba < t->nwa;
        return 1;
    }
    for (int i = 0; i < s->nreadable; i++)
        if (lba >= s->readable[i].a && lba <= s->readable[i].b) return 1;
    /* outside any track: readable only if within reported capacity */
    return s->capacity && lba < s->capacity;
}

static cmd_result_t sim_cmd(drive_t *d, const uint8_t *cdb, int cdb_len, void *buf,
                            uint32_t len, int dir, uint32_t timeout_ms) {
    (void)cdb_len; (void)dir; (void)timeout_ms;
    sim_t *s = (sim_t *)d->priv;
    cmd_result_t ok; memset(&ok, 0, sizeof ok);
    uint8_t *b = (uint8_t *)buf;
    if (b && len) memset(b, 0, len);

    switch (cdb[0]) {
    case 0x00: return ok;                                   /* TEST UNIT READY */
    case 0x12:                                              /* INQUIRY */
        if (len >= 36) {
            b[0] = 0x05; b[4] = 31;
            char v[29]; memset(v, ' ', 28); v[28] = 0;
            memcpy(v, s->vendor, strlen(s->vendor) > 28 ? 28 : strlen(s->vendor));
            memcpy(b + 8, v, 28);
        }
        ok.transferred = 36; return ok;
    case 0x46:                                              /* GET CONFIGURATION */
        if (len >= 8) { put32(b, 4); b[6] = s->profile >> 8; b[7] = (uint8_t)s->profile; }
        ok.transferred = 8; return ok;
    case 0x51: {                                            /* READ DISC INFORMATION */
        if (s->disc_status < 0) return check(0x05, 0x20, 0x00);
        b[1] = 32;
        b[2] = (uint8_t)(s->disc_status & 3) | (uint8_t)((s->disc_status == 2 ? 3 : 1) << 2);
        b[3] = 1;
        b[4] = 1;
        b[5] = (uint8_t)(s->ntracks ? s->tracks[0].n : 1);
        b[6] = (uint8_t)(s->ntracks ? s->tracks[s->ntracks - 1].n : 1);
        ok.transferred = 34; return ok;
    }
    case 0x52: {                                            /* READ TRACK INFORMATION */
        uint32_t tn = get32(cdb + 2);
        for (int i = 0; i < s->ntracks; i++) {
            sim_track_t *t = &s->tracks[i];
            if (t->n != tn) continue;
            b[1] = 34; b[2] = (uint8_t)t->n; b[3] = 1;
            if (t->kind == 0) b[6] = 0x80 | 0x40;                 /* RT, Blank */
            if (t->kind == 1) { b[6] = 0x20; b[7] = 0x01; put32(b + 12, t->nwa); } /* NWA valid */
            put32(b + 8, t->start);
            put32(b + 16, t->kind == 2 ? 0 : t->size - (t->kind == 1 ? t->nwa - t->start : 0));
            put32(b + 24, t->size);
            if (t->kind == 2) { b[7] |= 0x02; put32(b + 28, t->start + t->size - 1); }
            ok.transferred = 36; return ok;
        }
        return check(0x05, 0x24, 0x00);
    }
    case 0x25:                                              /* READ CAPACITY */
        put32(b, s->capacity ? s->capacity - 1 : 0); put32(b + 4, 2048);
        ok.transferred = 8; return ok;
    case 0x28: {                                            /* READ(10) */
        uint32_t lba = get32(cdb + 2);
        uint32_t cnt = (uint32_t)cdb[7] << 8 | cdb[8];
        s->reads++;
        if (s->wedge_after && s->reads > s->wedge_after) {
            cmd_result_t r; memset(&r, 0, sizeof r); r.result = CMD_TRANSPORT; r.os_error = -99;
            return r;
        }
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t l = lba + i;
            if (!recorded(s, l)) return check(0x05, 0x21, 0x00);
            for (int k = 0; k < s->nbad; k++)
                if (l >= s->bad[k].a && l <= s->bad[k].b) return check(0x03, 0x11, 0x00);
            for (int k = 0; k < s->nflaky; k++) {
                sim_range_t *f = &s->flaky[k];
                if (l >= f->a && l <= f->b && f->count[l - f->a] < f->fails) {
                    f->count[l - f->a]++;
                    return check(0x03, 0x11, 0x05);
                }
            }
        }
        if (fseek(s->img, (long)lba * 2048L, SEEK_SET) == 0) {
            size_t got = fread(b, 1, (size_t)cnt * 2048, s->img);
            (void)got;  /* short read past the end of the file stays zero */
        }
        ok.transferred = cnt * 2048; return ok;
    }
    case 0xBB: case 0x1E: case 0x1B: return ok;             /* SET SPEED, PREVENT, START/STOP */
    default: return check(0x05, 0x20, 0x00);
    }
}

static void sim_close(drive_t *d) {
    if (!d) return;
    sim_t *s = (sim_t *)d->priv;
    if (s) {
        if (s->img) fclose(s->img);
        for (int i = 0; i < s->nflaky; i++) free(s->flaky[i].count);
        free(s);
    }
    free(d);
}

drive_t *sim_open(const char *path, char *err, size_t errlen) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "cannot open sim config %s", path); return NULL; }
    sim_t *s = calloc(1, sizeof *s);
    drive_t *d = calloc(1, sizeof *d);
    snprintf(s->vendor, sizeof s->vendor, "SIMULATED DVD DRIVE");
    s->profile = 0x0011; s->disc_status = 1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char k[64] = {0}, a[512] = {0}, c[64] = {0}, e[64] = {0}, g[64] = {0};
        int n = sscanf(line, "%63s %511s %63s %63s %63s %63s", k, a, c, e, g, g);
        if (n < 1) continue;
        if (!strcmp(k, "image")) {
            s->img = fopen(a, "rb");
            if (!s->img) { snprintf(err, errlen, "cannot open sim image %s", a); fclose(f); sim_close(d ? (d->priv = s, d) : NULL); return NULL; }
        } else if (!strcmp(k, "vendor")) {
            char *v = line + strlen("vendor"); while (*v == ' ') v++;
            v[strcspn(v, "\r\n")] = 0; snprintf(s->vendor, sizeof s->vendor, "%.63s", v);
        } else if (!strcmp(k, "profile")) s->profile = (uint16_t)strtoul(a, NULL, 0);
        else if (!strcmp(k, "disc_status")) s->disc_status = atoi(a);
        else if (!strcmp(k, "capacity")) s->capacity = (uint32_t)strtoul(a, NULL, 0);
        else if (!strcmp(k, "wedge_after")) s->wedge_after = atol(a);
        else if (!strcmp(k, "track") && s->ntracks < SIM_MAX_TRACKS) {
            char kind[32] = {0}, extra[32] = {0};
            unsigned tn, st, sz;
            if (sscanf(line, "%*s %u %u %u %31s %31s", &tn, &st, &sz, kind, extra) >= 4) {
                sim_track_t *t = &s->tracks[s->ntracks++];
                t->n = tn; t->start = st; t->size = sz;
                if (!strcmp(kind, "blank")) t->kind = 0;
                else if (!strcmp(kind, "nwa")) { t->kind = 1; t->nwa = (uint32_t)strtoul(extra, NULL, 0); }
                else t->kind = 2;
            }
        } else if (!strcmp(k, "bad") && s->nbad < SIM_MAX_RANGES) {
            s->bad[s->nbad].a = (uint32_t)strtoul(a, NULL, 0);
            s->bad[s->nbad].b = (uint32_t)strtoul(c, NULL, 0); s->nbad++;
        } else if (!strcmp(k, "readable") && s->nreadable < SIM_MAX_RANGES) {
            s->readable[s->nreadable].a = (uint32_t)strtoul(a, NULL, 0);
            s->readable[s->nreadable].b = (uint32_t)strtoul(c, NULL, 0); s->nreadable++;
        } else if (!strcmp(k, "flaky") && s->nflaky < SIM_MAX_RANGES) {
            sim_range_t *r = &s->flaky[s->nflaky++];
            r->a = (uint32_t)strtoul(a, NULL, 0); r->b = (uint32_t)strtoul(c, NULL, 0);
            r->fails = atoi(e); r->count = calloc(r->b - r->a + 1, sizeof(int));
        }
    }
    fclose(f);
    if (!s->img) { snprintf(err, errlen, "sim config has no image line"); d->priv = s; sim_close(d); return NULL; }
    d->priv = s; d->cmd = sim_cmd; d->close = sim_close;
    snprintf(d->name, sizeof d->name, "%s", s->vendor);
    return d;
}
