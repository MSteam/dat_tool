#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <termios.h>
#include <errno.h>
#include <time.h>
#include "dat_tool.h"

#define DEFAULT_BUFFER_SIZE (4UL * 1024 * 1024)

// --- LP 32kHz (12-bit) decode table ---
// Adapted from DATlib (c) 1995-1996 Marcus Meissner, FAU IMMD4

static short lp_conv_12_to_16[4096];
static int   lp_decode_inited = 0;

static int lp_c_12_to_16(int arg) {
    int cnv = arg;
    if (cnv >= 0x800)  cnv = arg - 0x1000;
    if (cnv >= 1792)   return (cnv-1536)*64;
    if (cnv >= 1536)   return (cnv-1280)*32;
    if (cnv >= 1280)   return (cnv-1024)*16;
    if (cnv >= 1024)   return (cnv-768)*8;
    if (cnv >= 768)    return (cnv-512)*4;
    if (cnv >= 512)    return (cnv-256)*2;
    if (cnv >= 0)      return cnv;
    if (cnv >= -512)   return cnv;
    if (cnv >= -768)   return (cnv+256)*2;
    if (cnv >= -1024)  return (cnv+512)*4;
    if (cnv >= -1280)  return (cnv+768)*8;
    if (cnv >= -1536)  return (cnv+1024)*16;
    if (cnv >= -1792)  return (cnv+1280)*32;
    return (cnv+1536)*64;
}

static void lp_decode_init(void) {
    if (lp_decode_inited) return;
    lp_init_scatter();
    for (int i = 0; i < 4096; i++)
        lp_conv_12_to_16[i] = (short)lp_c_12_to_16(i);
    lp_decode_inited = 1;
}

static void decode_lp_frame(const unsigned char *audio, unsigned char *pcm_out) {
    short *out = (short *)pcm_out;
    for (int i = 0; i < DATA_SIZE; i += 3) {
        int b0 = audio[g_lp_scatter[i]];
        int b1 = audio[g_lp_scatter[i + 1]];
        int b2 = audio[g_lp_scatter[i + 2]];
        int l12 = (b0 << 4) | (b1 >> 4);
        int r12 = (b2 << 4) | (b1 & 0x0f);
        *out++ = lp_conv_12_to_16[l12 & 0xfff];
        *out++ = lp_conv_12_to_16[r12 & 0xfff];
    }
}

// --- Shared helpers ---

static int check_playback_dependencies(void) {
    if (system("command -v aplay >/dev/null 2>&1") == 0) return 1;
    if (system("command -v play  >/dev/null 2>&1") == 0) return 2;
    fprintf(stderr, "\n[ERROR] No audio playback utility found ('aplay' or 'play').\n");
    return 0;
}

static void write_wav_header(FILE *f, int channels, int sample_rate) {
    WavHeader hdr = {
        .riff = {'R','I','F','F'}, .overall_size = 36, .wave = {'W','A','V','E'},
        .fmt_chunk_marker = {'f','m','t',' '}, .length_of_fmt = 16, .format_type = 1,
        .channels = channels, .sample_rate = sample_rate,
        .byterate = sample_rate * channels * 2, .block_align = channels * 2,
        .bits_per_sample = 16, .data_chunk_header = {'d','a','t','a'}, .data_size = 0
    };
    fwrite(&hdr, sizeof(WavHeader), 1, f);
}

static void finalize_wav(FILE *f, uint32_t audio_bytes) {
    uint32_t size = 36 + audio_bytes;
    fseek(f, 4,  SEEK_SET); fwrite(&size,        4, 1, f);
    fseek(f, 40, SEEK_SET); fwrite(&audio_bytes, 4, 1, f);
    fclose(f);
}

static FILE *open_audio_pipe(int player_type, int channels, int sample_rate) {
    char cmd[256];
    if (player_type == 1)
        snprintf(cmd, sizeof(cmd),
                 "aplay -t raw -f S16_LE -c %d -r %d -q", channels, sample_rate);
    else
        snprintf(cmd, sizeof(cmd),
                 "play -q -t raw -r %d -e signed-integer -b 16 -c %d -L - 2>/dev/null",
                 sample_rate, channels);
    return popen(cmd, "w");
}

