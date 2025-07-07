#include "jpeg_utils.h"
#include <setjmp.h>

struct error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void error_exit(j_common_ptr cinfo) {
    struct error_mgr* err = (struct error_mgr*)cinfo->err;
    longjmp(err->setjmp_buffer, 1);
}

int compress_yuyv_to_jpeg(unsigned char *dst, size_t dst_size, 
                         unsigned char *src, int width, int height, int quality) {
    struct jpeg_compress_struct cinfo;
    struct error_mgr jerr;
    JSAMPROW row_pointer[1];
    int row_stride = width * 2;
    
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
    
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 2;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;
    
    jpeg_start_compress(&cinfo, TRUE);
    
    unsigned char *row = malloc(width * 3);
    if (!row) {
        jpeg_destroy_compress(&cinfo);
        return -1;
    }
    
    while (cinfo.next_scanline < cinfo.image_height) {
        unsigned char *yuyv_line = src + cinfo.next_scanline * row_stride;
        unsigned char *row_ptr = row;
        
        for (int x = 0; x < width; x += 2) {
            *row_ptr++ = yuyv_line[0]; // Y0
            *row_ptr++ = yuyv_line[1]; // U0
            *row_ptr++ = yuyv_line[3]; // V0
            
            *row_ptr++ = yuyv_line[2]; // Y1
            *row_ptr++ = yuyv_line[1]; // U0
            *row_ptr++ = yuyv_line[3]; // V0
            
            yuyv_line += 4;
        }
        
        row_pointer[0] = row;
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }
    
    free(row);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    
    return dst_size;
}

int compress_rgbp_to_jpeg(unsigned char *dst, size_t dst_size, 
                         unsigned char *src, int width, int height, int quality) {
    struct jpeg_compress_struct cinfo;
    struct error_mgr jerr;
    JSAMPROW row_pointer[1];
    
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
    
    unsigned char *row = malloc(width * 3);
    if (!row) {
        jpeg_destroy_compress(&cinfo);
        return -1;
    }
    
    while (cinfo.next_scanline < cinfo.image_height) {
        unsigned char *rgbp_line = src + cinfo.next_scanline * width * 2;
        unsigned char *row_ptr = row;
        
        for (int x = 0; x < width; x++) {
            unsigned short pixel = (rgbp_line[1] << 8) | rgbp_line[0];
            rgbp_line += 2;
            
            unsigned char r = (pixel >> 11) & 0x1F;
            unsigned char g = (pixel >> 5) & 0x3F;
            unsigned char b = pixel & 0x1F;
            
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
    
    return dst_size;
}