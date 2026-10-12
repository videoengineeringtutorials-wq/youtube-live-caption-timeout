# YouTube Live Caption Encoder with Timeout

Turn a video source and UDP caption text into a YouTube Live stream with
two-line closed captions. `cc_injector_youtube_timeout` converts video to
**1920×1080, 30 fps progressive** and clears captions after **10 seconds
without new caption data**.

This repository contains one application and the source files needed to build
it. It does not generate speech-to-text: use your existing caption sender or
send text manually over UDP. Adapted from
[Live-Caption-Encoder](https://github.com/videoengineeringtutorials-wq/Live-Caption-Encoder).

## 1. Install the build tools

Developed and tested on Ubuntu 24.04 with FFmpeg 6.1 libraries. The program
uses C++17 and POSIX sockets. The FFmpeg libraries must include the `libx264`
encoder, AAC encoder, and the codecs/protocols needed for your input.

```bash
sudo apt update
sudo apt install -y \
  build-essential git pkg-config ffmpeg python3 netcat-openbsd \
  libavformat-dev libavcodec-dev libavutil-dev libavfilter-dev
```

`ffmpeg`, `python3`, and netcat are used for examples and tests. Compilation
requires the C++ tools, pkg-config, and all four FFmpeg development packages.

## 2. Download and compile

```bash
git clone https://github.com/videoengineeringtutorials-wq/youtube-live-caption-timeout.git
cd youtube-live-caption-timeout
make
```

This creates the executable `./cc_injector_youtube_timeout`. To inspect every
supported option:

```bash
./cc_injector_youtube_timeout --help
```

To compile directly without Make:

```bash
g++ -O2 -std=c++17 -Wall -Wextra -pthread \
  cc_injector_youtube_timeout.cpp \
  $(pkg-config --cflags --libs libavformat libavcodec libavutil libavfilter) \
  -o cc_injector_youtube_timeout
```

## 3. Set up YouTube

Create a stream in YouTube Studio and copy its stream key. Enable closed
captions and select **Embedded 608/708**. Use the RTMPS server URL shown by
Studio if it differs from the application's default:
`rtmps://a.rtmps.youtube.com/live2`.

See YouTube's [embedded caption setup](https://support.google.com/youtube/answer/3068031?hl=en)
and [live encoder settings](https://support.google.com/youtube/answer/2853702?hl=en).
Verify stream health and captions in an unlisted/private stream before an event.

## 4. Start streaming

Replace the input URL and stream key below with your own:

```bash
./cc_injector_youtube_timeout \
  -i 'https://example.com/live/index.m3u8' \
  --stream-key 'YOUR_YOUTUBE_STREAM_KEY' \
  --video-bitrate=6800000 \
  --cc-mode=popon \
  --cc-port=54001
```

The example uses **6,800 Kbps video**, matching the stream-specific suggestion
reported during deployment. Audio and container overhead add to the total
bitrate YouTube reports. The application's default, when this option is
omitted, is **14,000 Kbps video**. Match the bitrate to your stream's settings
and available upload capacity; the value is in **bits per second**.

Use `--cc-mode=rollup` instead for two-line roll-up. To override the server,
add `--youtube-server='rtmps://YOUR_SERVER/live2'`.

Press **Ctrl+C** to stop streaming and flush the remaining encoded media.

## 5. Send captions

Point your caption sender at the encoder computer's IP address, UDP port
**54001**. If the sender runs on another computer, allow that UDP port through
the encoder's firewall. The caption listener binds to all IPv4 interfaces.

From a second terminal on the encoder computer:

```bash
# One new line: the previous line remains above it.
printf 'Hello from the caption encoder\n' | nc -u -w0 127.0.0.1 54001

# Two lines in one datagram: replace the two-row caption.
printf 'This is the top line\nThis is the bottom line\n' | nc -u -w0 127.0.0.1 54001

# Clear captions immediately.
printf '\n' | nc -u -w0 127.0.0.1 54001
```

Each datagram is wrapped into at most **two lines of 32 printable ASCII
characters**. Explicit newlines are honored. Text beyond those two lines is
not displayed; send longer speech in successive messages. Repeated text
does not roll or repaint, but it refreshes the inactivity timer.

| Mode | Behavior |
| --- | --- |
| `--cc-mode=popon` | Load both rows in hidden memory, then show the completed caption. |
| `--cc-mode=rollup` | Move the previous line up and write the newest line below it. This is the default. |

Captions are anchored to rows 14 and 15 at the bottom of the picture. The
encoder clears destination rows to prevent leftover characters when a new
caption is shorter. Player settings may affect how captions appear.

### How the ten-second timeout works

Ten seconds without UDP caption data triggers commands to erase both the
displayed and hidden caption buffers. Stored caption history is also reset,
so the next caption starts fresh. The startup `CC ONLINE` caption expires
the same way. The timeout is fixed at ten seconds in this version.

Timing uses a monotonic wall clock. Clearing finishes through continuing video
frames after any pending caption update completes. If input video stalls,
clearing commands cannot reach the player until video resumes. Network and
player buffering also add playback delay. Repeated datagrams count as fresh
data, even if their text is unchanged.

## Other inputs and local previews

The installed FFmpeg demuxers and decoders determine input support. Files,
HTTP/HLS, UDP, RTMP, and RTSP URLs are accepted. Interlaced video is
deinterlaced; other sizes and frame rates are scaled/converted to 1080p30.
Display aspect ratio is preserved with black bars where needed. Higher
bit-depth SDR input is converted to 8-bit; HDR input needs external tone
mapping to SDR first.

```bash
# UDP MPEG-TS input; add your stream key and desired bitrate.
./cc_injector_youtube_timeout \
  -i 'udp://127.0.0.1:5000?fifo_size=1000000&overrun_nonfatal=1' \
  --stream-key 'YOUR_KEY' --video-bitrate=6800000 --cc-mode=rollup

# RTSP over TCP.
./cc_injector_youtube_timeout \
  -i 'rtsp://camera/live' --input-option=rtsp_transport=tcp \
  --stream-key 'YOUR_KEY' --video-bitrate=6800000

# Accept an RTMP publisher, then send the captioned output to YouTube.
./cc_injector_youtube_timeout \
  -i 'rtmp://0.0.0.0:1935/live/input' --input-option=listen=1 \
  --stream-key 'YOUR_KEY' --video-bitrate=6800000

# Local preview at live speed, without a YouTube connection.
./cc_injector_youtube_timeout -i input.mp4 -o preview.flv --pace=on
```

Open `preview.flv` in a player that supports embedded CEA-608 captions and
enable its caption track. `--pace=off` is faster for offline encoding, but a
short file may finish before the ten-second wall-clock timeout occurs.
RTMP(S) output is paced automatically by muxed media timestamps, including
file sources and HLS segment bursts.

## Output and useful options

| Setting | Output |
| --- | --- |
| Video | H.264 High Level 4.1, 1920×1080, exactly 30 fps progressive |
| Pixel format | 8-bit YUV 4:2:0, square pixels, Rec.709 SDR |
| Rate control | CBR; default 14 Mbps, adjustable with `--video-bitrate=BPS` |
| Keyframes | Closed two-second GOP, two B-frames, one reference frame, CABAC |
| Audio | AAC-LC, 44.1 kHz stereo, 128 Kbps; generated silence for video-only input |
| Captions | UDP text injected as CEA-608 CC1 in H.264 A/53 SEI after conversion |
| Transport | FLV over RTMP/RTMPS, or a local `.flv` file |

`--input-option=KEY=VALUE` can be repeated for input protocol/demuxer options;
`-f FORMAT` forces an input format. `--preset=superfast` or `ultrafast` can
reduce CPU demand. `--pace=auto|on|off` controls output pacing.

Incoming embedded captions are replaced by the UDP caption feed. Subtitle
files and CEA-708 generation are not implemented. Source clock resets and
network disconnects require restarting; there is no automatic reconnection.

### Stream keys without command-line arguments

Arguments can be visible in shell history and process listings. The
application redacts keys from its logs, and environment-based entry is also
supported:

```bash
read -rsp 'YouTube stream key: ' YOUTUBE_STREAM_KEY
echo
export YOUTUBE_STREAM_KEY
./cc_injector_youtube_timeout -i 'YOUR_INPUT_URL' --video-bitrate=6800000
```

`--stream-key KEY` and `--stream-key=KEY` override `YOUTUBE_STREAM_KEY`.
Server selection uses `--youtube-server=SERVER`, then `YOUTUBE_STREAM_URL`,
then the default RTMPS server. Use either `--stream-key` or `-o OUTPUT`.

## Troubleshooting

- **Missing headers or libraries:** install every development package listed
  above, especially `libavfilter-dev`. Keep FFmpeg headers and runtime libraries
  from compatible versions.
- **No captions:** enable Embedded 608/708 in YouTube Studio, select the
  player's caption track, and test a UDP message locally. Check the sender's
  destination IP, port, and firewall when sending from another machine.
- **YouTube bitrate suggestion:** adjust `--video-bitrate`; the reported
  aggregate bitrate includes audio and overhead.
- **Encoding falls behind:** try `--preset=superfast`, check CPU usage, and
  inspect the application's `[stats]` output. Source buffering also contributes
  to schedule lateness.
- **Captions remain while video is frozen:** the timeout needs output video
  frames to transmit its clearing commands.

## Run the checks

```bash
make check-unit        # Caption modes, row clearing, wrapping and FLV pacing
make check-integration # About one minute: real 10-second expiry and recovery
# Or run both:
make check
```

Tests use local generated video and temporary files; they never publish to
YouTube or require a stream key. They decode the embedded caption data to
verify both modes, shorter replacements, repeated-data refresh, clearing,
and recovery. Media checks verify the output profile, cadence, bitrate,
keyframes, AAC audio, and A/V timing. Actual YouTube behavior still needs a
live deployment test.

## License

MIT. See [LICENSE](LICENSE).
