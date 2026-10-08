#pragma once
/*
 * User table (docs/APP_PROTOCOL.md "User model"): one record per person = name + the AS608 fingerprint slot + up to 5
 * esp-dl face feature ids. This is what closes the binding gap: the door only opens when the matched face belongs to
 * the SAME user as the matched finger. Pure logic, no ESP-IDF; the caller persists the blob (NVS) and deletes the
 * underlying templates when a user is removed.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USER_MAX 8
#define USER_NAME_MAX 24 /* 23 usable bytes + NUL */
#define USER_FACES_MAX 5
#define USER_BLOB_LEN 317 /* fixed size of the serialised table */

typedef struct {
    uint16_t id; /* 0 = unused slot. Assigned by the device, monotonic, never reused. */
    char name[USER_NAME_MAX];
    uint16_t fp_slot;
    uint8_t n_faces;
    uint16_t face_ids[USER_FACES_MAX];
} user_rec_t;

typedef struct {
    uint16_t next_id;
    user_rec_t u[USER_MAX];
} user_store_t;

void user_store_init(user_store_t *s);
int user_store_count(const user_store_t *s);

/* 1..23 bytes after trimming, printable ASCII or UTF-8 (no control characters, no '"' or '\\' so it can sit in JSON
 * unescaped). Writes the trimmed name to out (size USER_NAME_MAX). */
bool user_store_clean_name(const char *in, char out[USER_NAME_MAX]);

/* Returns the new user's id, or 0 if: table full, name invalid, n_faces not 1..5, or the fingerprint slot / any face
 * id is already bound to another user. */
uint16_t user_store_add(user_store_t *s, const char *name, uint16_t fp_slot, const uint16_t *face_ids, uint8_t n_faces);

bool user_store_get(const user_store_t *s, uint16_t id, user_rec_t *out);
/* Removes the user and hands the record back so the caller can delete its fingerprint template and face features. */
bool user_store_remove(user_store_t *s, uint16_t id, user_rec_t *removed);

/* True when face_id is one of the faces of the user that owns fp_slot. */
bool user_store_face_matches_finger(const user_store_t *s, uint16_t fp_slot, uint16_t face_id);
bool user_store_has_fp_slot(const user_store_t *s, uint16_t fp_slot);
bool user_store_has_face(const user_store_t *s, uint16_t face_id); /* is this stored face id part of any user? */

size_t user_store_serialize(const user_store_t *s, uint8_t out[USER_BLOB_LEN]);
bool user_store_deserialize(user_store_t *s, const uint8_t *blob, size_t len); /* validates; *s untouched on failure */

/* esp-dl face database file: {uint16 total, uint16 valid, uint16 feat_len} followed by `total` records of
 * {uint16 id (0 = deleted), float feat[feat_len]}. The recognizer does NOT report that stored id for a match: it reports
 * the 1-based POSITION among the valid records (DataBase::query_feat), while deletion takes the stored id. The two only
 * agree until the first deletion. This returns the stored id of the record at `position`, or -1. */
int facedb_stored_id_at(FILE *f, int position);

#ifdef __cplusplus
}
#endif
