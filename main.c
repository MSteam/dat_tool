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

// --- Shared LP scatter/gather table (used by both extract and record) ---
// Adapted from DATlib (c) 1995-1996 Marcus Meissner, FAU IMMD4

short g_lp_scatter[DATA_SIZE];
static int g_lp_scatter_inited = 0;

void lp_init_scatter(void) {
    if (g_lp_scatter_inited) return;

    short datbuf[2][128][32];
    int   xx[DATA_SIZE];

    memset(g_lp_scatter, 0, sizeof(g_lp_scatter));
    memset(datbuf, 0, sizeof(datbuf));

    for (int i = 0; i < DATA_SIZE; i++) xx[i] = i;

    for (int i = DATA_SIZE / 2; i-- > 0; ) {
        if ((i % 6) >= 3) {
            int tmp = xx[i]; xx[i] = xx[i + 2880]; xx[i + 2880] = tmp;
        }
    }

    for (int i = 0; i < 1440 * 4; i++) {
        int I = i / 4, A = (i & 2) / 2, U = 1 - (i % 2);
        short X = A % 2;
        short Y = (I % 52) + 75 * (I % 2) + (I / 832);
        short Z = 2 * (U + (I / 52)) - ((I / 52) % 2) - 32 * (I / 832);
        datbuf[X][Y][Z] = (short)i;
    }

    for (int i = 0; i < DATA_SIZE; i++) {
        int S = i % 3, I = i / 3, A = I / 960;
        int U = (3 * (I / 2) + S) % 2;
        int J = 2 * ((3 * (I / 2) + S) / 2) + (I % 2) - (1440 * A);
        short X = (short)((A + J) % 2);
        short Y = (short)((J % 52) + 75 * (J % 2) + J / 832);
        short Z = (short)(2 * (U + (J / 52)) - ((J / 52) % 2) - 32 * (J / 832));
        g_lp_scatter[xx[i]] = datbuf[X][Y][Z];
    }

    g_lp_scatter_inited = 1;
}

// --- Drive detection ---

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

    const char *devname = strrchr(dev_path, '/');
    devname = devname ? devname + 1 : dev_path;

    int have_vendor = read_sysfs_attr(devname, "vendor", vendor, sizeof(vendor));
    int have_model  = read_sysfs_attr(devname, "model",  model,  sizeof(model));
    int have_rev    = read_sysfs_attr(devname, "rev",    rev,    sizeof(rev));

    if (!have_vendor && !have_model) {
        printf("Drive info : unavailable (sysfs not accessible)\n");
        return;
    }

    int drive_ok = 0;
    for (int i = 0; i < (int)NUM_KNOWN_DRIVES; i++) {
        if (strcmp(vendor, known_drives[i].vendor) == 0 &&
            strncmp(model, known_drives[i].product_prefix,
                    strlen(known_drives[i].product_prefix)) == 0) {
            drive_ok = 1;
            break;
        }
    }

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
    ioctl(fd, MTIOCTOP, &mt_cmd);

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

// --- Argument parsing ---

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

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s play  [params] /dev/st0\n", prog);
    fprintf(stderr, "  %s save  [params] /dev/st0 [prefix]    (default prefix 'track')\n", prog);
    fprintf(stderr, "  %s write [params] /dev/st0 tape.cue\n", prog);
    fprintf(stderr, "\nParameters (name=value, any order, before device path):\n");
    fprintf(stderr, "  dat_batch=N   frames per write() syscall  (write only; 1..%d, default 1)\n", MAX_BATCH);
    fprintf(stderr, "  buffer=SIZE   RAM ring buffer size        (e.g. 4M, 64M, 1G; default 4M)\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s save  dat_batch=2 /dev/st0 mytape\n", prog);
    fprintf(stderr, "  %s play  dat_batch=2 buffer=64M /dev/st0\n", prog);
    fprintf(stderr, "  %s write dat_batch=2 /dev/st0 tape.cue\n", prog);
}

// Parse named params (name=value) from argv[start..argc), stopping at first
// positional arg. Advances *idx to first positional arg. Returns 0 on success.
static int parse_named_params(int argc, char *argv[], int *idx,
                              size_t *buffer_size, int *dat_batch) {
    int i = *idx;
    while (i < argc) {
        const char *a = argv[i];
        const char *eq = strchr(a, '=');
        if (!eq) break;

        size_t klen = (size_t)(eq - a);
        const char *val = eq + 1;

        if (klen == 9 && strncmp(a, "dat_batch", 9) == 0) {
            int b = atoi(val);
            if (b < 1 || b > MAX_BATCH) {
                fprintf(stderr, "Error: dat_batch must be 1..%d\n", MAX_BATCH);
                return -1;
            }
            *dat_batch = b;
        } else if (klen == 6 && strncmp(a, "buffer", 6) == 0) {
            size_t sz = parse_buffer_size(val);
            if (sz == 0) {
                fprintf(stderr, "Error: invalid buffer size '%s' (use e.g. 4M, 64M, 1G)\n", val);
                return -1;
            }
            *buffer_size = sz;
        } else {
            fprintf(stderr, "Error: unknown parameter '%.*s' (expected dat_batch= or buffer=)\n",
                    (int)klen, a);
            return -1;
        }
        i++;
    }
    *idx = i;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    int mode_play  = (strcmp(argv[1], "play")  == 0);
    int mode_save  = (strcmp(argv[1], "save")  == 0);
    int mode_write = (strcmp(argv[1], "write") == 0);

    if (!mode_play && !mode_save && !mode_write) {
        fprintf(stderr, "Unknown mode: %s\n\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    size_t      buffer_size = 0;
    int         dat_batch   = 1;
    const char *dev_path    = NULL;
    const char *cue_path    = NULL;
    const char *prefix      = "track";

    int idx = 2;
    if (parse_named_params(argc, argv, &idx, &buffer_size, &dat_batch) < 0)
        return 1;

    // Positional: device path
    if (idx >= argc) {
        fprintf(stderr, "Error: device path required.\n\n");
        print_usage(argv[0]);
        return 1;
    }
    dev_path = argv[idx++];

    // Mode-specific positional args
    if (mode_write) {
        if (idx >= argc) {
            fprintf(stderr, "Error: CUE file required for write mode.\n\n");
            print_usage(argv[0]);
            return 1;
        }
        cue_path = argv[idx++];
    } else if (mode_save) {
        if (idx < argc) prefix = argv[idx++];
    }

    int fd = open(dev_path, mode_write ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        perror("Failed to open tape device");
        return 1;
    }

    print_drive_info(dev_path);
    configure_tape_drive(fd);

    int result = 0;
    if (mode_write)     result = execute_record(fd, cue_path, buffer_size, dat_batch);
    else if (mode_play) result = execute_play(fd, buffer_size, dat_batch);
    else                result = execute_save(fd, prefix, buffer_size, dat_batch);

    close(fd);
    return result;
}
