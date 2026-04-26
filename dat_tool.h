#ifndef DAT_TOOL_H
#define DAT_TOOL_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#define FRAME_SIZE    5822
#define DATA_SIZE     5760
#define LP_INPUT_SIZE 7680  // bytes of 16-bit PCM consumed per LP frame (1920 stereo pairs)
#define MAX_BATCH     32

#pragma pack(push, 1)
typedef struct {
    char riff[4];
    uint32_t overall_size;
    char wave[4];
    char fmt_chunk_marker[4];
    uint32_t length_of_fmt;
    uint16_t format_type;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byterate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_chunk_header[4];
    uint32_t data_size;
} WavHeader;
#pragma pack(pop)

typedef struct {
    int startid;
    int program_number;
    int leadin_silence;
    int leadout_silence;
    int intertrack_silence;
    int lp_mode;
    char files[99][512];
    int file_count;
} CueConfig;

// Shared LP scatter/gather table (same for encode and decode). Computed once.
extern short g_lp_scatter[DATA_SIZE];
void lp_init_scatter(void);

void configure_tape_drive(int fd);

// Mode dispatch
int execute_play  (int fd, size_t buffer_size, int dat_batch);
int execute_save  (int fd, const char *prefix, size_t buffer_size, int dat_batch);
int execute_record(int fd, const char *cue_file, size_t buffer_size, int dat_batch);

#endif
