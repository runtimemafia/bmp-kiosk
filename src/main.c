#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include "mjpeg_grabber.h"
#include <stdio.h>
#include <jpeglib.h>
#include <zbar.h>
#include <unistd.h>   // for usleep()
#include <string.h>   // for memcpy()
#include <signal.h>
#include <stdlib.h>

volatile sig_atomic_t stop_requested = 0;

static void handle_sigint(int signum) {
    (void)signum;
    stop_requested = 1;
}

int decode_jpeg_to_gray(const uint8_t *jpeg, size_t jpeg_sz,
                        unsigned char **out_gray, int *w, int *h)
{
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

    unsigned char *gray = malloc((size_t)cinfo.output_width * cinfo.output_height);
    if (!gray) { jpeg_finish_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); return -1; }

    buffer = (*cinfo.mem->alloc_sarray)
                 ((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);

    int out_row = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, buffer, 1);
        if (cinfo.output_components == 3) {
            for (unsigned int x = 0; x < cinfo.output_width; x++) {
                unsigned char r = buffer[0][3*x + 0];
                unsigned char g = buffer[0][3*x + 1];
                unsigned char b = buffer[0][3*x + 2];
                gray[out_row * cinfo.output_width + x] =
                    (unsigned char)((0.299*r + 0.587*g + 0.114*b));
            }
        } else if (cinfo.output_components == 1) {
            memcpy(&gray[out_row * cinfo.output_width],
                   buffer[0], cinfo.output_width);
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
        uint8_t *jpeg = NULL;
        size_t jpeg_sz = 0;

        if (get_latest_jpeg_copy(&jpeg, &jpeg_sz) != 0) {
            // no frame yet -> wait a bit and retry
            usleep(100 * 1000); // 100 ms
            continue;
        }

        unsigned char *gray = NULL;
        int w = 0, h = 0;
        if (decode_jpeg_to_gray(jpeg, jpeg_sz, &gray, &w, &h) != 0) {
            fprintf(stderr, "JPEG decode failed\n");
            free(jpeg);
            usleep(50 * 1000);
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
        zbar_image_set_format(image, *(int*)"Y800");
        zbar_image_set_size(image, w, h);
        zbar_image_set_data(image, gray, w * h, zbar_image_free_data);

        int n = zbar_scan_image(scanner, image);
        if (n > 0) {
            const zbar_symbol_t *sym = zbar_image_first_symbol(image);
            for (; sym; sym = zbar_symbol_next(sym)) {
                const char *data = zbar_symbol_get_data(sym);
                if (data) {
                    printf("QR data: %s\n", data);
                }
            }
        } else {
            // uncomment next line to get "no QR" logs:
            // printf("No QR found\n");
        }

        zbar_image_destroy(image);
        free(jpeg); // jpeg was malloc'd by get_latest_jpeg_copy

        // small sleep to avoid busy loop; adjust to desired responsiveness
        usleep(50 * 1000); // 50 ms
    }

    printf("Stopping...\n");
    zbar_image_scanner_destroy(scanner);
    stream_stop();
    return 0;
}
