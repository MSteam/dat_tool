#ifndef DAT_TOOL_H
#define DAT_TOOL_H

#include <stdio.h>
#include <stdint.h>

#define FRAME_SIZE 5822
#define DATA_SIZE 5760

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
    char files[99][512];
    int file_count;
} CueConfig;

// Function prototypes
void configure_tape_drive(int fd);
int execute_extract_or_play(int fd, int mode_play);
int execute_record(int fd, const char *cue_file);

#endif