// --- Read-side ring buffer (tape producer, app consumer) ---

typedef struct {
    unsigned char  *data;
    size_t          cap;
    size_t          head;   // monotonic
    size_t          tail;
    int             batch;  // frames per read() syscall
    int             eof;
    int             fd;
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  avail;
    pthread_cond_t  space;
} ReadRing;

static void unlock_cleanup(void *arg) {
    pthread_mutex_unlock((pthread_mutex_t *)arg);
}

static void *tape_reader_thread(void *arg) {
    ReadRing *r = (ReadRing *)arg;
    unsigned char buf[MAX_BATCH * FRAME_SIZE];
    int    batch_n = r->batch;
    size_t req     = (size_t)batch_n * FRAME_SIZE;

    for (;;) {
        ssize_t n = read(r->fd, buf, req);  // cancellation point

        if (n <= 0 || (n % FRAME_SIZE) != 0) {
            pthread_mutex_lock(&r->lock);
            r->eof = 1;
            pthread_cond_broadcast(&r->avail);
            pthread_mutex_unlock(&r->lock);
            return NULL;
        }

        size_t frames = (size_t)n / FRAME_SIZE;

        pthread_mutex_lock(&r->lock);
        pthread_cleanup_push(unlock_cleanup, &r->lock);

        for (size_t i = 0; i < frames; i++) {
            while ((r->head - r->tail) >= r->cap)
                pthread_cond_wait(&r->space, &r->lock);  // cancellation point
            memcpy(r->data + (r->head % r->cap) * FRAME_SIZE,
                   buf + i * FRAME_SIZE, FRAME_SIZE);
            r->head++;
        }
        pthread_cond_broadcast(&r->avail);

        pthread_cleanup_pop(1);
    }
}

// Returns 1=frame ready, 0=EOF, -1=timeout
static int ring_get_timed(ReadRing *r, unsigned char *frame, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&r->lock);
    while (r->head == r->tail && !r->eof) {
        if (pthread_cond_timedwait(&r->avail, &r->lock, &ts) == ETIMEDOUT) {
            pthread_mutex_unlock(&r->lock);
            return -1;
        }
    }
    if (r->head == r->tail) { pthread_mutex_unlock(&r->lock); return 0; }
    memcpy(frame, r->data + (r->tail % r->cap) * FRAME_SIZE, FRAME_SIZE);
    r->tail++;
    pthread_cond_signal(&r->space);
    pthread_mutex_unlock(&r->lock);
    return 1;
}

static int ring_init(ReadRing *r, int fd, size_t buffer_size, int batch) {
    if (buffer_size == 0) buffer_size = DEFAULT_BUFFER_SIZE;
    size_t cap_frames = buffer_size / FRAME_SIZE;
    if (cap_frames < 128) cap_frames = 128;

    memset(r, 0, sizeof(*r));
    r->data = malloc(cap_frames * FRAME_SIZE);
    if (!r->data) {
        fprintf(stderr, "[ERROR] Failed to allocate %zu MB read buffer.\n",
                (cap_frames * FRAME_SIZE) / (1024 * 1024));
        return -1;
    }
    r->cap   = cap_frames;
    r->fd    = fd;
    r->batch = (batch >= 1 && batch <= MAX_BATCH) ? batch : 1;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->avail, NULL);
    pthread_cond_init(&r->space, NULL);

    size_t buf_mb = (cap_frames * FRAME_SIZE) / (1024 * 1024);
    printf("Buffer      : %zu MB (%zu frames)\n", buf_mb, cap_frames);

    pthread_create(&r->thread, NULL, tape_reader_thread, r);
    return 0;
}

static void ring_teardown(ReadRing *r) {
    if (r->thread) {
        pthread_cancel(r->thread);
        pthread_join(r->thread, NULL);
    }
    pthread_cond_destroy(&r->avail);
    pthread_cond_destroy(&r->space);
    pthread_mutex_destroy(&r->lock);
    free(r->data);
}

