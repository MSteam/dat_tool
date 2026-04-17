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
    
    // Step 1: Initialize the drive and clear Unit Attention status
    // This command forces the drive to load the tape and settle the mechanical assembly.
    // We ignore the direct return value to handle cases where the tape might already be loaded.
    printf("Initializing tape drive (MTLOAD)...\n");
    mt_cmd.mt_op = MTLOAD; 
    mt_cmd.mt_count = 1;
    ioctl(fd, MTIOCTOP, &mt_cmd);
    
    // Give the hardware a short moment to stabilize the drum and sensors
    sleep(2);

    printf("Configuring tape drive parameters...\n");

    // Set variable block size (0) for audio data compatibility
    mt_cmd.mt_op = MTSETBLK; mt_cmd.mt_count = 0;
    if (ioctl(fd, MTIOCTOP, &mt_cmd) == 0) printf(" [+] Block size set to 0 (Variable)\n");
    else perror(" [-] Failed to set block size");

    // Set density to 0x80 (standard for audio on DDS drives)
    mt_cmd.mt_op = MTSETDENSITY; mt_cmd.mt_count = 0x80;
    if (ioctl(fd, MTIOCTOP, &mt_cmd) == 0) printf(" [+] Density set to 0x80 (Audio)\n");
    else perror(" [-] Failed to set density");

    // Disable hardware compression to ensure bit-perfect audio transfer
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

    // Open the tape device
    int fd = open(argv[2], mode_write ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        perror("Failed to open tape device");
        return 1;
    }

    // Apply universal drive configuration (including the MTLOAD fix)
    configure_tape_drive(fd);

    int result = 0;
    if (mode_write) {
        if (argc < 4) {
            fprintf(stderr, "Error: CUE file required for write mode.\n");
            close(fd);
            return 1;
        }
        result = execute_record(fd, argv[3]);
    } else {
        result = execute_extract_or_play(fd, mode_play);
    }

    close(fd);
    return result;
}
