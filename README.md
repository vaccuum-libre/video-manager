# video-manager

`video-recorder` captures a Linux V4L2 camera, decodes the camera's native stream, converts frames when necessary, encodes H.264, and writes an MP4 file. A recording starts at the first camera timestamp and stops before the first frame timestamp at or beyond 180 seconds. The duration is therefore timestamp-driven rather than controlled by a sleep; its precision is one captured frame.

## Architecture and data flow

The streaming path is:

1. `libavdevice` opens the selected V4L2 device with the requested size, frame rate, and optional camera pixel format.
2. `libavformat` demuxes camera packets and `libavcodec` decodes them. This supports raw formats such as YUYV422 and compressed formats such as MJPEG through the decoder selected from the camera stream.
3. Frames already in encoder-compatible YUV420P are passed directly to the encoder. Other formats use one cached `libswscale` context and one reusable destination frame.
4. The H.264 encoder uses the modern send/receive API. Encoded packets are timestamp-rescaled and immediately muxed into MP4.
5. On completion, SIGINT, or SIGTERM, the encoder is flushed and the MP4 trailer is written before resources are released.

No recording-sized buffer is used. Memory is bounded by the camera/decoder/encoder internal queues, two reusable packets, a decoded frame, and at most one conversion frame. At 1920x1080 YUV420P, the explicit conversion frame is about 3 MiB. Codec queues add several more frames depending on the chosen encoder. Disabling B-frames and using x264's `zerolatency` tune bounds encoder delay and memory, while the `veryfast` preset trades some compression efficiency for CPU headroom. A slower preset may produce better quality at the same bitrate but is not exposed because preset names are encoder-specific.

Camera timestamps are rebased to zero, rescaled to the encoder time base, and forced strictly monotonic when coarse or duplicate device timestamps occur. If a driver supplies no timestamps, the program reports the limitation and derives timestamps from the configured frame rate. MP4 packet timestamps are rescaled again to the stream time base with `av_packet_rescale_ts()`.

## Requirements

- Linux with a V4L2 camera
- A C11 compiler
- `pkg-config`
- FFmpeg development libraries: `libavdevice`, `libavformat`, `libavcodec`, `libavutil`, and `libswscale`
- An FFmpeg build with an H.264 encoder, normally `libx264`

On Debian or Ubuntu:

```sh
sudo apt install build-essential pkg-config \
  libavdevice-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

The code targets FFmpeg 5.1 or newer and uses non-deprecated send/receive codec APIs. V4L2 option names and available encoders depend on how FFmpeg was built.

## Build

```sh
make
```

Equivalent direct command:

```sh
cc -std=c11 -O2 -Wall -Wextra -Wpedantic recorder.c \
  $(pkg-config --cflags --libs libavdevice libavformat libavcodec libavutil libswscale) \
  -o video-recorder
```

## Usage

```text
Usage: ./video-recorder [OPTIONS]
Record 180 seconds of camera video to an H.264 MP4 file.

Options:
  -o, --output FILE          Output file (default: recording.mp4)
  -d, --device DEVICE        V4L2 camera device (default: /dev/video0)
  -s, --size WIDTHxHEIGHT    Capture size (default: 1280x720)
  -r, --framerate FPS        Capture frame rate (default: 30)
  -b, --bitrate RATE         H.264 bitrate; K/M suffix allowed (default: 4M)
  -p, --pixel-format FORMAT  V4L2 format, e.g. yuyv422 or mjpeg
  -e, --encoder NAME         FFmpeg H.264 encoder (default: libx264)
  -h, --help                 Show help
```

Examples:

```sh
./video-recorder --device /dev/video0 --output recording.mp4
./video-recorder -d /dev/video0 -s 1920x1080 -r 30 -p mjpeg -b 8M -o camera.mp4
./video-recorder -d /dev/video2 -s 1280x720 -r 25 -p yuyv422 -o camera.mp4
```

List the modes exposed by a camera before choosing size, frame rate, and format:

```sh
v4l2-ctl --device /dev/video0 --list-formats-ext
```

The `--pixel-format` value is a V4L2/FFmpeg input format name, not the encoder format. Common values are `yuyv422` and `mjpeg`; supported values are device- and FFmpeg-build-specific. The encoder output is YUV420P for broad MP4 player compatibility. A named encoder can be selected explicitly, but it must accept software YUV420P frames; hardware encoders requiring device frames are intentionally not selected automatically.

Ctrl+C and SIGTERM request a graceful stop. The partial recording is finalized and playable; interruption returns status 130. Other failures return nonzero. MP4 requires a trailer, so a process kill, power failure, or complete storage failure can still leave an unplayable file.

Verify the result and inspect timestamps:

```sh
ffprobe -v error -show_entries format=duration:stream=codec_name,pix_fmt,width,height,avg_frame_rate \
  -of default=noprint_wrappers=1 recording.mp4
```

## Testing checklist

- Record a supported mode for three minutes. Confirm `ffprobe` reports H.264, YUV420P, the requested dimensions/rate, and a duration within one frame of 180 seconds.
- Press Ctrl+C after several seconds. Confirm exit status 130 and that `ffprobe` can read the finalized file.
- Select a missing device and disconnect a camera during capture. Confirm a nonzero exit and a readable diagnostic, with no crash.
- Request unsupported sizes, frame rates, and `--pixel-format` values. Confirm device negotiation fails or a negotiated mismatch is reported.
- Record onto a filesystem with insufficient free space. Confirm packet/trailer write failures are reported and the process exits nonzero.
- Pass an unavailable value to `--encoder`. Confirm initialization fails before capture starts.
- Try malformed sizes, rates, and bitrates. Confirm exit status 2 and a useful error.
- Repeat start, interrupt, and completion cycles to catch device/resource cleanup regressions.
- Build and run with AddressSanitizer, then repeat the short interruption and failure cases:

  ```sh
  make clean
  make CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined' \
       LDLIBS="$(pkg-config --libs libavdevice libavformat libavcodec libavutil libswscale) -fsanitize=address,undefined"
  ASAN_OPTIONS=detect_leaks=1 ./video-recorder -d /dev/video0 -o asan.mp4
  ```

  Alternatively run `valgrind --leak-check=full ./video-recorder ...` with a software encoder. FFmpeg and driver libraries can retain process-global allocations; distinguish those from definitely lost allocations owned by the recorder.

## Limitations

- Linux V4L2 is the only input backend.
- Recording length is fixed at 180 seconds and quantized to camera frame timestamps.
- Audio is not captured.
- Hardware encoders that require hardware frame contexts are not supported by this software-frame pipeline.
- Abrupt termination cannot finalize a conventional MP4 container.
