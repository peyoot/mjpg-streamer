#ifndef JPEG_UTILS_H
#define JPEG_UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>

int compress_yuyv_to_jpeg(unsigned char *dst, size_t dst_size, 
                         unsigned char *src, int width, int height, int quality);

int compress_rgbp_to_jpeg(unsigned char *dst, size_t dst_size, 
                         unsigned char *src, int width, int height, int quality);

#endif