#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dat_tool.h"

int check_playback_dependencies() {
    if (system("command -v aplay >/dev/null 2>&1") == 0) return 1;
    if (system("command -v play >/dev/null 2>&1") == 0) return 2;
    fprintf(stderr, "\n[ERROR] No audio playback utility found ('aplay' or 'play').\n");
    return 0;
}

void write_wav_header(FILE *f, int channels, int sample_rate) {
    WavHeader hdr = {
        .riff = {'R','I','F','F'}, .overall_size = 36, .wave = {'W','A','V','E'},
        .fmt_chunk_marker = {'f','m','t',' '}, .length_of_fmt = 16, .format_type = 1,
        .channels = channels, .sample_rate = sample_rate,
        .byterate = sample_rate * channels * 2, .block_align = channels * 2,
        .bits_per_sample = 16, .data_chunk_header = {'d','a','t','a'}, .data_size = 0
    };
    fwrite(&hdr, sizeof(WavHeader), 1, f);
}

int execute_extract_or_play(int fd, int mode_play) {
    int player_type = 0;
    if (mode_play) {
        player_type = check_playback_dependencies();
        if (player_type == 0) return 1;
    }

    unsigned char frame[FRAME_SIZE];
    int frame_counter = 0, current_track = -1, last_printed_second = -1;
    uint32_t current_audio_bytes = 0;
    FILE *current_wav = NULL, *audio_pipe = NULL;

    printf("Starting %s...\n", mode_play ? "playback" : "extraction");

    while (read(fd, frame, FRAME_SIZE) == FRAME_SIZE) {
        unsigned char *scode = frame + DATA_SIZE;
        unsigned char *subid = scode + 56;
        unsigned char *mainid = subid + 4;

        int channels = ((mainid[0] >> 0) & 0x3) == 1 ? 4 : 2;
        int samplerate_raw = (mainid[0] >> 2) & 0x3;
        int encoding = (mainid[1] >> 6) & 0x3;

        if (encoding != 0) continue; // Пропуск 12-bit LP

        int sample_rate = 48000;
        if (samplerate_raw == 1) sample_rate = 44100;
        else if (samplerate_raw == 2) sample_rate = 32000;

        int track_num = ((subid[1] >> 4) & 0xf) * 100 + ((subid[2] >> 4) & 0xf) * 10 + (subid[2] & 0xf);

        if (track_num > 0 && track_num != current_track && track_num < 0xAA) {
            if (current_wav) {
                uint32_t size = 36 + current_audio_bytes;
                fseek(current_wav, 4, SEEK_SET); fwrite(&size, 4, 1, current_wav);
                fseek(current_wav, 40, SEEK_SET); fwrite(&current_audio_bytes, 4, 1, current_wav);
                fclose(current_wav); current_wav = NULL;
                printf("\nTrack %02d saved.\n", current_track);
            }
            if (audio_pipe) { pclose(audio_pipe); audio_pipe = NULL; }

            current_track = track_num;
            current_audio_bytes = 0; last_printed_second = -1;

            if (mode_play) {
                char cmd[256];
                if (player_type == 1) snprintf(cmd, sizeof(cmd), "aplay -t raw -f S16_LE -c %d -r %d -q", channels, sample_rate);
                else snprintf(cmd, sizeof(cmd), "play -q -t raw -r %d -e signed-integer -b 16 -c %d -L - 2>/dev/null", sample_rate, channels);
                audio_pipe = popen(cmd, "w");
            } else {
                char fname[256]; snprintf(fname, sizeof(fname), "track_%02d.wav", current_track);
                current_wav = fopen(fname, "wb");
                if (current_wav) write_wav_header(current_wav, channels, sample_rate);
            }
        }

        int payload_size = (sample_rate == 48000) ? 5760 : (sample_rate == 44100 ? 5292 : 3840);
        if (mode_play && audio_pipe) fwrite(frame, 1, payload_size, audio_pipe);
        else if (!mode_play && current_wav) fwrite(frame, 1, payload_size, current_wav);

        current_audio_bytes += payload_size;
        uint32_t t_sec = current_audio_bytes / (sample_rate * channels * 2);
        if ((int)t_sec != last_printed_second) {
            printf("\r[%s] Track %02d | Time: %02d:%02d:%02d", mode_play ? "PLAYING" : "SAVING", current_track, t_sec/3600, (t_sec%3600)/60, t_sec%60);
            fflush(stdout); last_printed_second = t_sec;
        }
        frame_counter++;
    }
    printf("\nFinished. Frames: %d\n", frame_counter);
    return 0;
}
