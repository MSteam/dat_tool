#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mtio.h>
#include "dat_tool.h"

// Known compatible drives: vendor + product ID prefix (XXX = variant suffix, any value)
static const struct { const char *vendor; const char *product_prefix; } known_drives[] = {
    { "ARCHIVE", "Python 25601-" },
    { "ARCHIVE", "Python 01931-" },
    { "ARCHIVE", "Python 25501-" },
    { "SONY",    "SDT-9000"      },
};
#define NUM_KNOWN_DRIVES (sizeof(known_drives) / sizeof(known_drives[0]))

// Known compatible firmware versions: vendor + product prefix + exact revision string
static const struct { const char *vendor; const char *product_prefix; const char *revision; } known_firmwares[] = {
    { "ARCHIVE", "Python 25601-", "2.63" },
    { "ARCHIVE", "Python 25601-", "2.75" },
    { "ARCHIVE", "Python 01931-", "5AC"  },
    { "ARCHIVE", "Python 01931-", "5.56" },
    { "ARCHIVE", "Python 01931-", "5.63" },
    { "SONY",    "SDT-9000",      "13.1" },
    { "SONY",    "SDT-9000",      "12.2" },
};
#define NUM_KNOWN_FIRMWARES (sizeof(known_firmwares) / sizeof(known_firmwares[0]))

static void rtrim(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\n' || s[len-1] == '\r'))
        s[--len] = '\0';
}

static int read_sysfs_attr(const char *devname, const char *attr, char *out, int out_len) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/scsi_tape/%s/device/%s", devname, attr);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = (fgets(out, out_len, f) != NULL);
    fclose(f);
    if (ok) rtrim(out);
    return ok;
}

static void print_drive_info(const char *dev_path) {
    char vendor[16] = {0}, model[32] = {0}, rev[8] = {0};

    // Extract basename: "/dev/st0" -> "st0", "/dev/nst0" -> "nst0"
    const char *devname = strrchr(dev_path, '/');
    devname = devname ? devname + 1 : dev_path;

    int have_vendor = read_sysfs_attr(devname, "vendor", vendor, sizeof(vendor));
    int have_model  = read_sysfs_attr(devname, "model",  model,  sizeof(model));
    int have_rev    = read_sysfs_attr(devname, "rev",    rev,    sizeof(rev));

    if (!have_vendor && !have_model) {
        printf("Drive info : unavailable (sysfs not accessible)\n");
        return;
    }

    // Check drive compatibility
    int drive_ok = 0;
    for (int i = 0; i < (int)NUM_KNOWN_DRIVES; i++) {
        if (strcmp(vendor, known_drives[i].vendor) == 0 &&
            strncmp(model, known_drives[i].product_prefix,
                    strlen(known_drives[i].product_prefix)) == 0) {
            drive_ok = 1;
            break;
        }
    }

    // Check firmware compatibility
    int fw_ok = 0;
    if (have_rev) {
        for (int i = 0; i < (int)NUM_KNOWN_FIRMWARES; i++) {
            if (strcmp(vendor, known_firmwares[i].vendor) == 0 &&
                strncmp(model, known_firmwares[i].product_prefix,
                        strlen(known_firmwares[i].product_prefix)) == 0 &&
                strcmp(rev, known_firmwares[i].revision) == 0) {
                fw_ok = 1;
                break;
            }
        }
    }

    printf("Drive     : %s %s  [%s]\n",
           vendor, model,
           drive_ok ? "COMPATIBLE" : "UNKNOWN");
    printf("Firmware  : %s  [%s]\n",
           have_rev ? rev : "?",
           fw_ok ? "COMPATIBLE" : "UNKNOWN");
}

void configure_tape_drive(int fd) {
    struct mtop mt_cmd;

    printf("Initializing tape drive (MTLOAD)...\n");
    mt_cmd.mt_op = MTLOAD;
    mt_cmd.mt_count = 1;
    ioctl(fd, MTIOCTOP, &mt_cmd);  // ignore: tape may already be loaded

    sleep(2);

    printf("Configuring tape drive parameters...\n");

    mt_cmd.mt_op = MTSETBLK; mt_cmd.mt_count = 0;
    if (ioctl(fd, MTIOCTOP, &mt_cmd) == 0) {
        printf(" [+] Block size set to 0 (Variable)\n");
    } else {
        perror(" [-] Failed to set block size");
        close(fd); exit(1);
    }

    mt_cmd.mt_op = MTSETDENSITY; mt_cmd.mt_count = 0x80;
    if (ioctl(fd, MTIOCTOP, &mt_cmd) == 0) {
        printf(" [+] Density set to 0x80 (Audio)\n");
    } else {
        perror(" [-] Failed to set density");
        fprintf(stderr, "[ERROR] Drive may not have audio firmware. Audio operations require\n"
                        "        a DAT drive with audio firmware (e.g. ARCHIVE Python, SONY SDT-9000).\n");
        close(fd); exit(1);
    }

    mt_cmd.mt_op = MTCOMPRESSION; mt_cmd.mt_count = 0;
    if (ioctl(fd, MTIOCTOP, &mt_cmd) == 0) {
        printf(" [+] Hardware compression disabled\n");
    } else {
        perror(" [-] Failed to disable compression");
        close(fd); exit(1);
    }

    printf("----------------------------------------\n");
}

static size_t parse_buffer_size(const char *s) {
    if (!s) return 0;
    size_t val = 0;
    char   unit = 0;
    int    n = sscanf(s, "%zu%c", &val, &unit);
    if (n < 1 || val == 0) return 0;
    if (n == 1) return val;
    switch (unit) {
        case 'K': case 'k': return val * 1024UL;
        case 'M': case 'm': return val * 1024UL * 1024;
        case 'G': case 'g': return val * 1024UL * 1024 * 1024;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s play  [bufsize] /dev/st0   (e.g. 256M, 1G; default 256M)\n", argv[0]);
        fprintf(stderr, "  %s save  /dev/st0\n", argv[0]);
        fprintf(stderr, "  %s write /dev/st0 tape.cue\n", argv[0]);
        return 1;
    }

    int mode_write = (strcmp(argv[1], "write") == 0);
    int mode_play  = (strcmp(argv[1], "play")  == 0);
    int mode_save  = (strcmp(argv[1], "save")  == 0);

    if (!mode_write && !mode_play && !mode_save) {
        fprintf(stderr, "Unknown mode: %s\n", argv[1]);
        return 1;
    }

    // Determine device path and optional buffer size
    const char *dev_path = NULL;
    const char *cue_path = NULL;
    size_t      buf_size = 0;

    if (mode_play) {
        // dat_tool play [bufsize] /dev/st0
        if (argc >= 4 && argv[2][0] >= '0' && argv[2][0] <= '9') {
            buf_size = parse_buffer_size(argv[2]);
            dev_path = argv[3];
        } else {
            dev_path = argv[2];
        }
    } else if (mode_write) {
        if (argc < 4) {
            fprintf(stderr, "Error: CUE file required for write mode.\n");
            return 1;
        }
        dev_path = argv[2];
        cue_path = argv[3];
    } else {
        dev_path = argv[2];
    }

    int fd = open(dev_path, mode_write ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        perror("Failed to open tape device");
        return 1;
    }

    print_drive_info(dev_path);
    configure_tape_drive(fd);

    int result = 0;
    if (mode_write)     result = execute_record(fd, cue_path);
    else if (mode_play) result = execute_play(fd, buf_size);
    else                result = execute_save(fd);

    close(fd);
    return result;
}