// --- Terminal raw mode (for 'q' to quit in play mode) ---

static struct termios g_saved_termios;
static int g_raw_mode = 0;

static void leave_raw_mode(void) {
    if (g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
        g_raw_mode = 0;
    }
}

static void enter_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &g_saved_termios);
    struct termios raw = g_saved_termios;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    g_raw_mode = 1;
    atexit(leave_raw_mode);
}

static int read_key(void) {
    if (!g_raw_mode) return -1;
    unsigned char c;
    return (read(STDIN_FILENO, &c, 1) == 1) ? (int)c : -1;
}

// --- Shared subcode parsing ---

typedef struct {
    int  channels;
    int  sample_rate;
    int  is_lp;
    int  track_num;
} FrameInfo;

static void parse_frame_subcode(const unsigned char *frame, FrameInfo *out) {
    const unsigned char *scode  = frame + DATA_SIZE;
    const unsigned char *subid  = scode + 56;
    const unsigned char *mainid = subid + 4;
    int sr_raw = (mainid[0] >> 2) & 0x03;
    out->channels    = ((mainid[0] & 0x03) == 1) ? 4 : 2;
    out->sample_rate = (sr_raw == 1) ? 44100 : (sr_raw == 2) ? 32000 : 48000;
    out->is_lp       = (mainid[1] >> 6) & 0x01;
    out->track_num   = ((subid[1] >> 4) & 0xf) * 100
                     + ((subid[2] >> 4) & 0xf) * 10
                     +  (subid[2] & 0xf);
}

static int frame_audio_bytes(const FrameInfo *fi) {
    if (fi->is_lp) return LP_INPUT_SIZE;
    return (fi->sample_rate == 48000) ? 5760 :
           (fi->sample_rate == 44100) ? 5292 : 3840;
}

// --- execute_save ---

int execute_save(int fd, const char *prefix, size_t buffer_size, int dat_batch) {
    lp_decode_init();

    ReadRing ring;
    if (ring_init(&ring, fd, buffer_size, dat_batch) < 0) return 1;
    printf("Starting extraction with prefix '%s'...\n", prefix);

    unsigned char  frame[FRAME_SIZE];
    unsigned char  lp_pcm[LP_INPUT_SIZE];
    int            frame_counter       = 0;
    int            current_track       = -1;
    int            current_sample_rate = 0;
    int            current_channels    = 2;
    int            last_printed_second = -1;
    uint32_t       current_audio_bytes = 0;
    FILE          *current_wav         = NULL;

    for (;;) {
        int got = ring_get_timed(&ring, frame, 200);
        if (got == 0) break;      // EOF
        if (got == -1) continue;  // timeout

        FrameInfo fi;
        parse_frame_subcode(frame, &fi);

        if (fi.track_num > 0 && fi.track_num != current_track && fi.track_num < 0xAA) {
            if (current_wav) {
                finalize_wav(current_wav, current_audio_bytes);
                printf("\nTrack %02d saved.\n", current_track);
                current_wav = NULL;
            }

            current_track       = fi.track_num;
            current_sample_rate = fi.sample_rate;
            current_channels    = fi.channels;
            current_audio_bytes = 0;
            last_printed_second = -1;

            const char *rate_str = (fi.sample_rate == 48000) ? "48 kHz" :
                                   (fi.sample_rate == 44100) ? "44.1 kHz" : "32 kHz";
            const char *mode_str = fi.is_lp ? "LP (12-bit)" : "SP";
            char fname[512];
            snprintf(fname, sizeof(fname), "%s_%02d.wav", prefix, current_track);
            printf("\n[Track %02d] %s %s -> Saving to %s\n",
                   fi.track_num, rate_str, mode_str, fname);
            current_wav = fopen(fname, "wb");
            if (current_wav)
                write_wav_header(current_wav, fi.channels, fi.sample_rate);
        }

        if (current_track < 0) { frame_counter++; continue; }

        const unsigned char *out_data;
        int out_bytes = frame_audio_bytes(&fi);
        if (fi.is_lp) {
            decode_lp_frame(frame, lp_pcm);
            out_data = lp_pcm;
        } else {
            out_data = frame;
        }

        if (current_wav) fwrite(out_data, 1, out_bytes, current_wav);
        current_audio_bytes += out_bytes;

        uint32_t t_sec = current_audio_bytes /
                         ((uint32_t)current_sample_rate * current_channels * 2);
        if ((int)t_sec != last_printed_second) {
            printf("\r[SAVING] Track %02d | Time: %02d:%02d:%02d",
                   current_track, t_sec / 3600, (t_sec % 3600) / 60, t_sec % 60);
            fflush(stdout);
            last_printed_second = (int)t_sec;
        }

        frame_counter++;
    }

    if (current_wav) {
        finalize_wav(current_wav, current_audio_bytes);
        printf("\nTrack %02d saved.\n", current_track);
    }

    printf("\nFinished. Frames read: %d\n", frame_counter);
    ring_teardown(&ring);
    return 0;
}

