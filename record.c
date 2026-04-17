#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mtio.h>
#include "dat_tool.h"

int g_abs_frames = 0; // Absolute frame counter
int g_rel_frames = 0; // Relative frame counter

// Parse the CUE sheet configuration
void parse_cuefile(const char *filename, CueConfig *cfg) {
    memset(cfg, 0, sizeof(CueConfig));
    FILE *f = fopen(filename, "r");
    if (!f) { perror("Failed to open CUE file"); exit(1); }
    char line[512]; int in_files = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
        if (strstr(line, "[CONFIG]")) { in_files = 0; continue; }
        if (strstr(line, "[FILES]")) { in_files = 1; continue; }

        if (!in_files) {
            if (strstr(line, "STARTID=ON")) cfg->startid = 1;
            else if (strstr(line, "PROGRAM_NUMBER=ON")) cfg->program_number = 1;
            else if (strstr(line, "LEADIN_SILENCE=")) sscanf(line, "LEADIN_SILENCE=%d", &cfg->leadin_silence);
            else if (strstr(line, "LEADOUT_SILENCE=")) sscanf(line, "LEADOUT_SILENCE=%d", &cfg->leadout_silence);
            else if (strstr(line, "INTERTRACK_SILENCE=")) sscanf(line, "INTERTRACK_SILENCE=%d", &cfg->intertrack_silence);
        } else {
            char filepath[512];
            if (sscanf(line, "FILE_%*d=%511[^\r\n]", filepath) == 1) {
                snprintf(cfg->files[cfg->file_count], 512, "%s", filepath);
                cfg->file_count++;
            }
        }
    }
    fclose(f);
}

// Validate all files and print the pre-flight configuration screen
void validate_and_print_playlist(const CueConfig *cfg) {
    printf("\n========================================\n");
    printf(" Tape Recording Configuration\n");
    printf("========================================\n");
    printf(" START-ID (Auto)    : %s\n", cfg->startid ? "ON" : "OFF");
    printf(" PROGRAM NUMBER     : %s\n", cfg->program_number ? "ON" : "OFF");
    printf(" LEAD-IN SILENCE    : %d seconds\n", cfg->leadin_silence);
    printf(" INTERTRACK SILENCE : %d seconds\n", cfg->intertrack_silence);
    printf(" LEAD-OUT SILENCE   : %d seconds\n", cfg->leadout_silence);
    printf("----------------------------------------\n");
    printf(" Playlist (%d files):\n", cfg->file_count);

    int total_audio_seconds = 0;

    for (int i = 0; i < cfg->file_count; i++) {
        FILE *f = fopen(cfg->files[i], "rb");
        if (!f) {
            fprintf(stderr, "\n[FATAL ERROR] Cannot open file [%02d]: '%s'\n", i + 1, cfg->files[i]);
            exit(1);
        }

        WavHeader hdr;
        if (fread(&hdr, sizeof(WavHeader), 1, f) != 1 || strncmp(hdr.riff, "RIFF", 4) != 0 || strncmp(hdr.wave, "WAVE", 4) != 0) {
            fprintf(stderr, "\n[FATAL ERROR] File [%02d] '%s' is not a valid WAV file.\n", i + 1, cfg->files[i]);
            exit(1);
        }

        // Strict format validation
        if (hdr.format_type != 1) {
            fprintf(stderr, "\n[FATAL ERROR] '%s' is compressed. Only uncompressed PCM WAV is supported.\n", cfg->files[i]);
            exit(1);
        }
        if (hdr.channels != 2) {
            fprintf(stderr, "\n[FATAL ERROR] '%s' has %d channels. DAT requires exactly 2 (Stereo).\n", cfg->files[i], hdr.channels);
            exit(1);
        }
        if (hdr.bits_per_sample != 16) {
            fprintf(stderr, "\n[FATAL ERROR] '%s' is %d-bit. DAT requires exactly 16-bit audio.\n", cfg->files[i], hdr.bits_per_sample);
            exit(1);
        }
        if (hdr.sample_rate != 48000 && hdr.sample_rate != 44100 && hdr.sample_rate != 32000) {
            fprintf(stderr, "\n[FATAL ERROR] '%s' has unsupported sample rate (%d Hz).\n", cfg->files[i], hdr.sample_rate);
            fprintf(stderr, "Supported DAT rates: 48000 Hz, 44100 Hz, 32000 Hz.\n");
            exit(1);
        }

        // Calculate accurate duration using file size minus 44-byte header
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        long audio_bytes = file_size - 44;
        if (audio_bytes < 0) audio_bytes = 0;

        int duration_sec = audio_bytes / (hdr.sample_rate * hdr.channels * (hdr.bits_per_sample / 8));
        total_audio_seconds += duration_sec;

        int m = duration_sec / 60;
        int s = duration_sec % 60;

        printf("  [%02d] %s (%d Hz, %d-bit, %d ch, %02d:%02d)\n",
               i + 1, cfg->files[i], hdr.sample_rate, hdr.bits_per_sample, hdr.channels, m, s);

        fclose(f);
    }

    // Calculate total silence duration
    int total_silence_sec = cfg->leadin_silence + cfg->leadout_silence;
    if (cfg->file_count > 1 && cfg->intertrack_silence > 0) {
        total_silence_sec += cfg->intertrack_silence * (cfg->file_count - 1);
    }

    int grand_total = total_audio_seconds + total_silence_sec;
    int h = grand_total / 3600;
    int m = (grand_total % 3600) / 60;
    int s = grand_total % 60;

    printf("----------------------------------------\n");
    printf(" TOTAL TAPE DURATION: %02d:%02d:%02d\n", h, m, s);
    printf("========================================\n\n");
}

