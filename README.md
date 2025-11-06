### compile

```bash
gcc -std=c11 -o qr_cam main.c mjpeg_grabber.c -lpthread -ljpeg -lzbar
```


### src/mjpeg_grabber.c

```bash
# compile
gcc -O3 -march=armv8-a -mtune=cortex-a53 -pthread -Wall -Wextra -o mjpeg_grabber mjpeg_grabber.c
```

```bash
# run
./mjpeg_grabber 640 480 5
# in this the parameters are WIDTH HEIGHT FPS
```

This file starts a rpicam-vid stream which runs continously.

We get these API

- stream_start()
- grab_jpeg()
- stop_stream()

So we can get the latest JPEG from the stream and a continous running stream keeps the exposure and other settings in balance.
