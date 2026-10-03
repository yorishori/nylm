/*
 * Thumbnails (see image.h). The picture is decoded row by row and each row
 * is added into a box filter, which writes a thumbnail row as soon as the
 * rows under it are in: memory stays a few rows, whatever the picture's
 * size. A JPEG is first decoded at 1/2, 1/4 or 1/8 of its size when that is
 * still at least the thumbnail's (libjpeg does this cheaply).
 *
 * libjpeg and libpng report errors by calling back; the callbacks here
 * longjmp() to image_thumbnail(). Everything they change lives in a struct
 * in the arena, so nothing is lost by the jump.
 */
#define _POSIX_C_SOURCE 200809L

#include "image.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>

#include "arena.h"

#define THUMB_QUALITY 85

struct job;

/* libjpeg's error manager with a way back to the job. */
struct jpeg_err {
    struct jpeg_error_mgr pub;
    struct job *job;
};

struct job {
    jmp_buf jb;
    char msg[JMSG_LENGTH_MAX];

    const unsigned char *src; /* the picture */
    size_t src_size, src_at;  /* src_at: what libpng read so far */

    struct jpeg_decompress_struct dec;
    struct jpeg_err dec_err;
    int dec_made;
    png_structp png;
    png_infop png_info;

    struct jpeg_compress_struct enc;
    struct jpeg_err enc_err;
    int enc_made;
    unsigned char *buf; /* the thumbnail, malloc()ed by libjpeg */
    unsigned long buf_size;

    /* the box filter: input iw x ih into output ow x oh */
    unsigned iw, ih, ow, oh;
    unsigned y;          /* the next input row */
    unsigned oy;         /* the output row being gathered */
    unsigned rows;       /* input rows in it so far */
    uint32_t *sum;       /* ow * 3 channel sums */
    uint32_t *cols;      /* input columns under each output column */
    unsigned char *line; /* one output row */
};

/* Stops with msg: jumps back to image_thumbnail(). */
static void fail(struct job *j, const char *msg)
{
    snprintf(j->msg, sizeof j->msg, "%s", msg);
    longjmp(j->jb, 1);
}

static void on_jpeg_error(j_common_ptr cinfo)
{
    struct job *j = ((struct jpeg_err *)cinfo->err)->job;
    char buf[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, buf);
    fail(j, buf);
}

/* A warning (level -1) means damaged data: refused like an error. */
static void on_jpeg_message(j_common_ptr cinfo, int level)
{
    if (level < 0)
        on_jpeg_error(cinfo);
}

static void on_png_error(png_structp png, png_const_charp msg)
{
    fail(png_get_error_ptr(png), msg);
}

/* libpng's warnings are about chunks it skips (a colour profile, ...): the
 * picture itself is fine. */
static void on_png_warning(png_structp png, png_const_charp msg)
{
    (void)png;
    (void)msg;
}

static void read_png(png_structp png, png_bytep out, size_t len)
{
    struct job *j = png_get_io_ptr(png);
    if (len > j->src_size - j->src_at)
        png_error(png, "the picture ends too early");
    memcpy(out, j->src + j->src_at, len);
    j->src_at += len;
}

static void *alloc(struct job *j, size_t size)
{
    void *p = arena_alloc(size);
    if (p == NULL)
        fail(j, "out of memory");
    memset(p, 0, size);
    return p;
}

/* The thumbnail's size for a w x h picture: fits max_side, at least 1. */
static void thumb_size(unsigned w, unsigned h, unsigned max_side, unsigned *ow, unsigned *oh)
{
    unsigned longest = w > h ? w : h;
    if (longest <= max_side) {
        *ow = w;
        *oh = h;
        return;
    }
    uint64_t a = ((uint64_t)w * max_side + longest / 2) / longest;
    uint64_t b = ((uint64_t)h * max_side + longest / 2) / longest;
    *ow = a > 0 ? (unsigned)a : 1;
    *oh = b > 0 ? (unsigned)b : 1;
}

static void check_size(struct job *j, unsigned long w, unsigned long h)
{
    if (w < 1 || h < 1 || w > IMAGE_MAX_SIDE || h > IMAGE_MAX_SIDE)
        fail(j, "the picture is empty or bigger than 16384 pixels");
}

