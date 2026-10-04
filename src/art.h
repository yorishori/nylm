#ifndef ART_H
#define ART_H

#include <stddef.h>

/*
 * Pictures stored once each under the SHA-256 of their bytes, in a store:
 *   <hash>        the picture as it is
 *   <hash>.thumb  a JPEG at most ART_THUMB_SIZE pixels wide and high
 *
 * ART_MUSIC (<NYLM_DATA>/music/art/): album art. The art table in music_db
 * lists it (mime type, size, width, height, whether there is a thumbnail).
 * The scan stores the pictures it reads, the server stores uploaded
 * covers, and the scan of the whole library removes those nothing uses.
 *
 * ART_PLANTS (<NYLM_DATA>/plants/photos/): photos of the plant journal, a
 * JPEG and its thumbnail each, both made by the browser; the care_photos
 * table in plants_db lists them.
 */

#define ART_HASH_LEN    64          /* lowercase hex characters of a SHA-256 */
#define ART_THUMB_SIZE  256         /* pixels, the longer side of a thumbnail */
#define ART_MAX_UPLOAD  (700 * 1024) /* bytes in an uploaded cover (its base64 fits a body) */

enum art_store { ART_MUSIC, ART_PLANTS, ART_STORES };

/* Sets the stores' folders below data_dir. 0, or -1 if a path is too long
 * (logged). A folder is made when first used. */
int art_configure(const char *data_dir);

/* 1 if s is ART_HASH_LEN lowercase hex characters. */
int art_hash_valid(const char *s);

/* The SHA-256 of data as lowercase hex into out. 0, or -1 (logged). */
int art_hash(const unsigned char *data, size_t size, char out[ART_HASH_LEN + 1]);

/* The type of a picture from its first bytes: "image/jpeg", "image/png",
 * "image/gif", "image/webp", or NULL for anything else. */
const char *art_mime(const unsigned char *data, size_t size);

/*
 * Decodes base64 (RFC 4648: A-Z a-z 0-9 + /, '=' padding to a multiple of
 * 4 characters, no line breaks, unused bits zero) of len characters into
 * out. The number of bytes; -1 if s is not valid base64, -2 if it decodes
 * to more than max bytes.
 */
long art_base64_decode(const char *s, size_t len, unsigned char *out, size_t max);

/* Stores data in store as the file of hash (thumb: its thumbnail) unless
 * it is there, through a temporary file and a rename. 0, or -1 (logged). */
int art_save(enum art_store store, const char *hash, int thumb, const unsigned char *data, size_t size);

/* Opens store's file of hash (thumb: its thumbnail) for reading: an fd, or -1
 * (errno ENOENT if it is not there; anything else logged). */
int art_open(enum art_store store, const char *hash, int thumb);

/* Reads store's whole file of hash into out (max bytes). Its size; -1 on error
 * (logged), -2 if it is bigger than max. */
long art_load(enum art_store store, const char *hash, unsigned char *out, size_t max);

/*
 * Removes the files in store's folder whose hash keep() says to drop (returns
 * 0; 1 keeps, -1 is an error that stops the sweep), and any temporary file
 * left by a cut-off save. Other names are left alone. The number removed,
 * or -1 (logged).
 */
long art_sweep(enum art_store store, int (*keep)(const char *hash, void *ctx), void *ctx);

#endif
