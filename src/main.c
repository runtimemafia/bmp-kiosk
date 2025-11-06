#include "mjpeg_grabber.h"
#include <stdio.h>
#include <jpeglib.h>
#include <zbar.h>

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
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    *w = cinfo.output_width;
    *h = cinfo.output_height;
    row_stride = cinfo.output_width * cinfo.output_components;

    // allocate output as grayscale buffer
    unsigned char *gray = malloc(cinfo.output_width * cinfo.output_height);
    if (!gray) { jpeg_destroy_decompress(&cinfo); return -1; }

    // buffer for one row
    buffer = (*cinfo.mem->alloc_sarray)
                 ((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);

    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, buffer, 1);
        // convert row to grayscale if needed
        if (cinfo.output_components == 3) {
            for (unsigned int x = 0; x < cinfo.output_width; x++) {
                unsigned char r = buffer[0][3*x + 0];
                unsigned char g = buffer[0][3*x + 1];
                unsigned char b = buffer[0][3*x + 2];
                // simple luminance
                gray[cinfo.output_scanline * cinfo.output_width + x] =
                    (unsigned char)((0.299*r + 0.587*g + 0.114*b));
            }
        } else if (cinfo.output_components == 1) {
            memcpy(&gray[cinfo.output_scanline * cinfo.output_width],
                   buffer[0], cinfo.output_width);
        } else {
            // handle other component counts if necessary
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    *out_gray = gray;
    return 0;
}

int main(void) {
    // start stream first
    if (stream_start(640, 480, 5) != 0) {
        fprintf(stderr, "Failed to start stream\n");
        return 1;
    }

    // wait or loop; here we take one snapshot
    sleep(1);

    uint8_t *jpeg = NULL;
    size_t jpeg_sz = 0;
    if (get_latest_jpeg_copy(&jpeg, &jpeg_sz) != 0) {
        fprintf(stderr, "No frame yet\n");
        stream_stop();
        return 1;
    }

    unsigned char *gray = NULL;
    int w=0,h=0;
    if (decode_jpeg_to_gray(jpeg, jpeg_sz, &gray, &w, &h) != 0) {
        fprintf(stderr, "JPEG decode failed\n");
        free(jpeg);
        stream_stop();
        return 1;
    }

    // zbar scanning
    zbar_image_scanner_t *scanner = zbar_image_scanner_create();
    zbar_image_scanner_set_config(scanner, 0, ZBAR_CFG_ENABLE, 1);

    zbar_image_t *image = zbar_image_create();
    zbar_image_set_format(image, *(int*)"Y800"); // gray format
    zbar_image_set_size(image, w, h);
    zbar_image_set_data(image, gray, w*h, zbar_image_free_data); // zbar will free gray

    int n = zbar_scan_image(scanner, image);
    if (n > 0) {
        const zbar_symbol_t *sym = zbar_image_first_symbol(image);
        for (; sym; sym = zbar_symbol_next(sym)) {
            printf("QR data: %s\n", zbar_symbol_get_data(sym));
        }
    } else {
        printf("No QR found\n");
        free(gray); // if not passed to zbar; above we passed to zbar so careful
    }

    zbar_image_destroy(image);
    zbar_image_scanner_destroy(scanner);
    free(jpeg);
    stream_stop();
    return 0;
}
