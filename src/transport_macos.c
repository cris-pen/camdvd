/*
 * camdvd - macOS backend.
 *
 * Talks to the drive through IOKit's MMC device user client:
 *   authoring device service -> MMCDeviceInterface -> SCSITaskDeviceInterface
 *   -> ObtainExclusiveAccess -> raw command blocks.
 *
 * This bypasses the file system entirely, which is the point: an
 * unfinalized camcorder DVD-R has no file system for macOS to mount, but the
 * recorded sectors are still readable with plain READ(10) commands.
 */
#ifdef __APPLE__

#include "drive.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/scsi/SCSITaskLib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>

extern char **environ;

typedef struct {
    io_service_t               service;
    IOCFPlugInInterface      **plugin;
    MMCDeviceInterface       **mmc;
    SCSITaskDeviceInterface  **dev;
    int                        exclusive;
} mac_priv_t;

#define MAX_DRIVES 16

typedef struct {
    io_service_t service;
    char vendor[64];
    char product[64];
    char bsd[32];
} drive_desc_t;

static void cfstr_to_buf(CFTypeRef v, char *buf, size_t len) {
    buf[0] = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)v, buf, (CFIndex)len, kCFStringEncodingUTF8);
    /* trim trailing spaces (INQUIRY strings are space padded) */
    size_t n = strlen(buf);
    while (n && buf[n - 1] == ' ') buf[--n] = 0;
}

static int enumerate(drive_desc_t *out, int max) {
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef sub = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sub, CFSTR(kIOPropertySCSITaskDeviceCategory),
                         CFSTR(kIOPropertySCSITaskAuthoringDevice));
    CFDictionarySetValue(match, CFSTR(kIOPropertyMatchKey), sub);
    CFRelease(sub);

    io_iterator_t it = 0;
    /* IOServiceGetMatchingServices consumes the matching dictionary. */
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, match, &it) != KERN_SUCCESS)
        return 0;

    int n = 0;
    io_service_t s;
    while ((s = IOIteratorNext(it)) != 0) {
        if (n >= max) { IOObjectRelease(s); continue; }
        drive_desc_t *d = &out[n];
        memset(d, 0, sizeof(*d));
        d->service = s;

        CFTypeRef chars = IORegistryEntrySearchCFProperty(s, kIOServicePlane,
            CFSTR("Device Characteristics"), kCFAllocatorDefault,
            kIORegistryIterateRecursively | kIORegistryIterateParents);
        if (chars && CFGetTypeID(chars) == CFDictionaryGetTypeID()) {
            cfstr_to_buf(CFDictionaryGetValue((CFDictionaryRef)chars, CFSTR("Vendor Name")),
                         d->vendor, sizeof d->vendor);
            cfstr_to_buf(CFDictionaryGetValue((CFDictionaryRef)chars, CFSTR("Product Name")),
                         d->product, sizeof d->product);
        }
        if (chars) CFRelease(chars);

        /* The BSD disk node only exists while macOS has media published. */
        CFTypeRef bsd = IORegistryEntrySearchCFProperty(s, kIOServicePlane,
            CFSTR("BSD Name"), kCFAllocatorDefault, kIORegistryIterateRecursively);
        cfstr_to_buf(bsd, d->bsd, sizeof d->bsd);
        if (bsd) CFRelease(bsd);
        n++;
    }
    IOObjectRelease(it);
    return n;
}

int mac_list(void) {
    drive_desc_t d[MAX_DRIVES];
    int n = enumerate(d, MAX_DRIVES);
    if (n == 0) {
        printf("No optical drives found. Check the USB connection; some drives need "
               "a powered hub or both plugs of a Y-cable.\n");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        printf("%d: %s %s%s%s%s\n", i, d[i].vendor[0] ? d[i].vendor : "?",
               d[i].product[0] ? d[i].product : "?",
               d[i].bsd[0] ? "  (" : "", d[i].bsd, d[i].bsd[0] ? ")" : "");
        IOObjectRelease(d[i].service);
    }
    return n;
}

