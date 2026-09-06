# tutor_vision

A one-key HDMI screenshot tutor for Linux. Developed on a Dell E6400 (Core 2 Duo, 64-bit Linux), but works on any Linux machine with a V4L2 MJPEG capture device.

Run the agent, point an HDMI source at the USB capture dongle, press **Space**, and
it grabs a fresh MJPEG frame, sends it (base64 as a data URL) to a vision model via
OpenRouter, and prints the tutor's answer. Press **q** or **Ctrl-C** to quit.

## What you need

- Any USB HDMI capture dongle that exposes a V4L2 MJPEG device (e.g. `MS2109`).
- Build only: a POSIX box with `gcc`, `make`-style tooling, and static
  `libcurl`/`libssl`/`libcrypto`/`libz` archives.
- An [OpenRouter](https://openrouter.ai) API key and a vision-capable model.

## Setup

Copy the key template and put in your real OpenRouter key (this file is gitignored):

```
cp secret.h.example secret.h
# edit secret.h
```

## Build

```
./build.sh
```

Produces a single static `agent` binary whose only dynamic dependency is `libc`
(carries its own TLS/HTTPS via static curl + OpenSSL). Copy it to your target machine and run:

```
./agent
```

On startup it opens `/dev/video0`, sets 1280x720 MJPEG, and streams so the HDMI
source stays connected.

## Configuration

- Model: set `TUTOR_VISION_MODEL` env var to override the default
  (`google/gemini-2.5-flash`).
- Video device: edit `VIDEO_DEV` in `agent.c` if your capture device lands on a
  different `/dev/videoN`.