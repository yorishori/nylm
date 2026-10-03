/* Thumbnails: JPEG and PNG pictures made here in memory, of every colour
 * format, their sizes (min, the thumbnail size, max, max + 1), and the
 * pictures refused (damaged, interlaced, CMYK, not a picture). */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>

#include <jpeglib.h>
#include <png.h>

#include "../src/arena.h"
#include "../src/image.h"
#include "test.h"

/* A picture made here: data malloc()ed. */
struct pic {
    unsigned char *data;
    size_t size;
};

/* A w x h JPEG of one colour (CMYK if cmyk). */
static struct pic make_jpeg(unsigned w, unsigned h, int cmyk)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr err;
    unsigned char *buf = NULL;
    unsigned long len = 0;
    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &buf, &len);
    c.image_width = w;
    c.image_height = h;
    c.input_components = cmyk ? 4 : 3;
    c.in_color_space = cmyk ? JCS_CMYK : JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 95, TRUE);
    jpeg_start_compress(&c, TRUE);
    unsigned char *row = malloc((size_t)w * 4);
    for (unsigned x = 0; x < w; x++) {
        row[x * (cmyk ? 4 : 3)] = 200;
        row[x * (cmyk ? 4 : 3) + 1] = 100;
        row[x * (cmyk ? 4 : 3) + 2] = 50;
        if (cmyk)
            row[x * 4 + 3] = 0;
    }
    while (c.next_scanline < h) {
        JSAMPROW r = row;
        jpeg_write_scanlines(&c, &r, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(row);
    return (struct pic){ buf, len };
}

struct mem {
    unsigned char *buf;
    size_t len, cap;
};

static void write_mem(png_structp png, png_bytep data, size_t len)
{
    struct mem *m = png_get_io_ptr(png);
    if (m->len + len > m->cap) {
        m->cap = (m->len + len) * 2;
        m->buf = realloc(m->buf, m->cap);
    }
    memcpy(m->buf + m->len, data, len);
    m->len += len;
}

static void flush_mem(png_structp png)
{
    (void)png;
}

/*
 * A w x h PNG: color is a PNG colour type, depth 8 or 16; every pixel
 * (200, 100, 50) (grey: 100) and fully opaque, except the first 16 rows,
 * which are transparent when there is alpha.
 */
static struct pic make_png(unsigned w, unsigned h, int color, int depth, int interlace)
{
    struct mem m = { NULL, 0, 0 };
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    png_set_write_fn(png, &m, write_mem, flush_mem);
    png_set_IHDR(png, info, w, h, depth, color,
                 interlace ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    if (color == PNG_COLOR_TYPE_PALETTE) {
        png_color pal[1] = { { 200, 100, 50 } };
        png_set_PLTE(png, info, pal, 1);
    }
    png_write_info(png, info);
    int channels = color == PNG_COLOR_TYPE_RGB ? 3 : color == PNG_COLOR_TYPE_RGBA ? 4
                 : color == PNG_COLOR_TYPE_GRAY_ALPHA ? 2 : 1;
    size_t bytes = (size_t)depth / 8;
    unsigned char *row = calloc((size_t)w * (size_t)channels * bytes, 1);
    unsigned char rgba[4] = { 200, 100, 50, 255 };
    unsigned char gray[2] = { 100, 255 };
    for (unsigned x = 0; x < w; x++)
        for (int c = 0; c < channels; c++)
            for (size_t b = 0; b < bytes; b++)
                row[(x * (size_t)channels + (size_t)c) * bytes + b] =
                    color == PNG_COLOR_TYPE_PALETTE ? 0
                    : color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA
                        ? gray[c] : rgba[c];
    int passes = png_set_interlace_handling(png);
    for (int p = 0; p < passes; p++) {
        for (unsigned y = 0; y < h; y++) {
            for (unsigned x = 0; x < w && (color & PNG_COLOR_MASK_ALPHA); x++)
                for (size_t b = 0; b < bytes; b++)
                    row[(x * (size_t)channels + (size_t)channels - 1) * bytes + b] =
                        y < 16 ? 0 : 255;
            png_write_row(png, row);
        }
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    free(row);
    return (struct pic){ m.buf, m.len };
}

/* The size and the first and last pixel of a JPEG (the thumbnail). */
static void read_jpeg(const unsigned char *data, size_t size, unsigned *w, unsigned *h,
                      unsigned char first[3], unsigned char last[3])
{
    struct jpeg_decompress_struct d;
    struct jpeg_error_mgr err;
    d.err = jpeg_std_error(&err);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, data, size);
    jpeg_read_header(&d, TRUE);
    d.out_color_space = JCS_RGB;
    jpeg_start_decompress(&d);
    *w = d.output_width;
    *h = d.output_height;
    unsigned char *row = malloc((size_t)d.output_width * 3);
    while (d.output_scanline < d.output_height) {
        JSAMPROW r = row;
        jpeg_read_scanlines(&d, &r, 1);
        if (d.output_scanline == 1)
            memcpy(first, row, 3);
    }
    memcpy(last, row + ((size_t)d.output_width - 1) * 3, 3);
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    free(row);
}

static int near(unsigned char a, int b)
{
    return abs((int)a - b) <= 4;
}

static char err[256];

/*
 * Makes the thumbnail of p (and frees it): 1 if it worked and the picture
 * is w x h, the thumbnail tw x th, with the last pixel (r, g, b). first
 * gets its first pixel.
 */
static int thumb_is(struct pic p, unsigned w, unsigned h, unsigned tw, unsigned th, int r, int g,
                    int b, unsigned char first[3])
{
    unsigned char *out;
    size_t out_size;
    unsigned pw = 0, ph = 0, ow = 0, oh = 0;
    unsigned char last[3] = { 0, 0, 0 };
    err[0] = '\0';
    int rc = image_thumbnail(p.data, p.size, 256, &out, &out_size, &pw, &ph, err, sizeof err);
    free(p.data);
    arena_reset();
    if (rc != 0) {
        fprintf(stderr, "thumbnail failed: %s\n", err);
        return 0;
    }
    read_jpeg(out, out_size, &ow, &oh, first, last);
    free(out);
    int ok = pw == w && ph == h && ow == tw && oh == th && near(last[0], r) &&
             near(last[1], g) && near(last[2], b);
    if (!ok)
        fprintf(stderr, "picture %ux%u thumb %ux%u last %d,%d,%d\n", pw, ph, ow, oh, last[0],
                last[1], last[2]);
    return ok;
}

/* 1 if p (freed) is refused with a reason containing why. */
static int refused(struct pic p, const char *why)
{
    unsigned char *out = (unsigned char *)"untouched";
    size_t out_size = 1;
    unsigned w, h;
    err[0] = '\0';
    int rc = image_thumbnail(p.data, p.size, 256, &out, &out_size, &w, &h, err, sizeof err);
    free(p.data);
    arena_reset();
    if (rc == 0 || out != NULL || out_size != 0 || strstr(err, why) == NULL) {
        fprintf(stderr, "not refused as '%s': rc %d, '%s'\n", why, rc, err);
        if (rc == 0)
            free(out);
        return 0;
    }
    return 1;
}

static void test_jpeg(void)
{
    unsigned char first[3];
    CHECK(thumb_is(make_jpeg(600, 300, 0), 600, 300, 256, 128, 200, 100, 50, first));
    CHECK(near(first[0], 200) && near(first[1], 100) && near(first[2], 50));
    CHECK(thumb_is(make_jpeg(300, 600, 0), 300, 600, 128, 256, 200, 100, 50, first));
    CHECK(thumb_is(make_jpeg(3000, 1000, 0), 3000, 1000, 256, 85, 200, 100, 50, first));
    CHECK(thumb_is(make_jpeg(100, 50, 0), 100, 50, 100, 50, 200, 100, 50, first));
    CHECK(thumb_is(make_jpeg(256, 256, 0), 256, 256, 256, 256, 200, 100, 50, first));
    CHECK(thumb_is(make_jpeg(257, 257, 0), 257, 257, 256, 256, 200, 100, 50, first));
    CHECK(thumb_is(make_jpeg(1, 1, 0), 1, 1, 1, 1, 200, 100, 50, first));           /* min */
    CHECK(thumb_is(make_jpeg(16384, 2, 0), 16384, 2, 256, 1, 200, 100, 50, first)); /* max */
    CHECK(refused(make_jpeg(16385, 2, 0), "bigger than 16384"));                    /* max + 1 */
    CHECK(refused(make_jpeg(2, 16385, 0), "bigger than 16384"));
    CHECK(refused(make_jpeg(40, 40, 1), "CMYK"));

    /* damaged: cut in the middle, or the end marker missing */
    struct pic j = make_jpeg(400, 400, 0);
    j.size /= 2;
    CHECK(refused(j, "")); /* "Premature end of JPEG file" */
    j = make_jpeg(400, 400, 0);
    j.size -= 2;
    CHECK(refused(j, ""));
    j = make_jpeg(400, 400, 0);
    memcpy(j.data + j.size * 3 / 4, "\xff\xd9", 2); /* an end marker inside the data */
    CHECK(refused(j, "Corrupt JPEG data"));
}

static void test_png(void)
{
    unsigned char first[3];
    CHECK(thumb_is(make_png(512, 256, PNG_COLOR_TYPE_RGB, 8, 0), 512, 256, 256, 128, 200,
                   100, 50, first));
    CHECK(thumb_is(make_png(100, 60, PNG_COLOR_TYPE_RGB, 16, 0), 100, 60, 100, 60, 200, 100,
                   50, first));
    CHECK(thumb_is(make_png(100, 60, PNG_COLOR_TYPE_GRAY, 8, 0), 100, 60, 100, 60, 100, 100,
                   100, first));
    CHECK(thumb_is(make_png(300, 30, PNG_COLOR_TYPE_PALETTE, 8, 0), 300, 30, 256, 26, 200,
                   100, 50, first));
    /* transparency over black: the first rows are clear */
    CHECK(thumb_is(make_png(64, 64, PNG_COLOR_TYPE_RGBA, 8, 0), 64, 64, 64, 64, 200, 100,
                   50, first));
    CHECK(first[0] < 40 && first[1] < 40 && first[2] < 40);
    CHECK(thumb_is(make_png(64, 64, PNG_COLOR_TYPE_GRAY_ALPHA, 8, 0), 64, 64, 64, 64, 100,
                   100, 100, first));
    CHECK(thumb_is(make_png(1, 1, PNG_COLOR_TYPE_RGB, 8, 0), 1, 1, 1, 1, 200, 100, 50,
                   first));                                                                /* min */
    CHECK(thumb_is(make_png(1, 16384, PNG_COLOR_TYPE_RGB, 8, 0), 1, 16384, 1, 256, 200,
                   100, 50, first));                                                       /* max */
    CHECK(refused(make_png(1, 16385, PNG_COLOR_TYPE_RGB, 8, 0), ""));               /* max + 1 */
    CHECK(refused(make_png(64, 64, PNG_COLOR_TYPE_RGB, 8, 1), "interlaced"));

    struct pic p = make_png(200, 200, PNG_COLOR_TYPE_RGB, 8, 0);
    p.size /= 2;
    CHECK(refused(p, "ends too early"));
    p = make_png(200, 200, PNG_COLOR_TYPE_RGB, 8, 0);
    p.size = 20;
    CHECK(refused(p, "ends too early"));
}

static struct pic copy(const char *s, size_t n)
{
    unsigned char *p = malloc(n + 1);
    memcpy(p, s, n);
    return (struct pic){ p, n };
}

static void test_not_pictures(void)
{
    CHECK(refused(copy("", 0), "not a JPEG or PNG"));
    CHECK(refused(copy("\xff\xd8", 2), "not a JPEG or PNG"));
    CHECK(refused(copy("GIF89a\1\0\1\0", 10), "not a JPEG or PNG"));
    CHECK(refused(copy("<svg/>", 6), "not a JPEG or PNG"));
    CHECK(refused(copy("\xff\xd8\xff not a jpeg at all", 22), ""));
    CHECK(refused(copy("\x89PNG\r\n\x1a\n not a png", 18), ""));

    unsigned char *out;
    size_t out_size;
    unsigned w, h;
    struct pic j = make_jpeg(10, 10, 0);
    CHECK(image_thumbnail(j.data, j.size, 0, &out, &out_size, &w, &h, err, sizeof err) == -1);
    CHECK(strstr(err, "thumbnail size") != NULL);
    free(j.data);
    arena_reset();
}

int main(void)
{
    if (arena_init(1024 * 1024) != 0)
        return 1;
    test_jpeg();
    test_png();
    test_not_pictures();
    TEST_DONE();
}