/* Starts the thumbnail (ow x oh) and the box filter for iw x ih rows. */
static void start_output(struct job *j, unsigned iw, unsigned ih)
{
    j->iw = iw;
    j->ih = ih;
    if (iw < j->ow || ih < j->oh)
        fail(j, "decoded smaller than the thumbnail"); /* can not happen */
    j->sum = alloc(j, (size_t)j->ow * 3 * sizeof *j->sum);
    j->cols = alloc(j, (size_t)j->ow * sizeof *j->cols);
    j->line = alloc(j, (size_t)j->ow * 3);
    for (unsigned x = 0; x < iw; x++)
        j->cols[(uint64_t)x * j->ow / iw]++;

    j->enc.err = jpeg_std_error(&j->enc_err.pub);
    j->enc_err.pub.error_exit = on_jpeg_error;
    j->enc_err.pub.emit_message = on_jpeg_message;
    j->enc_err.job = j;
    jpeg_create_compress(&j->enc);
    j->enc_made = 1;
    jpeg_mem_dest(&j->enc, &j->buf, &j->buf_size);
    j->enc.image_width = j->ow;
    j->enc.image_height = j->oh;
    j->enc.input_components = 3;
    j->enc.in_color_space = JCS_RGB;
    jpeg_set_defaults(&j->enc);
    jpeg_set_quality(&j->enc, THUMB_QUALITY, TRUE);
    jpeg_start_compress(&j->enc, TRUE);
}

/* Writes the gathered output row: each pixel the mean of its box. */
static void emit_row(struct job *j)
{
    for (unsigned ox = 0; ox < j->ow; ox++) {
        uint32_t n = j->cols[ox] * j->rows;
        for (unsigned c = 0; c < 3; c++)
            j->line[ox * 3 + c] = (unsigned char)((j->sum[ox * 3 + c] + n / 2) / n);
    }
    JSAMPROW row = j->line;
    jpeg_write_scanlines(&j->enc, &row, 1);
    memset(j->sum, 0, (size_t)j->ow * 3 * sizeof *j->sum);
    j->rows = 0;
}

/* Adds one input row (iw RGB pixels) to the box filter. */
static void add_row(struct job *j, const unsigned char *row)
{
    unsigned oy = (unsigned)((uint64_t)j->y * j->oh / j->ih);
    if (oy != j->oy && j->rows > 0)
        emit_row(j);
    j->oy = oy;
    for (unsigned x = 0; x < j->iw; x++) {
        uint32_t *s = &j->sum[(uint64_t)x * j->ow / j->iw * 3];
        s[0] += row[x * 3];
        s[1] += row[x * 3 + 1];
        s[2] += row[x * 3 + 2];
    }
    j->rows++;
    if (++j->y == j->ih) {
        emit_row(j);
        jpeg_finish_compress(&j->enc);
    }
}

static void from_jpeg(struct job *j, unsigned max_side, unsigned *width, unsigned *height)
{
    j->dec.err = jpeg_std_error(&j->dec_err.pub);
    j->dec_err.pub.error_exit = on_jpeg_error;
    j->dec_err.pub.emit_message = on_jpeg_message;
    j->dec_err.job = j;
    jpeg_create_decompress(&j->dec);
    j->dec_made = 1;
    jpeg_mem_src(&j->dec, j->src, j->src_size);
    jpeg_read_header(&j->dec, TRUE);
    unsigned w = j->dec.image_width, h = j->dec.image_height;
    check_size(j, w, h);
    if (j->dec.jpeg_color_space == JCS_CMYK || j->dec.jpeg_color_space == JCS_YCCK)
        fail(j, "CMYK JPEG is not supported");
    *width = w;
    *height = h;
    thumb_size(w, h, max_side, &j->ow, &j->oh);

    /* decode at 1/d of the size when that still covers the thumbnail */
    unsigned d = 8;
    while (d > 1 && ((w + d - 1) / d < j->ow || (h + d - 1) / d < j->oh))
        d /= 2;
    j->dec.scale_num = 1;
    j->dec.scale_denom = d;
    j->dec.out_color_space = JCS_RGB;
    jpeg_start_decompress(&j->dec);
    if (j->dec.output_components != 3)
        fail(j, "unexpected JPEG colour format");
    start_output(j, j->dec.output_width, j->dec.output_height);
    unsigned char *row = alloc(j, (size_t)j->iw * 3);
    while (j->dec.output_scanline < j->dec.output_height) {
        JSAMPROW r = row;
        if (jpeg_read_scanlines(&j->dec, &r, 1) != 1)
            fail(j, "the picture ends too early");
        add_row(j, row);
    }
    jpeg_finish_decompress(&j->dec);
}

