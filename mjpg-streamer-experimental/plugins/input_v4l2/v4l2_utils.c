// jpeg_utils.c
#include "jpeg_utils.h"
#include <string.h>

int compress_yuyv_to_jpeg(unsigned char *dst, size_t dst_size, 
                         unsigned char *src, int width, int height, int quality) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];
    int row_stride;
    unsigned char *line_buffer, *yuyv;
    int z;
    unsigned long jpeg_size = 0;

    // 创建错误处理器
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    
    // 设置内存目标
    jpeg_mem_dest(&cinfo, &dst, &jpeg_size);
    dst_size = jpeg_size; // 更新实际使用的缓冲区大小

    // 设置图像参数
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3; // RGB
    cinfo.in_color_space = JCS_RGB;

    // 设置默认压缩参数
    jpeg_set_defaults(&cinfo);
    
    // 设置质量
    jpeg_set_quality(&cinfo, quality, TRUE);
    
    // 开始压缩
    jpeg_start_compress(&cinfo, TRUE);

    // 分配行缓冲区
    line_buffer = malloc(width * 3);
    if (!line_buffer) {
        fprintf(stderr, "Failed to allocate line buffer\n");
        jpeg_destroy_compress(&cinfo);
        return -1;
    }
    
    // YUYV 格式：每个像素2字节（Y0, U, Y1, V）
    row_stride = width * 2;

    // 逐行处理
    while (cinfo.next_scanline < cinfo.image_height) {
        yuyv = src + cinfo.next_scanline * row_stride;
        
        for (z = 0; z < width; z++) {
            int Y, U, V, R, G, B;
            int index = z * 2;

            // 获取 YUV 值
            if (z % 2 == 0) {
                // 偶数像素：Y0 和 UV 分量
                Y = yuyv[index];
                U = yuyv[index + 1];
                V = yuyv[index + 3];
            } else {
                // 奇数像素：Y1 和 UV 分量（共享）
                Y = yuyv[index];
                U = yuyv[index - 1];
                V = yuyv[index + 1];
            }

            // 转换 YUV 到 RGB
            // 基本转换公式 (ITU-R BT.601)
            Y -= 16;
            U -= 128;
            V -= 128;
            
            R = (298 * Y + 409 * V + 128) >> 8;
            G = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            B = (298 * Y + 516 * U + 128) >> 8;

            // 限制值在 0-255 范围内
            if (R > 255) R = 255;
            if (G > 255) G = 255;
            if (B > 255) B = 255;
            if (R < 0) R = 0;
            if (G < 0) G = 0;
            if (B < 0) B = 0;

            // 存储 RGB 值
            line_buffer[z * 3] = (unsigned char)R;
            line_buffer[z * 3 + 1] = (unsigned char)G;
            line_buffer[z * 3 + 2] = (unsigned char)B;
        }
        
        // 写入一行
        row_pointer[0] = line_buffer;
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    // 完成压缩
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    
    // 释放行缓冲区
    free(line_buffer);
    
    return jpeg_size;
}