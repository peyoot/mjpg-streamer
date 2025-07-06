// plugins/input_v4l2/jpeg_utils.c
#include "jpeg_utils.h"
#include "v4l2_utils.h"  // 添加这个头文件以获取 V4L2 格式定义
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>

int compress_yuyv_to_jpeg(unsigned char *dst, size_t dst_size, 
                         unsigned char *src, int width, int height, int quality) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    int row_stride;
    unsigned char *line_buffer, *yuyv;
    int z;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &dst, &dst_size);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    line_buffer = malloc(width * 3);
    row_stride = width * 2; // YUYV is 2 bytes per pixel

    while (cinfo.next_scanline < cinfo.image_height) {
        yuyv = src + cinfo.next_scanline * row_stride;
        for (z = 0; z < width; z++) {
            int Y, U, V, R, G, B;
            int index = z * 2;

            if (z & 1) {
                Y = yuyv[index];
                U = yuyv[index - 1];
                V = yuyv[index + 1];
            } else {
                Y = yuyv[index];
                U = yuyv[index + 1];
                V = yuyv[index + 3];
            }

            // Convert YUV to RGB
            Y -= 16;
            U -= 128;
            V -= 128;
            R = (298 * Y + 409 * V + 128) >> 8;
            G = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            B = (298 * Y + 516 * U + 128) >> 8;

            // Clamp values
            if (R > 255) R = 255;
            if (G > 255) G = 255;
            if (B > 255) B = 255;
            if (R < 0) R = 0;
            if (G < 0) G = 0;
            if (B < 0) B = 0;

            line_buffer[z * 3] = R;
            line_buffer[z * 3 + 1] = G;
            line_buffer[z * 3 + 2] = B;
        }
        row_pointer[0] = line_buffer;
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(line_buffer);
    return dst_size;
}
