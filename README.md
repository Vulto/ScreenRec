# ScreenRec

ScreenRec is a lightweight Linux screen recorder written in C.

It is designed for X11 desktops and focuses on simple command-line operation, efficient screen capture, separate audio tracks, and a self-contained build process.

**Default recording:**

- 60 FPS
- MP4 output
- Desktop audio as a separate track
- Microphone as a separate track
- Automatic video encoder selection
- XShm-based X11 capture

## Features

- Full-screen capture
- Window capture with interactive selection
- Arbitrary area capture with interactive selection
- Individual monitor capture using XRandR
- 60 FPS default capture rate
- Desktop audio recording
- Microphone recording
- Desktop and microphone stored as separate audio tracks
- Automatic video encoder detection and fallback
- Explicit video codec selection
- Automatic output filename generation
- Configurable output directory
- Hardware video encoding through VA-API when available
- Software encoding fallback when hardware encoding is unavailable
- Single-binary static build support

## Requirements

ScreenRec uses the following system components and libraries.

**Build dependencies**

- C compiler with C23 support
- POSIX threads
- X11
- XRandR
- XShm / Xext
- PulseAudio
- FFmpeg:
- libavcodec
- libavformat
- libavutil
- VA-API / libva support for the VA-API integration

The program also uses standard Linux interfaces such as System V shared memory and POSIX APIs.

**Runtime dependencies**

ScreenRec requires:

- An X11 display
- PulseAudio for desktop and microphone capture
- The libraries required by the selected FFmpeg encoder

VA-API is optional at runtime. When a usable VA-API encoder cannot be initialized, ScreenRec automatically attempts another video encoder.

## Building

ScreenRec provides its own configuration and build scripts.

The recommended build process is:

```sh
./configure.sh
./build.sh
```

The default configuration builds a **static executable**.

The configuration script is responsible for:

- Detecting dependencies
- Checking system libraries and headers
- Reusing previously downloaded dependencies
- Building missing static dependencies when necessary
- Generating the final build configuration

After configuration succeeds, `build.sh` performs only the application compilation.

The resulting executable is the ScreenRec program.

## Dynamic build

A dynamic build can be requested with:

```sh
./configure.sh --dynamic
./build.sh
```

Dynamic mode uses the dependencies already installed on the system.

It does not download or build private copies of libraries.

This is useful when you prefer the executable to use the system's shared libraries.

## Cleaning the build state

To remove generated build state while keeping the dependency download cache:

```sh
./configure.sh --clean-run
```

This allows dependencies that were already downloaded to be reused instead of downloaded again.

## Basic usage

Running ScreenRec without arguments records the entire X11 screen:

```sh
./screenrec
```

The output filename is generated automatically.

Example:

```
recording-2026-09-25-18-30-42-12345.mp4
```

## Capture modes

**Full screen**

```sh
./screenrec --screen
```

**Window selection**

```sh
./screenrec --window
```

Click the window you want to record.

Press Escape to cancel the selection.

**Area selection**

```sh
./screenrec --area
```

Drag the area you want to record.

Press Escape to cancel the selection.

**Specific monitor**

First list the available monitors:

```sh
./screenrec --list-monitors
```

Then select one:

```sh
./screenrec --monitor 2
```

## Output directory

Recordings are written to the current directory by default.

A different directory can be selected with:

```sh
./screenrec --output-dir ~/Videos
```

The directory is created automatically when necessary.

## Video codecs

ScreenRec can automatically search for a usable video encoder.

When no codec is specified, it attempts available encoders and uses the first one that successfully initializes.

To see the video encoders compiled into the current binary:

```sh
./screenrec --list-codecs
```

A specific encoder can be selected with:

```sh
./screenrec --codec mpeg4
```

When `--codec` is explicitly specified, ScreenRec does not silently replace it with another encoder if initialization fails.

## Hardware encoding

When available, ScreenRec can use VA-API video encoding.

The program attempts to initialize available DRM devices and use a VA-API encoder.

For systems where VA-API is unavailable or the encoder cannot be initialized, automatic codec selection can continue with another encoder.

## Audio

ScreenRec records two independent audio sources:

**Desktop audio**

Captured from the default PulseAudio monitor source.

**Microphone**

Captured from the default PulseAudio input source.

Both are stored as independent audio tracks in the resulting MP4 file.

Audio is captured at:

```
48 kHz
Stereo
16-bit PCM input
```

The AAC encoder is used for the final audio tracks.

## Command reference

```
Usage: screenrec [options]

With no capture-mode flag, the entire X11 screen is recorded.

Capture modes:
--screen                 Record the entire X11 screen.
--window                 Click a window to record it.
--area                   Drag an area to record.
--monitor N              Record monitor N from RandR.

Output and codec:
--output-dir DIR         Write recordings into DIR.
--codec CODEC            Use a specific video encoder.
Missing CODEC prints this help.

Information:
--list-monitors          List active X11 monitors.
--list-codecs            List video encoders compiled into the binary.
--help                   Show this help.
```

## Examples

Record the entire desktop:

```sh
./screenrec
```

Record a selected window:

```sh
./screenrec --window
```

Record a selected area:

```sh
./screenrec --area
```

Record the second monitor:

```sh
./screenrec --monitor 2
```

Save recordings to `~/Videos`:

```sh
./screenrec --output-dir ~/Videos
```

Use a specific encoder:

```sh
./screenrec --codec mpeg4
```

Combine options:

```sh
./screenrec --monitor 1 --output-dir ~/Videos --codec mpeg4
```

## Design

ScreenRec is intentionally implemented as a single C program.

The recorder is divided internally into independent capture and encoding paths:

- X11/XShm video capture
- Video encoding
- Desktop audio capture
- Microphone capture
- MP4 muxing

Video and audio processing run independently so that screen capture and audio capture do not have to share the same timing loop.

The application uses XShm rather than ordinary X11 image transfer because high-frequency screen capture is a primary requirement.

## Current platform

ScreenRec currently targets:

- Linux
- X11
- PulseAudio

Wayland-native capture is not currently implemented.

## License

Add the project's license information here.

