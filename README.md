# DAT Tool (Digital Audio Tape CLI Utility)

A lightweight command-line tool for Linux that lets you extract, play back, and record audio on Digital Audio Tape using computer DDS SCSI tape drives with audio firmware.

## Features

**Extraction and Playback**
- Extract tracks to individual WAV files with a customizable filename prefix
- Real-time playback streamed directly from tape to your sound card
- RAM read-ahead buffer to decouple disk and audio output from the tape mechanism
- Press `q` during playback to stop
- Supports both SP (16-bit PCM) and LP (12-bit non-linear) recordings
- Automatically decodes 32 kHz LP frames to standard 16-bit PCM
- Displays track number, sample rate, and encoding mode for each track

**Recording**
- Write 16-bit stereo WAV files to tape at 48 kHz, 44.1 kHz, or 32 kHz
- 32 kHz LP mode: 12-bit non-linear encoding packs more audio per frame
- RAM ring buffer + dedicated writer thread to keep the drive's internal buffer fed
- Optional batched writes (`dat_batch=N`) to reduce SCSI command rate on slow links such as USB 1.1 → SCSI bridges
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

General syntax:
```
./dat_tool MODE [params] /dev/st0 [mode-specific args]
```

Optional named parameters appear in any order, before the device path:

- **`dat_batch=N`** — frames per `write()` syscall (1..32, default 1). Effective on **write only**. See "Batched writes" below.
- **`buffer=SIZE`** — RAM ring buffer size, e.g. `4M`, `64M`, `1G`. Default `4M`.

### Extract tracks to disk

Reads the tape and saves each track as a WAV file in the current directory. By default files are named `track_01.wav`, `track_02.wav`, ... You can supply a custom prefix as the last argument.

```
./dat_tool save /dev/st0                  # → track_01.wav, track_02.wav, ...
./dat_tool save /dev/st0 mytape           # → mytape_01.wav, mytape_02.wav, ...
./dat_tool save buffer=16M /dev/st0 album # 16 MB ring buffer, prefix "album"
```

### Live playback

Streams audio from the tape directly to your sound card. The tape is read into a RAM buffer to absorb brief stalls.

```
./dat_tool play /dev/st0
./dat_tool play buffer=64M /dev/st0
```

Press `q` to stop.

### Record audio to tape

Records audio tracks defined in a CUE configuration file.

```
./dat_tool write /dev/st0 tape.cue
./dat_tool write dat_batch=2 /dev/st0 tape.cue
./dat_tool write dat_batch=2 buffer=64M /dev/st0 tape.cue
```

## Batched writes (`dat_batch=N`)

On slow host interfaces such as USB 1.1 → SCSI bridges, the per-command protocol overhead can prevent the host from feeding the drive's internal buffer fast enough at the drive's full write speed. The drive then stops, rewinds slightly, and resumes — so-called "shoe-shining."

`dat_batch=N` makes the writer thread emit `N` DAT frames in a single `write()` syscall. The drive splits each oversized variable-length block into `N` proper 5822-byte DAT audio frames internally, but the host only paid SCSI command overhead once. The output tape is bit-identical to a non-batched recording.

In practice, `dat_batch=2` is often enough to eliminate write-side shoe-shining on USB 1.1 setups. Tested values: 1..32.

A note on reads: `dat_batch` has no effect on `play` or `save`, because audio-firmware DAT drives lock to one frame per SCSI READ command. Reducing read command rate would require raw SCSI passthrough (not currently implemented) or a faster host interface.

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
