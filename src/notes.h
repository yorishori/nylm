#ifndef NOTES_H
#define NOTES_H

#include <stddef.h>

/*
 * The notes app's ids and names, without the database (src/api_notes.c
 * keeps the rest). Every reminder, note and tag has an id the device that
 * made it chose: NOTES_ID_LEN random lowercase hex characters, so a phone
 * can make them offline.
 */

#define NOTES_ID_LEN   32 /* hex characters: 128 random bits */
#define NOTES_TAG_MAX  50 /* bytes in a tag's name */

/* 1 if s is NOTES_ID_LEN lowercase hex characters. */
int notes_id_valid(const char *s);

/* A new random id into out. 0, or -1 (logged). */
int notes_new_id(char out[NOTES_ID_LEN + 1]);

/*
 * The name "<name> <n>" for a tag whose name another tag already has, cut
 * at a UTF-8 character so it fits NOTES_TAG_MAX bytes. n is 2..99; out
 * must hold NOTES_TAG_MAX + 1 bytes. 0, or -1 if n is out of range.
 */
int notes_suffix_name(const char *name, int n, char out[NOTES_TAG_MAX + 1]);

#endif
