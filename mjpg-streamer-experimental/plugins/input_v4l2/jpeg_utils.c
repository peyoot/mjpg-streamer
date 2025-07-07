#include "jpeg_utils.h"
#include <setjmp.h>
#include <time.h>

struct error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void error_exit(j_common_ptr cinfo) {
    struct error_mgr* err = (struct error_mgr*)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(err->setjmp_buffer, 1);
}

// 优化的 YUYV 转 JPEG 函数
int compress_yuyv_to_jpeg(unsigned char *dst, size_t dst_size, 
                         unsigned char *src, int width, int height, int quality) {
    struct jpeg_compress_struct cinfo;
    struct error_mgr jerr;
    JSAMPROW row_pointer[1];
    int row_stride = width * 2; // YUYV是2字节/像素
    
    // 性能测量
    clock_t start = clock();
    
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = error_exit;
    
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_compress(&cinfo);
        return -1;
    }

    jpeg_create_compress(&cinfo);
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_mem_dest(&cinfo, &dst, &dst_size);
    
    // 设置采样因子 (4:2:2)
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 2;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;
    
    jpeg_start_compress(&cinfo, TRUE);
    
    // 直接处理，避免内存复制
    unsigned char *row = malloc(width * 3);
    if (!row) {
        jpeg_destroy_compress(&cinfo);
        return -1;
    }
    
    while (cinfo.next_scanline < cinfo.image_height) {
        unsigned char *yuyv_line = src + cinfo.next_scanline * row_stride;
        unsigned char *row_ptr = row;
        
        for (int x = 0; x < width; x += 2) {
            // 一次处理两个像素
            int y0 = yuyv_line[0];
            int u0 = yuyv_line[1];
            int y1 = yuyv_line[2];
            int v0 = yuyv_line[3];
            yuyv_line += 4;
            
            // 像素1: Y0, U0, V0
            *row_ptr++ = y0;
            *row_ptr++ = u0;
            *row_ptr++ = v0;
            
            // 像素2: Y1, U0, V0
            *row_ptr++ = y1;
            *row_ptr++ = u0;
            *row_ptr++ = v0;
        }
        
        row_pointer[0] = row;
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }
    
    free(row);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    
    // 性能报告
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    printf("YUYV->JPEG: %dx%d in %.2fms\n", width, height, elapsed * 1000);
    
    return dst_size;
}

// 优化的 RGBP (RGB565) 转 JPEG 函数
int compress_rgbp_to_jpeg(unsigned char *dst, size_t dst_size, 
                         unsigned char *src, int width, int height, int quality) {
    struct jpeg_compress_struct cinfo;
    struct error_mgr jerr;
    JSAMPROW row_pointer[1];
    
    // 性能测量
    clock_t start = clock();
    
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = error_exit;
    
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_compress(&cinfo);
        return -1;
    }

    jpeg_create_compress(&cinfo);
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_mem_dest(&cinfo, &dst, &dst_size);
    
    jpeg_start_compress(&cinfo, TRUE);
    
    // 直接处理，避免内存复制
    unsigned char *row = malloc(width * 3);
    if (!row) {
        jpeg_destroy_compress(&cinfo);
        return -1;
    }
    
    while (cinfo.next_scanline < cinfo.image_height) {
        unsigned char *rgbp_line = src + cinfo.next_scanline * width * 2;
        unsigned char *row_ptr = row;
        
        for (int x = 0; x < width; x++) {
            // 提取RGB565值
            unsigned short pixel = (rgbp_line[1] << 8) | rgbp_line[0];
            rgbp_line += 2;
            
            // 转换为RGB888
            unsigned char r = (pixel >> 11) & 0x1F;
            unsigned char g = (pixel >> 5) & 0x3F;
            unsigned char b = pixel & 0x1F;
            
            // 扩展到8位
            *row_ptr++ = (r << 3) | (r >> 2);
            *row_ptr++ = (g << 2) | (g >> 4);
            *row_ptr++ = (b << 3) | (b >> 2);
        }
        
        row_pointer[0] = row;
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }
    
    free(row);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    
    // 性能报告
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    printf("RGBP->JPEG: %dx%d in %.2fms\n", width, height, elapsed * 1000);
    
    return dst_size;
}