static void from_png(struct job *j, unsigned max_side, unsigned *width, unsigned *height)
{
    j->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, j, on_png_error, on_png_warning);
    if (j->png == NULL)
        fail(j, "out of memory");
    j->png_info = png_create_info_struct(j->png);
    if (j->png_info == NULL)
        fail(j, "out of memory");
    png_set_user_limits(j->png, IMAGE_MAX_SIDE, IMAGE_MAX_SIDE);
    png_set_read_fn(j->png, j, read_png);
    png_read_info(j->png, j->png_info);
    png_uint_32 w, h;
    int depth, color, interlace;
    png_get_IHDR(j->png, j->png_info, &w, &h, &depth, &color, &interlace, NULL, NULL);
    check_size(j, w, h);
    if (interlace != PNG_INTERLACE_NONE)
        fail(j, "interlaced PNG is not supported");
    *width = (unsigned)w;
    *height = (unsigned)h;
    thumb_size(*width, *height, max_side, &j->ow, &j->oh);

    /* everything to 8-bit RGB; transparency over black */
    png_set_expand(j->png);
    png_set_strip_16(j->png);
    if ((color & PNG_COLOR_MASK_COLOR) == 0)
        png_set_gray_to_rgb(j->png);
    if ((color & PNG_COLOR_MASK_ALPHA) != 0 || png_get_valid(j->png, j->png_info, PNG_INFO_tRNS)) {
        png_color_16 black = { 0, 0, 0, 0, 0 };
        png_set_background_fixed(j->png, &black, PNG_BACKGROUND_GAMMA_SCREEN, 0, PNG_FP_1);
    }
    png_read_update_info(j->png, j->png_info);
    if (png_get_channels(j->png, j->png_info) != 3 ||
        png_get_rowbytes(j->png, j->png_info) != (size_t)w * 3)
        fail(j, "unexpected PNG colour format");
    start_output(j, *width, *height);
    unsigned char *row = alloc(j, (size_t)w * 3);
    for (png_uint_32 y = 0; y < h; y++) {
        png_read_row(j->png, row, NULL);
        add_row(j, row);
    }
    png_read_end(j->png, NULL);
}

/* Frees what libjpeg and libpng hold (not the thumbnail). */
static void finish(struct job *j)
{
    if (j->dec_made)
        jpeg_destroy_decompress(&j->dec);
    if (j->enc_made)
        jpeg_destroy_compress(&j->enc);
    if (j->png != NULL)
        png_destroy_read_struct(&j->png, j->png_info != NULL ? &j->png_info : NULL, NULL);
}

int image_thumbnail(const unsigned char *data, size_t size, unsigned max_side,
                    unsigned char **out, size_t *out_size, unsigned *width, unsigned *height,
                    char *err, size_t errlen)
{
    *out = NULL;
    *out_size = 0;
    struct job *j = arena_alloc(sizeof *j);
    if (j == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    memset(j, 0, sizeof *j);
    j->src = data;
    j->src_size = size;
    if (setjmp(j->jb) != 0) {
        finish(j);
        free(j->buf);
        snprintf(err, errlen, "%s", j->msg);
        return -1;
    }
    if (max_side < 1)
        fail(j, "invalid thumbnail size");
    if (size >= 3 && memcmp(data, "\xff\xd8\xff", 3) == 0)
        from_jpeg(j, max_side, width, height);
    else if (size >= 8 && png_sig_cmp(data, 0, 8) == 0)
        from_png(j, max_side, width, height);
    else
        fail(j, "not a JPEG or PNG picture");
    finish(j);
    *out = j->buf;
    *out_size = (size_t)j->buf_size;
    return 0;
}
