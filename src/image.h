#ifndef IMAGE_H
#define IMAGE_H

#include <stddef.h>

/*
 * Decoding pictures (libjpeg-turbo, libpng) to make thumbnails. Only the
 * services use it, never the server: a picture's bytes come from music
 * files or uploads and are untrusted.
 */

#define IMAGE_MAX_SIDE 16384 /* pixels: a wider or higher picture is refused */

/*
 * Decodes a JPEG or PNG picture completely (any error or warning, e.g.
 * missing data, refuses it) and makes a JPEG thumbnail no wider or higher
 * than max_side, keeping the proportions (a smaller picture keeps its
 * size). *width and *height: the picture's size. *out: the thumbnail,
 * malloc()ed (free() it), *out_size its bytes. 0, or -1 with the reason in
 * err. Interlaced PNG and CMYK JPEG are not supported.
 */
int image_thumbnail(const unsigned char *data, size_t size, unsigned max_side,
                    unsigned char **out, size_t *out_size, unsigned *width, unsigned *height,
                    char *err, size_t errlen);

#endif
