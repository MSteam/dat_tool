#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mtio.h>
#include "dat_tool.h"

int g_abs_frames = 0; // Absolute frame counter
int g_rel_frames = 0; // Relative frame counter

#define DEFAULT_BUFFER_SIZE (4UL * 1024 * 1024)

// --- Write ring buffer (keeps drive's internal buffer full on slow links) ---

#define MAX_BATCH 32

typedef struct {
    unsigned char  *data;
    size_t          cap;     // frames
    size_t          head;    // monotonic frames pushed
    size_t          tail;    // monotonic frames written
    int             batch;   // frames per write() syscall (1..MAX_BATCH)
    volatile int    eof;
    volatile int    err;
    int             fd;
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  avail;
    pthread_cond_t  space;
} WriteRing;

static WriteRing *g_write_ring = NULL;

static void *tape_writer_thread(void *arg) {
    WriteRing *r = (WriteRing *)arg;
    unsigned char buf[MAX_BATCH * FRAME_SIZE];
    int batch_n = r->batch;

    for (;;) {
        pthread_mutex_lock(&r->lock);
        while (r->head == r->tail && !r->eof)
            pthread_cond_wait(&r->avail, &r->lock);
        if (r->head == r->tail) {
            pthread_mutex_unlock(&r->lock);
            break;
        }
        size_t avail = r->head - r->tail;
        size_t take  = (avail < (size_t)batch_n) ? avail : (size_t)batch_n;
        for (size_t i = 0; i < take; i++) {
            memcpy(buf + i * FRAME_SIZE,
                   r->data + ((r->tail + i) % r->cap) * FRAME_SIZE,
                   FRAME_SIZE);
        }
        r->tail += take;
        pthread_cond_broadcast(&r->space);
        pthread_mutex_unlock(&r->lock);

        size_t bytes = take * FRAME_SIZE;
        if ((size_t)write(r->fd, buf, bytes) != bytes) {
            pthread_mutex_lock(&r->lock);
            r->err = 1;
            pthread_cond_broadcast(&r->space);
            pthread_mutex_unlock(&r->lock);
            return NULL;
        }
    }
    return NULL;
}

static int ring_put(WriteRing *r, const unsigned char *frame) {
    pthread_mutex_lock(&r->lock);
    while ((r->head - r->tail) >= r->cap && !r->err)
        pthread_cond_wait(&r->space, &r->lock);
    if (r->err) { pthread_mutex_unlock(&r->lock); return -1; }
    memcpy(r->data + (r->head % r->cap) * FRAME_SIZE, frame, FRAME_SIZE);
    r->head++;
    pthread_cond_signal(&r->avail);
    pthread_mutex_unlock(&r->lock);
    return 0;
}

static int ring_finish(WriteRing *r) {
    pthread_mutex_lock(&r->lock);
    r->eof = 1;
    pthread_cond_signal(&r->avail);
    pthread_mutex_unlock(&r->lock);
    pthread_join(r->thread, NULL);
    return r->err ? -1 : 0;
}

// --- LP 32kHz (12-bit non-linear) encoding ---
// Uses g_lp_scatter from main.c as the scatter table (same table for encode and decode).
// Adapted from DATlib (c) 1995-1996 Marcus Meissner, FAU IMMD4

static short lp_conv_16_to_12[65536];   // 16-bit linear -> 12-bit non-linear lookup
static int   lp_encode_inited = 0;

static int lp_c_16_to_12(int arg) {
    int cnv = arg;
    if (cnv > 0x8000)  cnv -= 0x10000;
    if (cnv >=  0x4000) return  cnv/64  + 1536;
    if (cnv >=  0x2000) return  cnv/32  + 1280;
    if (cnv >=  0x1000) return  cnv/16  + 1024;
    if (cnv >=  0x0800) return  cnv/8   +  768;
    if (cnv >=  0x0400) return  cnv/4   +  512;
    if (cnv >=  0x0200) return  cnv/2   +  256;
    if (cnv >= -0x0200) return  cnv;
    if (cnv >= -0x0400) return (cnv+1)/2  -  257;
    if (cnv >= -0x0800) return (cnv+1)/4  -  513;
    if (cnv >= -0x1000) return (cnv+1)/8  -  769;
    if (cnv >= -0x2000) return (cnv+1)/16 - 1025;
    if (cnv >= -0x4000) return (cnv+1)/32 - 1281;
    return (cnv+1)/64 - 1537;
}

static void lp_encode_init(void) {
    if (lp_encode_inited) return;
    lp_init_scatter();
    for (int i = 0; i < 65536; i++)
        lp_conv_16_to_12[i] = (short)lp_c_16_to_12(i);
    lp_encode_inited = 1;
}