void frames_to_time(int total_frames, int *h, int *m, int *s, int *f) {
    int blocks_of_100 = total_frames / 100;
    int remainder = total_frames % 100;
    int total_sec = blocks_of_100 * 3;

    if (remainder < 33) {
        *f = remainder;
    } else if (remainder < 66) {
        total_sec += 1;
        *f = remainder - 33;
    } else {
        total_sec += 2;
        *f = remainder - 66;
    }

    *h = total_sec / 3600;
    *m = (total_sec / 60) % 60;
    *s = total_sec % 60;
}

void write_pack(unsigned char *pack, int item, int pno, int h, int m, int s, int f) {
    pack[0] = (item << 4) | (pno / 100);
    if (pno == 0) {
        pack[1] = 0x00; pack[2] = 0x00;
    } else {
        pack[1] = (((pno / 10) % 10) << 4) | (pno % 10);
        pack[2] = 0x01;
    }
    pack[3] = ((h / 10) << 4) | (h % 10);
    pack[4] = ((m / 10) << 4) | (m % 10);
    pack[5] = ((s / 10) << 4) | (s % 10);
    pack[6] = ((f / 10) << 4) | (f % 10);
    pack[7] = pack[0] ^ pack[1] ^ pack[2] ^ pack[3] ^ pack[4] ^ pack[5] ^ pack[6];
}

void write_dat_frame(int fd, unsigned char *audio_data, int sample_rate, int pno, int start_id) {
    unsigned char frame[FRAME_SIZE];
    memset(frame, 0, FRAME_SIZE);

    int payload_size = (sample_rate == 48000) ? 5760 : (sample_rate == 44100 ? 5292 : 3840);
    memcpy(frame, audio_data, payload_size);

    unsigned char *scode = frame + DATA_SIZE;
    unsigned char *packs = scode;
    unsigned char *subid = scode + 56;
    unsigned char *mainid = subid + 4;

    int a_h, a_m, a_s, a_f;
    frames_to_time(g_abs_frames, &a_h, &a_m, &a_s, &a_f);

    int r_h, r_m, r_s, r_f;
    frames_to_time(g_rel_frames, &r_h, &r_m, &r_s, &r_f);

    write_pack(&packs[0], 1, pno, r_h, r_m, r_s, r_f);
    write_pack(&packs[8], 2, pno, a_h, a_m, a_s, a_f);
    write_pack(&packs[16], 1, pno, r_h, r_m, r_s, r_f);
    write_pack(&packs[24], 2, pno, a_h, a_m, a_s, a_f);
    write_pack(&packs[32], 1, pno, r_h, r_m, r_s, r_f);
    write_pack(&packs[40], 2, pno, a_h, a_m, a_s, a_f);

    // --- Sub-ID Setup ---
    subid[0] = (pno > 0) ? 0x80 : 0x00;
    if (start_id) subid[0] |= 0x40;

    if (pno > 0) {
        subid[1] = 0x01;
        subid[2] = (((pno / 10) % 10) << 4) | (pno % 10);
    } else {
        subid[1] = 0x00; subid[2] = 0x00;
    }
    subid[3] = subid[0] ^ subid[1] ^ subid[2];

    // --- Main-ID Setup (SP MODE) ---
    int sr_bits = 0;
    if (sample_rate == 44100) sr_bits = 1;
    else if (sample_rate == 32000) sr_bits = 2;
    mainid[0] = (sr_bits << 2) | 0x00;
    mainid[1] = 0x00;

    if (write(fd, frame, FRAME_SIZE) != FRAME_SIZE) {
        perror("\n\n[FATAL ERROR] Tape drive rejected the write command");
        exit(1);
    }

    g_abs_frames++;
    g_rel_frames++;
}

