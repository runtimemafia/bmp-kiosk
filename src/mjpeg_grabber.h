// mjpeg_grabber.h
#ifndef MJPEG_GRABBER_H
#define MJPEG_GRABBER_H

#include <stdint.h>
#include <stdlib.h>

int stream_start(int width, int height, int fps);
void stream_stop(void);

// old file saver
int grab_jpeg(const char *path);

// new: get a malloc'd copy of the latest JPEG bytes.
// Caller must free(*out_buf) when done.
// returns 0 on success, -1 if no frame available.
int get_latest_jpeg_copy(uint8_t **out_buf, size_t *out_sz);

#endif // MJPEG_GRABBER_H