static void encode_lp_frame(const unsigned char *pcm_in, unsigned char *frame_out) {
    const unsigned short *inbuf = (const unsigned short *)pcm_in;
    int j = 0;
    memset(frame_out, 0, DATA_SIZE);
    for (int i = 0; i < LP_INPUT_SIZE / 2; i += 2) {
        int left  = lp_conv_16_to_12[inbuf[i]];
        int right = lp_conv_16_to_12[inbuf[i + 1]];
        frame_out[g_lp_scatter[j++]] = (unsigned char)(left >> 4);
        frame_out[g_lp_scatter[j++]] = (unsigned char)(((left & 0xf) << 4) | (right & 0xf));
        frame_out[g_lp_scatter[j++]] = (unsigned char)(right >> 4);
    }
}

// Timecode for LP 32kHz: 1920 samples/frame at 32kHz = 50 frames per 3 seconds (16.67 fps)
static void frames_to_time_lp(int total_frames, int *h, int *m, int *s, int *f) {
    int blocks = total_frames / 50;
    int rem    = total_frames % 50;
    int total_sec = blocks * 3;
    if      (rem < 17) { *f = rem;       }
    else if (rem < 34) { total_sec += 1; *f = rem - 17; }
    else               { total_sec += 2; *f = rem - 34; }
    *h = total_sec / 3600;
    *m = (total_sec % 3600) / 60;
    *s = total_sec % 60;
}

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
            else if (strstr(line, "LP_MODE=ON")) cfg->lp_mode = 1;
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
    printf(" RECORDING MODE     : %s\n", cfg->lp_mode ? "32kHz LP (12-bit)" : "Standard Play (SP)");
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
        if (cfg->lp_mode && hdr.sample_rate != 32000) {
            fprintf(stderr, "\n[FATAL ERROR] LP_MODE=ON requires 32000 Hz input, but '%s' is %d Hz.\n", cfg->files[i], hdr.sample_rate);
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

void write_dat_frame(int fd, unsigned char *audio_data, int sample_rate, int pno, int start_id, int lp_mode) {
    unsigned char frame[FRAME_SIZE];
    memset(frame, 0, FRAME_SIZE);

    if (lp_mode) {
        encode_lp_frame(audio_data, frame);
    } else {
        int payload_size = (sample_rate == 48000) ? 5760 : (sample_rate == 44100 ? 5292 : 3840);
        memcpy(frame, audio_data, payload_size);
    }

    unsigned char *scode = frame + DATA_SIZE;
    unsigned char *packs = scode;
    unsigned char *subid = scode + 56;
    unsigned char *mainid = subid + 4;

    int a_h, a_m, a_s, a_f;
    int r_h, r_m, r_s, r_f;
    if (lp_mode) {
        frames_to_time_lp(g_abs_frames, &a_h, &a_m, &a_s, &a_f);
        frames_to_time_lp(g_rel_frames, &r_h, &r_m, &r_s, &r_f);
    } else {
        frames_to_time(g_abs_frames, &a_h, &a_m, &a_s, &a_f);
        frames_to_time(g_rel_frames, &r_h, &r_m, &r_s, &r_f);
    }

    write_pack(&packs[0],  1, pno, r_h, r_m, r_s, r_f);
    write_pack(&packs[8],  2, pno, a_h, a_m, a_s, a_f);
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

    // --- Main-ID Setup ---
    int sr_bits = 0;
    if (sample_rate == 44100) sr_bits = 1;
    else if (sample_rate == 32000) sr_bits = 2;
    mainid[0] = (unsigned char)((sr_bits << 2) | 0x00);
    mainid[1] = lp_mode ? 0x40 : 0x00;  // bit 6 = encoding: 0=16-bit SP, 1=12-bit LP

    if (g_write_ring) {
        if (ring_put(g_write_ring, frame) < 0) {
            fprintf(stderr, "\n\n[FATAL ERROR] Tape drive rejected the write command\n");
            exit(1);
        }
    } else {
        if (write(fd, frame, FRAME_SIZE) != FRAME_SIZE) {
            perror("\n\n[FATAL ERROR] Tape drive rejected the write command");
            exit(1);
        }
    }

    g_abs_frames++;
    g_rel_frames++;
}

void write_silence(int fd, int seconds, int sample_rate, const char *zone, int lp_mode) {
    if (seconds <= 0) return;

    g_rel_frames = 0;
    printf("Writing %d sec %s silence...\n", seconds, zone);

    // LP: 50 frames/3 s (16.67 fps);  SP: 100 frames/3 s (33.33 fps)
    int total_frames = lp_mode ? (seconds * 50) / 3 : (seconds * 100) / 3;
    unsigned char empty[LP_INPUT_SIZE];
    memset(empty, 0, sizeof(empty));
    int last_printed_sec = -1;

    for (int i = 0; i < total_frames; i++) {
        write_dat_frame(fd, empty, sample_rate, 0, 0, lp_mode);

        int current_sec = lp_mode ? (i * 3) / 50 : (i * 3) / 100;
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
    int lp_mode = cfg->lp_mode;

    // LP mode consumes 7680 bytes of PCM per frame (1920 stereo pairs at 32kHz)
    int read_size;
    if (lp_mode) {
        read_size = LP_INPUT_SIZE;
    } else {
        read_size = (sample_rate == 48000) ? 5760 : (sample_rate == 44100 ? 5292 : 3840);
    }

    unsigned char buf[LP_INPUT_SIZE];
    int bytes_read;
    int start_id_written = 0;
    uint32_t current_audio_bytes = 0;
    int last_printed_second = -1;
    int max_start_id_frames = lp_mode ? 150 : 300;  // ~9 sec worth at respective frame rates
    g_rel_frames = 0;

    while ((bytes_read = fread(buf, 1, read_size, wav)) > 0) {
        if (bytes_read < read_size) memset(buf + bytes_read, 0, read_size - bytes_read);

        int do_start_id = (cfg->startid && start_id_written < max_start_id_frames) ? 1 : 0;
        write_dat_frame(fd, buf, sample_rate, cfg->program_number ? pno : 0, do_start_id, lp_mode);
        start_id_written++;

        current_audio_bytes += bytes_read;
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

int execute_record(int fd, const char *cue_file, size_t buffer_size, int dat_batch) {
    CueConfig cfg;
    parse_cuefile(cue_file, &cfg);

    if (cfg.lp_mode)
        lp_encode_init();

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

    // Set up write-side ring buffer. Producer (this thread) encodes frames
    // and pushes them to the ring; a consumer thread issues back-to-back
    // write() calls to keep the drive's internal buffer full without
    // userspace gaps between SCSI commands.
    WriteRing ring = {0};
    if (buffer_size == 0) buffer_size = DEFAULT_BUFFER_SIZE;
    size_t cap_frames = buffer_size / FRAME_SIZE;
    if (cap_frames < 128) cap_frames = 128;
    ring.data = malloc(cap_frames * FRAME_SIZE);
    if (!ring.data) {
        fprintf(stderr, "[ERROR] Failed to allocate %zu MB write buffer.\n",
                (cap_frames * FRAME_SIZE) / (1024 * 1024));
        return 1;
    }
    ring.cap = cap_frames;
    ring.fd  = fd;
    ring.batch = (dat_batch >= 1 && dat_batch <= MAX_BATCH) ? dat_batch : 1;

    pthread_mutex_init(&ring.lock, NULL);
    pthread_cond_init(&ring.avail, NULL);
    pthread_cond_init(&ring.space, NULL);
    pthread_create(&ring.thread, NULL, tape_writer_thread, &ring);
    g_write_ring = &ring;

    size_t buf_mb = (cap_frames * FRAME_SIZE) / (1024 * 1024);
    printf("Buffer      : %zu MB (%zu frames)\n", buf_mb, cap_frames);
    if (ring.batch > 1)
        printf("Batch mode  : %d frames per SCSI WRITE (%d bytes/block) -- experimental\n",
               ring.batch, ring.batch * FRAME_SIZE);

    g_abs_frames = 0;

    int sr_silence = cfg.lp_mode ? 32000 : 44100;
    write_silence(fd, cfg.leadin_silence, sr_silence, "LEAD-IN", cfg.lp_mode);

    for (int i = 0; i < cfg.file_count; i++) {
        write_wav_file(fd, cfg.files[i], i + 1, &cfg);
        if (i < cfg.file_count - 1 && cfg.intertrack_silence > 0) {
            write_silence(fd, cfg.intertrack_silence, sr_silence, "INTERTRACK", cfg.lp_mode);
        }
    }

    write_silence(fd, cfg.leadout_silence, sr_silence, "LEAD-OUT", cfg.lp_mode);

    // Drain the ring before writing the end-of-data mark
    printf("Flushing write buffer...\n");
    int ring_err = ring_finish(&ring);
    g_write_ring = NULL;
    pthread_cond_destroy(&ring.avail);
    pthread_cond_destroy(&ring.space);
    pthread_mutex_destroy(&ring.lock);
    free(ring.data);

    if (ring_err) {
        fprintf(stderr, "[ERROR] A write error occurred during recording.\n");
        return 1;
    }

    mt_cmd.mt_op = MTWEOF; mt_cmd.mt_count = 1;
    ioctl(fd, MTIOCTOP, &mt_cmd);

    printf("\nRecording finished successfully!\n");
    return 0;
}
