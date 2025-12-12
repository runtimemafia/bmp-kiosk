#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <jpeglib.h>
#include <zbar.h>
#include "mjpeg_grabber.h"
#include "server_communication.h"

static atomic_bool sending = ATOMIC_VAR_INIT(false);
volatile sig_atomic_t stop_requested = 0;

#ifndef USE_RPI_CAM
// *** TOGGLE: Set to true to save frames with detected QR codes ***
static const bool DEBUG_SAVE_FRAMES = true;

// Debug helper: save frame to images/ for development
static void debug_save_frame(const uint8_t *jpeg, size_t jpeg_sz) {
  static int frame_count = 0;
  char path[64];
  snprintf(path, sizeof path, "images/qr_debug_%04d.jpg", frame_count++);
  FILE *f = fopen(path, "wb");
  if (f) {
    fwrite(jpeg, 1, jpeg_sz, f);
    fclose(f);
  }
}
#endif

static void handle_sigint(int signum) {
  (void)signum;
  stop_requested = 1;
}

static void msleep(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

int decode_jpeg_to_gray(const uint8_t *jpeg, size_t jpeg_sz,
                        unsigned char **out_gray, int *w, int *h) {
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  JSAMPARRAY buffer;
  int row_stride;

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, jpeg, (unsigned long)jpeg_sz);
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }
  if (!jpeg_start_decompress(&cinfo)) {
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  *w = cinfo.output_width;
  *h = cinfo.output_height;
  row_stride = cinfo.output_width * cinfo.output_components;

  unsigned char *gray =
      malloc((size_t)cinfo.output_width * cinfo.output_height);
  if (!gray) {
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return -1;
  }

  buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE,
                                      row_stride, 1);

  int out_row = 0;
  while (cinfo.output_scanline < cinfo.output_height) {
    jpeg_read_scanlines(&cinfo, buffer, 1);
    if (cinfo.output_components == 3) {
      for (unsigned int x = 0; x < cinfo.output_width; x++) {
        unsigned char r = buffer[0][3 * x + 0];
        unsigned char g = buffer[0][3 * x + 1];
        unsigned char b = buffer[0][3 * x + 2];
        gray[out_row * cinfo.output_width + x] =
            (unsigned char)((0.299 * r + 0.587 * g + 0.114 * b));
      }
    } else if (cinfo.output_components == 1) {
      memcpy(&gray[out_row * cinfo.output_width], buffer[0],
             cinfo.output_width);
    } else {
      // unsupported component count
      free(gray);
      jpeg_finish_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      return -1;
    }
    out_row++;
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);

  *out_gray = gray;
  return 0;
}

int main(void) {
  // handle Ctrl+C
  struct sigaction sa = {0};
  sa.sa_handler = handle_sigint;
  sigaction(SIGINT, &sa, NULL);

  // example: read URL from env var or hardcode
  const char *url = getenv("QR_SERVER_URL");
  if (!url)
    url = "http://localhost:3000/api/kiosk/v1/qrscan";

  if (server_comm_init(url) != 0) {
    fprintf(stderr, "server_comm_init failed\n");
    // continue anyway or exit
  }

  if (stream_start(640, 480, 5) != 0) {
    fprintf(stderr, "Failed to start stream\n");
    return 1;
  }

  printf("Streaming started. Press Ctrl+C to stop.\n");

  // create a single scanner and reuse it
  zbar_image_scanner_t *scanner = zbar_image_scanner_create();
  if (!scanner) {
    fprintf(stderr, "Failed to create zbar scanner\n");
    stream_stop();
    return 1;
  }
  zbar_image_scanner_set_config(scanner, 0, ZBAR_CFG_ENABLE, 1);

  while (!stop_requested) {
    // If a send is in progress, wait here (do NOT call get_latest_jpeg_copy)
    if (atomic_load(&sending)) {
      // sleep short while send completes (adjust ms as needed)
      msleep(100);
      continue;
    }

    uint8_t *jpeg = NULL;
    size_t jpeg_sz = 0;

    // get latest jpeg (won't happen while sending==true because of check above)
    if (get_latest_jpeg_copy(&jpeg, &jpeg_sz) != 0) {
      // no frame yet -> wait a bit and retry
      msleep(100); // 100 ms
      continue;
    }

    unsigned char *gray = NULL;
    int w = 0, h = 0;
    if (decode_jpeg_to_gray(jpeg, jpeg_sz, &gray, &w, &h) != 0) {
      fprintf(stderr, "JPEG decode failed\n");
      free(jpeg);
      msleep(50);
      continue;
    }

    // prepare zbar image (zbar will free gray via zbar_image_free_data)
    zbar_image_t *image = zbar_image_create();
    if (!image) {
      fprintf(stderr, "Failed to create zbar image\n");
      free(gray);
      free(jpeg);
      break;
    }
    zbar_image_set_format(image, *(int *)"Y800");
    zbar_image_set_size(image, w, h);
    zbar_image_set_data(image, gray, w * h, zbar_image_free_data);

    int n = zbar_scan_image(scanner, image);
    if (n > 0) {
      const zbar_symbol_t *sym = zbar_image_first_symbol(image);
      for (; sym; sym = zbar_symbol_next(sym)) {
        const char *data = zbar_symbol_get_data(sym);
        if (data) {
          printf("QR data: %s\n", data);

#ifndef USE_RPI_CAM
          // Debug: save frame with QR code (toggle DEBUG_SAVE_FRAMES)
          if (DEBUG_SAVE_FRAMES)
            debug_save_frame(jpeg, jpeg_sz);
#endif

          // Try to become the single sender.
          // If sending was false, this will atomically set it to true and
          // succeed.
          bool expected = false;
          if (atomic_compare_exchange_strong(&sending, &expected, true)) {
            // We now "own" the send slot. Do the blocking send.
            if (send_qr_to_server(data) == 0) {
              printf(" -> sent to server\n");
            } else {
              fprintf(stderr, " -> failed to send to server\n");
            }
            // release send slot so scanning resumes
            atomic_store(&sending, false);
          } else {
            // another send already in progress; skip sending this one
            fprintf(stderr, "Send in progress, skipping this QR\n");
          }
        }
      }
    }

    zbar_image_destroy(image);
    free(jpeg); // jpeg was malloc'd by get_latest_jpeg_copy

    // small sleep to avoid busy loop; adjust to desired responsiveness
    msleep(50);
  }

  printf("Stopping...\n");
  server_comm_cleanup();
  zbar_image_scanner_destroy(scanner);
  stream_stop();
  return 0;
}
