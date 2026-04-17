#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mtio.h>
#include "dat_tool.h"

void configure_tape_drive(int fd) {
    struct mtop mt_cmd;
    printf("Configuring tape drive parameters...\n");

    mt_cmd.mt_op = MTSETBLK; mt_cmd.mt_count = 0;
    if (ioctl(fd, MTIOCTOP, &mt_cmd) == 0) printf(" [+] Block size set to 0 (Variable)\n");
    else perror(" [-] Failed to set block size");

    mt_cmd.mt_op = MTSETDENSITY; mt_cmd.mt_count = 0x80;
    if (ioctl(fd, MTIOCTOP, &mt_cmd) == 0) printf(" [+] Density set to 0x80 (Audio)\n");
    else perror(" [-] Failed to set density");

    mt_cmd.mt_op = MTCOMPRESSION; mt_cmd.mt_count = 0;
    if (ioctl(fd, MTIOCTOP, &mt_cmd) == 0) printf(" [+] Hardware compression disabled\n");
    else perror(" [-] Failed to disable compression");

    printf("----------------------------------------\n");
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s play /dev/st0\n", argv[0]);
        fprintf(stderr, "  %s save /dev/st0\n", argv[0]);
        fprintf(stderr, "  %s write /dev/st0 tape.cue\n", argv[0]);
        return 1;
    }

    int mode_write = (strcmp(argv[1], "write") == 0);
    int mode_play = (strcmp(argv[1], "play") == 0);
    int mode_save = (strcmp(argv[1], "save") == 0);

    if (!mode_write && !mode_play && !mode_save) {
        fprintf(stderr, "Unknown mode: %s\n", argv[1]);
        return 1;
    }

    int flags = mode_write ? O_WRONLY : O_RDONLY;
    int fd = open(argv[2], flags);
    if (fd < 0) { perror("Failed to open device"); return 1; }

    configure_tape_drive(fd);

    if (mode_write) {
        if (argc < 4) { fprintf(stderr, "Error: CUE file required for write mode.\n"); close(fd); return 1; }
        execute_record(fd, argv[3]);
    } else {
        execute_extract_or_play(fd, mode_play);
    }

    close(fd);
    return 0;
}
