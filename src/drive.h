/*
 * camdvd - drive transport interface.
 *
 * A backend sends raw MMC (SCSI) command blocks to an optical drive.
 * Backends: macOS IOKit (transport_macos.c) and a simulated drive for
 * testing (transport_sim.c).
 */
#ifndef CAMDVD_DRIVE_H
#define CAMDVD_DRIVE_H

#include <stddef.h>
#include <stdint.h>

enum { XFER_NONE = 0, XFER_IN = 1 };

enum {
    CMD_OK = 0,          /* command completed, status GOOD            */
    CMD_CHECK = 1,       /* CHECK CONDITION, sense data valid         */
    CMD_TRANSPORT = -1   /* the command never completed (USB, timeout) */
};

typedef struct {
    int      result;     /* CMD_OK / CMD_CHECK / CMD_TRANSPORT */
    uint8_t  sk, asc, ascq;
    uint32_t transferred;
    int      os_error;   /* backend specific code, for messages */
} cmd_result_t;

typedef struct drive drive_t;

struct drive {
    cmd_result_t (*cmd)(drive_t *d, const uint8_t *cdb, int cdb_len,
                        void *buf, uint32_t len, int dir, uint32_t timeout_ms);
    void (*close)(drive_t *d);
    char name[256];
    void *priv;
};

/* Backends. Each returns NULL and fills err on failure. */
drive_t *sim_open(const char *config_path, char *err, size_t errlen);
#ifdef __APPLE__
drive_t *mac_open(const char *spec, char *err, size_t errlen);
int      mac_list(void);
#endif

#endif