void write_silence(int fd, int seconds, int sample_rate, const char *zone) {
    if (seconds <= 0) return;

    g_rel_frames = 0;
    printf("Writing %d sec %s silence...\n", seconds, zone);

    int total_frames = (seconds * 100) / 3;
    unsigned char empty[DATA_SIZE];
    memset(empty, 0, DATA_SIZE);
    int last_printed_sec = -1;

    for (int i = 0; i < total_frames; i++) {
        write_dat_frame(fd, empty, sample_rate, 0, 0);

        int current_sec = (i * 3) / 100;
        if (current_sec != last_printed_sec) {
            printf("\r[SILENCE] Time: %02d / %02d sec", current_sec, seconds);
            fflush(stdout);
            last_printed_sec = current_sec;
        }
    }
    printf("\r[SILENCE] Completed.             \n");
}

void write_wav_file(int fd, const char *filepath, int pno, CueConfig *cfg) {
    FILE *wav = fopen(filepath, "rb");
    if (!wav) { fprintf(stderr, " [-] Cannot open %s\n", filepath); return; }

    WavHeader hdr;
    fread(&hdr, sizeof(WavHeader), 1, wav);
    fseek(wav, 44, SEEK_SET);

    int sample_rate = hdr.sample_rate;
    int channels = hdr.channels;
    int payload_size = (sample_rate == 48000) ? 5760 : (sample_rate == 44100 ? 5292 : 3840);

    unsigned char buf[DATA_SIZE];
    int bytes_read;
    int start_id_written = 0;
    uint32_t current_audio_bytes = 0;
    int last_printed_second = -1;
    int max_start_id_frames = 300;
    g_rel_frames = 0;

    while ((bytes_read = fread(buf, 1, payload_size, wav)) > 0) {
        if (bytes_read < payload_size) memset(buf + bytes_read, 0, payload_size - bytes_read);

        int do_start_id = (cfg->startid && start_id_written < max_start_id_frames) ? 1 : 0;
        write_dat_frame(fd, buf, sample_rate, cfg->program_number ? pno : 0, do_start_id);
        start_id_written++;

        current_audio_bytes += payload_size;
        uint32_t total_seconds = current_audio_bytes / (sample_rate * channels * 2);

        if ((int)total_seconds != last_printed_second) {
            int h = total_seconds / 3600;
            int m = (total_seconds % 3600) / 60;
            int s = total_seconds % 60;
            printf("\r[WRITING] Track %02d | Time: %02d:%02d:%02d", pno, h, m, s);
            fflush(stdout);
            last_printed_second = total_seconds;
        }
    }
    printf("\n");
    fclose(wav);
}

int execute_record(int fd, const char *cue_file) {
    CueConfig cfg;
    parse_cuefile(cue_file, &cfg);

    // Validate files and display the playlist with durations
    validate_and_print_playlist(&cfg);

    printf("Rewinding tape to the beginning... Please wait.\n");
    struct mtop mt_cmd;
    mt_cmd.mt_op = MTREW;
    mt_cmd.mt_count = 1;
    if (ioctl(fd, MTIOCTOP, &mt_cmd) < 0) {
        perror(" [-] Warning: Failed to rewind tape");
    } else {
        printf(" [+] Tape rewound successfully.\n");
    }

    g_abs_frames = 0;

    write_silence(fd, cfg.leadin_silence, 44100, "LEAD-IN");

    for (int i = 0; i < cfg.file_count; i++) {
        write_wav_file(fd, cfg.files[i], i + 1, &cfg);
        if (i < cfg.file_count - 1 && cfg.intertrack_silence > 0) {
            write_silence(fd, cfg.intertrack_silence, 44100, "INTERTRACK");
        }
    }

    write_silence(fd, cfg.leadout_silence, 44100, "LEAD-OUT");

    mt_cmd.mt_op = MTWEOF; mt_cmd.mt_count = 1;
    ioctl(fd, MTIOCTOP, &mt_cmd);

    printf("\nRecording finished successfully!\n");
    return 0;
}
