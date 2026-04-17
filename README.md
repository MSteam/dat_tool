# DAT Tool (Digital Audio Tape CLI Utility)

A lightweight, powerful command-line tool for Linux that allows you to read, extract, and bit-perfectly record standard PCM audio to Digital Audio Tape (DAT) using computer DDS (Digital Data Storage) SCSI tape drives with audio firmware. 

## Features

* **Bit-Perfect Recording:** Write standard 16-bit Stereo WAV files directly to DAT tapes at 48kHz, 44.1kHz, or 32kHz (Standard Play).
* **Automatic Subcode Generation:** Fully handles DAT subcodes during recording, including:
  * Absolute Time (continuous tape timer).
  * Program Time (individual track timer).
  * Exact Track Numbering (01, 02, 03...).
  * 9-second `START-ID` markers for automatic track seeking on consumer DAT Walkmans and decks.
* **CUE Sheet Orchestration:** Easily configure playlists, lead-in/lead-out gaps, and intertrack silence using a simple text-based configuration file.
* **Audio Extraction & Playback:** Dump raw DAT audio tracks to your hard drive or stream them directly from the tape to your sound card in real-time.
* **SCMS-Free:** Automatically sets the Serial Copy Management System bits to `00` (Copy Permit), ensuring your tapes are DRM-free.

## Limitations

* **No Long Play (LP / Mode III) Support:** This tool strictly operates in Standard Play (SP) mode. Standard DDS computer drives spin the head drum at 2000 RPM. The DAT LP specification requires a physical drum speed of 1000 RPM and 12-bit non-linear compression, which generic data streamers do not support natively.
* **Input Format:** Files prepared for recording *must* be uncompressed PCM WAV, 16-bit, 2-channel (Stereo).
* **Sample Rates:** Only standard DAT sample rates are supported: `48000 Hz`, `44100 Hz`, or `32000 Hz`.

## Requirements & Dependencies

This tool is built for Linux and interacts directly with SCSI tape devices (`/dev/st0`). 

To compile the source code, you need a standard C compiler (`gcc`) and `make`. For real-time tape playback, you need either `aplay` (from alsa-utils) or `play` (from sox).

**Debian / Ubuntu / Linux Mint:**
`sudo apt update`
`sudo apt install build-essential alsa-utils sox`

**Arch Linux / Manjaro:**
`sudo pacman -S base-devel alsa-utils sox`

## Compilation

Clone the repository and run `make`:

`git clone https://github.com/MSteam/dat_tool`
`cd dat_tool`
`make`

## Usage

Ensure your user has read/write permissions to the tape drive (usually `/dev/st0` or `/dev/nst0`). You may need to add your user to the `tape` group or run the tool with `sudo`.

### 1. Extract Tracks to Disk
Reads the tape and saves audio as `track_01.wav`, `track_02.wav`, etc., in the current directory.
`./dat_tool save /dev/st0`

### 2. Live Playback
Streams the audio directly from the tape drive to your sound card.
`./dat_tool play /dev/st0`

### 3. Record Audio to Tape
Records audio to the tape based on a configuration file.
`./dat_tool write /dev/st0 tape.cue`

## The CUE Configuration File (`tape.cue`)

Before recording, you need to create a `tape.cue` file in the same directory. This file dictates the structure of your tape. 

**Example `tape.cue`:**
[CONFIG]
STARTID=ON
PROGRAM_NUMBER=ON
LEADIN_SILENCE=5
LEADOUT_SILENCE=3
INTERTRACK_SILENCE=3

[FILES]
FILE_001=track1.wav
FILE_002=track2.wav
FILE_003=track3.wav

### Configuration Parameters:
* `STARTID=ON/OFF`: Automatically generate a 9-second Start-ID marker at the beginning of each track.
* `PROGRAM_NUMBER=ON/OFF`: Write sequential track numbers into the subcode.
* `LEADIN_SILENCE`: Seconds of digital silence to write before the first track.
* `LEADOUT_SILENCE`: Seconds of digital silence to write after the final track.
* `INTERTRACK_SILENCE`: Seconds of digital silence to insert between tracks.

## Credits & Acknowledgements

* Original reverse engineering and hardware interface inspired by various DAT and SCSI tape documentation.
* The source code logic, BCD subcode packing, and C implementation were developed with the assistance of Google's **Gemini AI**, transforming complex DAT specifications into a modern, robust C utility.