// --- execute_play ---

int execute_play(int fd, size_t buffer_size, int dat_batch) {
    lp_decode_init();

    int player_type = check_playback_dependencies();
    if (!player_type) return 1;

    ReadRing ring;
    if (ring_init(&ring, fd, buffer_size, dat_batch) < 0) return 1;
    printf("Starting playback... [press q to quit]\n");

    enter_raw_mode();

    unsigned char  frame[FRAME_SIZE];
    unsigned char  lp_pcm[LP_INPUT_SIZE];
    int            frame_counter       = 0;
    int            current_track       = -1;
    int            current_sample_rate = 0;
    int            current_channels    = 2;
    int            last_printed_second = -1;
    uint32_t       current_audio_bytes = 0;
    FILE          *audio_pipe          = NULL;
    int            quit                = 0;

    for (;;) {
        int got = ring_get_timed(&ring, frame, 100);
        if (quit || got == 0) break;

        if (got == -1) {
            if (read_key() == 'q') quit = 1;
            continue;
        }

        FrameInfo fi;
        parse_frame_subcode(frame, &fi);

        if (fi.track_num > 0 && fi.track_num != current_track && fi.track_num < 0xAA) {
            if (audio_pipe) { pclose(audio_pipe); audio_pipe = NULL; }

            current_track       = fi.track_num;
            current_sample_rate = fi.sample_rate;
            current_channels    = fi.channels;
            current_audio_bytes = 0;
            last_printed_second = -1;

            const char *rate_str = (fi.sample_rate == 48000) ? "48 kHz" :
                                   (fi.sample_rate == 44100) ? "44.1 kHz" : "32 kHz";
            const char *mode_str = fi.is_lp ? "LP (12-bit)" : "SP";
            printf("\n[Track %02d] %s %s -> Playing\n",
                   fi.track_num, rate_str, mode_str);

            audio_pipe = open_audio_pipe(player_type, fi.channels, fi.sample_rate);
        }

        if (current_track < 0) { frame_counter++; goto check_keys; }

        const unsigned char *out_data;
        int out_bytes = frame_audio_bytes(&fi);
        if (fi.is_lp) {
            decode_lp_frame(frame, lp_pcm);
            out_data = lp_pcm;
        } else {
            out_data = frame;
        }

        if (audio_pipe) fwrite(out_data, 1, out_bytes, audio_pipe);
        current_audio_bytes += out_bytes;

        {
            uint32_t t_sec = current_audio_bytes /
                             ((uint32_t)current_sample_rate * current_channels * 2);
            if ((int)t_sec != last_printed_second) {
                printf("\r[PLAYING] Track %02d | Time: %02d:%02d:%02d",
                       current_track,
                       t_sec / 3600, (t_sec % 3600) / 60, t_sec % 60);
                fflush(stdout);
                last_printed_second = (int)t_sec;
            }
        }

        frame_counter++;

check_keys:
        if (read_key() == 'q') quit = 1;
    }

    leave_raw_mode();

    if (audio_pipe) pclose(audio_pipe);
    printf("\nFinished. Frames played: %d\n", frame_counter);

    ring_teardown(&ring);
    return 0;
}
