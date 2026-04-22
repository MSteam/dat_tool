# DAT Tool (Digital Audio Tape CLI Utility)

A lightweight command-line tool for Linux that lets you extract, play back, and record audio on Digital Audio Tape using computer DDS SCSI tape drives with audio firmware.

## Features

**Extraction and Playback**
- Extract tracks to individual WAV files (`track_01.wav`, `track_02.wav`, ...)
- Real-time playback streamed directly from tape to your sound card
- RAM read-ahead buffer for playback (default 128 MB, configurable) to prevent the tape drive from stopping and searching mid-playback
- Press `q` during playback to stop
- Supports both SP (16-bit PCM) and LP (12-bit non-linear) recordings
- Automatically decodes 32 kHz LP frames to standard 16-bit PCM on extraction and playback
- Displays track number, sample rate, and encoding mode for each track

**Recording**
- Write 16-bit stereo WAV files to tape at 48 kHz, 44.1 kHz, or 32 kHz
- 32 kHz LP mode: 12-bit non-linear encoding packs more audio per frame
- Automatic subcode generation: absolute time, program time, track numbers
- 9-second START-ID markers for automatic track search on consumer DAT decks and Walkmans
- Configurable lead-in, lead-out, and intertrack silence
- CUE sheet format for defining the track list and tape layout
- SCMS bits set to Copy Permit (00) — no DRM

**Drive Detection**
- Reads vendor, model, and firmware version from sysfs at startup
- Reports `[COMPATIBLE]` or `[UNKNOWN]` against a built-in list of known-working drives and firmware versions
- Exits immediately with a clear error if the drive cannot be configured for audio mode

## Requirements

Linux only. Requires `gcc` and `make` to build. For playback you need either `aplay` (alsa-utils) or `play` (sox).

Debian / Ubuntu / Linux Mint:
```
sudo apt install build-essential alsa-utils sox
```

Arch Linux / Manjaro:
```
sudo pacman -S base-devel alsa-utils sox
```

## Compatible Drives

The following drives have been verified to work with audio firmware. The tool reports compatibility status at startup for both the drive model and firmware version.

**Archive 4320NT / 4330**
Vendor: ARCHIVE, Product: Python 25601-XXX
Compatible firmware: 2.63, 2.75

**Conner/Seagate CTD-8000 / 4326NT**
Vendor: ARCHIVE, Product: Python 01931-XXX
Compatible firmware: 5AC, 5.56, 5.63

**Conner 4530**
Vendor: ARCHIVE, Product: Python 25501-XXX
Compatible firmware: any

**Sony SDT-9000**
Vendor: SONY, Product: SDT-9000
Compatible firmware: 12.2, 13.1

`XXX` in the product ID is a 3-character hardware variant code that varies by unit — all variants of a listed model are treated as compatible.

These drives must have audio firmware installed. A standard DDS data firmware will reject the audio density code (0x80) and the tool will exit with an error.

## Compilation

```
git clone https://github.com/MSteam/dat_tool
cd dat_tool
make
```

## Usage

Your user needs read/write access to the tape device (usually `/dev/st0` or `/dev/nst0`). Add yourself to the `tape` group or use `sudo`.

### Extract tracks to disk

Reads the tape and saves each track as a WAV file in the current directory.

```
./dat_tool save /dev/st0
```

### Live playback

Streams audio from the tape directly to your sound card. The tape is read into a RAM buffer first to prevent mechanical stops during playback.

```
./dat_tool play /dev/st0
./dat_tool play 256M /dev/st0
./dat_tool play 1G /dev/st0
```

The buffer size is optional and defaults to 128 MB. Specify it before the device path using K, M, or G suffixes. Press `q` to stop.

### Record audio to tape

Records audio tracks defined in a CUE configuration file.

```
./dat_tool write /dev/st0 tape.cue
```

## The CUE Configuration File

Create a `tape.cue` file before recording. It defines the track list and tape parameters.

```ini
[CONFIG]
STARTID=ON
PROGRAM_NUMBER=ON
LP_MODE=OFF
LEADIN_SILENCE=5
LEADOUT_SILENCE=3
INTERTRACK_SILENCE=3

[FILES]
FILE_001=track1.wav
FILE_002=track2.wav
FILE_003=track3.wav
```

**STARTID** — ON or OFF. Writes a 9-second Start-ID marker at the beginning of each track, enabling automatic track search on consumer DAT decks and Walkmans.

**PROGRAM_NUMBER** — ON or OFF. Writes sequential track numbers (01, 02, 03...) into the subcode.

**LP_MODE** — ON or OFF. Enables 32 kHz LP recording with 12-bit non-linear encoding. All input files must be 32 kHz when this is on. See LP Mode section below.

**LEADIN_SILENCE** — seconds of digital silence before the first track.

**LEADOUT_SILENCE** — seconds of digital silence after the last track.

**INTERTRACK_SILENCE** — seconds of digital silence between tracks.

### Input file requirements

For SP mode (LP_MODE=OFF): uncompressed PCM WAV, 16-bit, stereo, at 48000, 44100, or 32000 Hz.

For LP mode (LP_MODE=ON): uncompressed PCM WAV, 16-bit, stereo, at 32000 Hz only.

## LP Mode

`LP_MODE=ON` enables 12-bit non-linear encoding at 32 kHz. Each tape frame encodes 1920 stereo sample pairs (7680 bytes of 16-bit input PCM) into 5760 bytes using logarithmic companding. This doubles the recording capacity compared to SP 32 kHz mode.

The frame rate changes from 33.33 fps (SP) to 16.67 fps (LP), and subcode timecode adjusts automatically. The encoding flag in Main-ID signals the format to the drive.

On extraction and playback, LP frames are automatically decoded back to 16-bit PCM.

## Credits

- LP 32 kHz encoding/decoding tables adapted from **DATlib** © 1995–1996 Marcus Meissner, Friedrich-Alexander Universität Erlangen-Nürnberg (FAU).
- Developed with assistance from Google Gemini AI and Anthropic Claude AI.
