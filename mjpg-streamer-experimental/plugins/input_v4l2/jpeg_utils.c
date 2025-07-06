#include "jpeg_utils.h"
#include <setjmp.h>

struct error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void error_exit(j_common_ptr cinfo) {
    struct error_mgr* err = (struct error_mgr*)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(err->setjmp_buffer, 1);
}

int compress_yuyv_to_jpeg(unsigned char *dst, size_t dst_size, 
                         unsigned char *src, int width, int height, int quality) {
    struct jpeg_compress_struct cinfo;
    struct error_mgr jerr;
    JSAMPROW row_pointer[1];
    int row_stride;
    int y, x;
    unsigned char *yuyv, *y_row, *u_row, *v_row;

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
    
    jpeg_start_compress(&cinfo, TRUE);
    
    row_stride = width * 2; // YUYV是2字节/像素
    yuyv = src;
    
    // 分配临时行缓冲区
    unsigned char *row = malloc(width * 3);
    if (!row) {
        jpeg_destroy_compress(&cinfo);
        return -1;
    }
    
    while (cinfo.next_scanline < cinfo.image_height) {
        unsigned char *row_ptr = row;
        for (x = 0; x < width; x += 2) {
            // 处理两个像素 (YUYV格式: Y0 U0 Y1 V0)
            int y0 = yuyv[0];
            int u0 = yuyv[1];
            int y1 = yuyv[2];
            int v0 = yuyv[3];
            yuyv += 4;
            
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
    
    return dst_size;
}