static void run_quiet(const char *const argv[]) {
    pid_t pid;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (posix_spawn(&pid, argv[0], &fa, NULL, (char *const *)argv, environ) == 0) {
        int st;
        waitpid(pid, &st, 0);
    }
    posix_spawn_file_actions_destroy(&fa);
}

static cmd_result_t mac_cmd(drive_t *d, const uint8_t *cdb, int cdb_len,
                            void *buf, uint32_t len, int dir, uint32_t timeout_ms) {
    mac_priv_t *p = (mac_priv_t *)d->priv;
    cmd_result_t r;
    memset(&r, 0, sizeof r);
    r.result = CMD_TRANSPORT;

    SCSITaskInterface **task = (*p->dev)->CreateSCSITask(p->dev);
    if (!task) { r.os_error = kIOReturnNoMemory; return r; }

    uint8_t cdbcopy[16];
    memset(cdbcopy, 0, sizeof cdbcopy);
    memcpy(cdbcopy, cdb, (size_t)cdb_len);

    IOReturn kr = (*task)->SetCommandDescriptorBlock(task, cdbcopy, (UInt8)cdb_len);
    if (kr == kIOReturnSuccess) {
        if (dir == XFER_IN && len > 0) {
            SCSITaskSGElement sg;
            sg.address = (uintptr_t)buf;
            sg.length = len;
            kr = (*task)->SetScatterGatherEntries(task, &sg, 1, len,
                                                  kSCSIDataTransfer_FromTargetToInitiator);
        } else {
            kr = (*task)->SetScatterGatherEntries(task, NULL, 0, 0,
                                                  kSCSIDataTransfer_NoDataTransfer);
        }
    }
    if (kr == kIOReturnSuccess)
        kr = (*task)->SetTimeoutDuration(task, timeout_ms);

    if (kr == kIOReturnSuccess) {
        SCSI_Sense_Data sense;
        SCSITaskStatus status = kSCSITaskStatus_No_Status;
        UInt64 xfer = 0;
        memset(&sense, 0, sizeof sense);
        kr = (*task)->ExecuteTaskSync(task, &sense, &status, &xfer);
        r.os_error = kr;
        r.transferred = (uint32_t)xfer;
        if (kr == kIOReturnSuccess) {
            if (status == kSCSITaskStatus_GOOD) {
                r.result = CMD_OK;
            } else if (status == kSCSITaskStatus_CHECK_CONDITION) {
                uint8_t raw[sizeof sense];
                memcpy(raw, &sense, sizeof sense);
                r.result = CMD_CHECK;
                r.sk = raw[2] & 0x0F;
                r.asc = raw[12];
                r.ascq = raw[13];
            } else {
                r.result = CMD_TRANSPORT;  /* BUSY, TASK SET FULL, ... */
                r.os_error = (int)status;
            }
        }
    } else {
        r.os_error = kr;
    }
    (*task)->Release(task);
    return r;
}

static void mac_close(drive_t *d) {
    if (!d) return;
    mac_priv_t *p = (mac_priv_t *)d->priv;
    if (p) {
        if (p->dev) {
            if (p->exclusive) (*p->dev)->ReleaseExclusiveAccess(p->dev);
            (*p->dev)->Release(p->dev);
        }
        if (p->mmc) (*p->mmc)->Release(p->mmc);
        if (p->plugin) IODestroyPlugInInterface(p->plugin);
        if (p->service) IOObjectRelease(p->service);
        free(p);
    }
    free(d);
}

