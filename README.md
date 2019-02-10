[![Build Status](https://travis-ci.org/brndnmtthws/doge-streamer.svg?branch=master)](https://travis-ci.org/brndnmtthws/doge-streamer)

# Doge Streamer 🦊

`doge-streamer` is a tool for Doge live streaming, which you can find at:

- [YouTube](https://www.youtube.com/channel/UCg4HoZlSlRGvyPtcInkIQvQ), or
- [Twitch](https://www.twitch.tv/live_doge)

This tool is based on OpenCV and FFmpeg.

OpenCV is used for capturing images from cameras, processing video, performing Doge detection, image processing, and overlaying text/images.

FFmpeg is used to encode the stream with x264, AAC, and flv. The stream is sent out to [castr.io](castr.io) using RTMP.

Features:

- background subtraction based motion detection (using `BackgroundSubtractorMOG2`)
- automatically switches to active camera
- displays an image when cameras are inactive
- stream can be controlled remotely using [doge-stream-helper](https://github.com/brndnmtthws/doge-stream-helper)
- sound from one audio source

TODO:

- better audio input handling
- add delightful background music
- train a TensorFlow model to detect Doge? perhaps use a combination of motion detection and an RNN model?

## Quickstart

### Install dependencies

On macOS:

```ShellSession
$ brew install cmake ffmpeg opencv
...
```

On Debian based distros:

```ShellSession:
$ apt-get install -yqq \
    cmake             \
    build-essential   \
    libavcodec-dev    \
    libavdevice-dev   \
    libopencv-dev     \
    libavformat-dev   \
    libavutil-dev     \
    libswresample-dev \
    libswscale-dev    \
    libopencv-dev
...
```

### Compile

To compile source code just run:

```ShellSession
$ mkdir build
$ cd build
$ cmake .. -DCMAKE_BUILD_TYPE=Debug
$ make -j4
```

### Run RTMP Server

```ShellSession
$ docker run -p 1935:1935 tiangolo/nginx-rtmp
...
```

### Run `doge-streamer`

```ShellSession
$ ./doge-streamer
```

### Watch & Enjoy

Use VLC or `ffplay` to connect to live video stream:

```ShellSession
$ ffplay -sync ext rtmp://localhost/live/stream
...
```