drive_t *mac_open(const char *spec, char *err, size_t errlen) {
    drive_desc_t list[MAX_DRIVES];
    int n = enumerate(list, MAX_DRIVES);
    if (n == 0) {
        snprintf(err, errlen, "no optical drive found (check the USB connection)");
        return NULL;
    }

    int pick = -1;
    if (!spec || !*spec) {
        pick = 0;
    } else if (strncmp(spec, "disk", 4) == 0 || strncmp(spec, "/dev/", 5) == 0) {
        const char *b = strncmp(spec, "/dev/", 5) == 0 ? spec + 5 : spec;
        for (int i = 0; i < n; i++)
            if (list[i].bsd[0] && strcmp(list[i].bsd, b) == 0) pick = i;
    } else {
        char *end;
        long v = strtol(spec, &end, 10);
        if (*end == 0 && v >= 0 && v < n) pick = (int)v;
    }
    if (pick < 0) {
        for (int i = 0; i < n; i++) IOObjectRelease(list[i].service);
        snprintf(err, errlen, "drive '%s' not found; run 'camdvd list'", spec);
        return NULL;
    }
    for (int i = 0; i < n; i++)
        if (i != pick) IOObjectRelease(list[i].service);

    drive_desc_t *dd = &list[pick];

    /* Exclusive access fails while macOS has the disc mounted. */
    if (dd->bsd[0]) {
        char node[64];
        snprintf(node, sizeof node, "/dev/%s", dd->bsd);
        const char *argv[] = {"/usr/sbin/diskutil", "unmountDisk", "force", node, NULL};
        run_quiet(argv);
    }

    mac_priv_t *p = calloc(1, sizeof *p);
    drive_t *d = calloc(1, sizeof *d);
    if (!p || !d) { free(p); free(d); snprintf(err, errlen, "out of memory"); return NULL; }
    d->priv = p;
    d->cmd = mac_cmd;
    d->close = mac_close;
    snprintf(d->name, sizeof d->name, "%s %s", dd->vendor, dd->product);
    p->service = dd->service;

    SInt32 score = 0;
    IOReturn kr = IOCreatePlugInInterfaceForService(p->service, kIOMMCDeviceUserClientTypeID,
                                                    kIOCFPlugInInterfaceID, &p->plugin, &score);
    if (kr != kIOReturnSuccess || !p->plugin) {
        if (kr == kIOReturnNoResources)
            snprintf(err, errlen,
                "macOS refused to open the drive (0x%08x). This happens when the tool runs "
                "from a privacy-protected folder (Desktop, Documents, Downloads, iCloud). "
                "Move the camdvd folder to your home folder, e.g. ~/camdvd, and run it from there.",
                (unsigned)kr);
        else
            snprintf(err, errlen, "could not create the MMC user client (0x%08x)", (unsigned)kr);
        mac_close(d);
        return NULL;
    }

    HRESULT hr = (*p->plugin)->QueryInterface(p->plugin,
                     CFUUIDGetUUIDBytes(kIOMMCDeviceInterfaceID), (LPVOID *)&p->mmc);
    if (hr != S_OK || !p->mmc) {
        snprintf(err, errlen, "MMC interface unavailable (0x%08x)", (unsigned)hr);
        mac_close(d);
        return NULL;
    }

    p->dev = (*p->mmc)->GetSCSITaskDeviceInterface(p->mmc);
    if (!p->dev) {
        snprintf(err, errlen, "SCSI task interface unavailable");
        mac_close(d);
        return NULL;
    }

    /* Give Disk Arbitration a moment to let go after the unmount. */
    for (int attempt = 0; attempt < 15; attempt++) {
        kr = (*p->dev)->ObtainExclusiveAccess(p->dev);
        if (kr == kIOReturnSuccess) { p->exclusive = 1; break; }
        if (attempt == 4 && dd->bsd[0]) {
            char node[64];
            snprintf(node, sizeof node, "/dev/%s", dd->bsd);
            const char *argv[] = {"/usr/sbin/diskutil", "unmountDisk", "force", node, NULL};
            run_quiet(argv);
        }
        sleep(1);
    }
    if (!p->exclusive) {
        snprintf(err, errlen,
            "the drive is busy (0x%08x). Quit apps that may be holding it (DVD Player, Music, "
            "Disk Utility, Finder burn folders, VLC), dismiss any 'blank disc' dialog with "
            "'Ignore', then try again.", (unsigned)kr);
        mac_close(d);
        return NULL;
    }
    return d;
}

#endif /* __APPLE__ */
