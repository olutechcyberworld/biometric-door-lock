#include "user_store.h"

#include <string.h>

void user_store_init(user_store_t *s)
{
    memset(s, 0, sizeof(*s));
    s->next_id = 1;
}

int user_store_count(const user_store_t *s)
{
    int n = 0;
    for (int i = 0; i < USER_MAX; i++) {
        if (s->u[i].id) n++;
    }
    return n;
}

bool user_store_clean_name(const char *in, char out[USER_NAME_MAX])
{
    if (!in) return false;
    while (*in == ' ') in++;
    size_t len = strlen(in);
    while (len && in[len - 1] == ' ') len--;
    if (len < 1 || len > USER_NAME_MAX - 1) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == 0x7F || c == '"' || c == '\\') return false;
    }
    memcpy(out, in, len);
    out[len] = 0;
    return true;
}

static bool fp_used(const user_store_t *s, uint16_t fp)
{
    for (int i = 0; i < USER_MAX; i++)
        if (s->u[i].id && s->u[i].fp_slot == fp) return true;
    return false;
}

static bool face_used(const user_store_t *s, uint16_t face)
{
    for (int i = 0; i < USER_MAX; i++)
        for (int k = 0; k < s->u[i].n_faces; k++)
            if (s->u[i].id && s->u[i].face_ids[k] == face) return true;
    return false;
}

uint16_t user_store_add(user_store_t *s, const char *name, uint16_t fp_slot, const uint16_t *face_ids, uint8_t n_faces)
{
    char clean[USER_NAME_MAX];
    if (!face_ids || n_faces < 1 || n_faces > USER_FACES_MAX || !user_store_clean_name(name, clean)) return 0;
    if (s->next_id == 0 || fp_used(s, fp_slot)) return 0;
    for (int k = 0; k < n_faces; k++) {
        if (face_used(s, face_ids[k])) return 0;
        for (int j = 0; j < k; j++)
            if (face_ids[j] == face_ids[k]) return 0;
    }
    for (int i = 0; i < USER_MAX; i++) {
        if (s->u[i].id == 0) {
            memset(&s->u[i], 0, sizeof(s->u[i]));
            s->u[i].id = s->next_id++;
            memcpy(s->u[i].name, clean, strlen(clean) + 1);
            s->u[i].fp_slot = fp_slot;
            s->u[i].n_faces = n_faces;
            memcpy(s->u[i].face_ids, face_ids, n_faces * sizeof(uint16_t));
            return s->u[i].id;
        }
    }
    return 0;
}

bool user_store_get(const user_store_t *s, uint16_t id, user_rec_t *out)
{
    if (id == 0) return false;
    for (int i = 0; i < USER_MAX; i++) {
        if (s->u[i].id == id) {
            if (out) *out = s->u[i];
            return true;
        }
    }
    return false;
}

bool user_store_remove(user_store_t *s, uint16_t id, user_rec_t *removed)
{
    if (id == 0) return false;
    for (int i = 0; i < USER_MAX; i++) {
        if (s->u[i].id == id) {
            if (removed) *removed = s->u[i];
            memset(&s->u[i], 0, sizeof(s->u[i]));
            return true;
        }
    }
    return false;
}

bool user_store_has_fp_slot(const user_store_t *s, uint16_t fp_slot)
{
    return fp_used(s, fp_slot);
}

bool user_store_has_face(const user_store_t *s, uint16_t face_id)
{
    return face_used(s, face_id);
}

bool user_store_face_matches_finger(const user_store_t *s, uint16_t fp_slot, uint16_t face_id)
{
    for (int i = 0; i < USER_MAX; i++) {
        if (s->u[i].id && s->u[i].fp_slot == fp_slot) {
            for (int k = 0; k < s->u[i].n_faces; k++)
                if (s->u[i].face_ids[k] == face_id) return true;
            return false;
        }
    }
    return false;
}

/* blob: 'U' 'S' version(1) next_id(2) then USER_MAX x { id(2) fp(2) n(1) name(24) faces(5x2) } = 5 + 8*39 = 317. */
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

size_t user_store_serialize(const user_store_t *s, uint8_t out[USER_BLOB_LEN])
{
    uint8_t *p = out;
    *p++ = 'U'; *p++ = 'S'; *p++ = 1;
    w16(p, s->next_id); p += 2;
    for (int i = 0; i < USER_MAX; i++) {
        const user_rec_t *u = &s->u[i];
        w16(p, u->id); p += 2;
        w16(p, u->fp_slot); p += 2;
        *p++ = u->n_faces;
        memcpy(p, u->name, USER_NAME_MAX); p += USER_NAME_MAX;
        for (int k = 0; k < USER_FACES_MAX; k++) { w16(p, u->face_ids[k]); p += 2; }
    }
    return (size_t)(p - out);
}

bool user_store_deserialize(user_store_t *s, const uint8_t *blob, size_t len)
{
    if (!blob || len != USER_BLOB_LEN || blob[0] != 'U' || blob[1] != 'S' || blob[2] != 1) return false;
    user_store_t t;
    memset(&t, 0, sizeof(t));
    const uint8_t *p = blob + 3;
    t.next_id = r16(p); p += 2;
    if (t.next_id == 0) return false;
    for (int i = 0; i < USER_MAX; i++) {
        user_rec_t *u = &t.u[i];
        u->id = r16(p); p += 2;
        u->fp_slot = r16(p); p += 2;
        u->n_faces = *p++;
        memcpy(u->name, p, USER_NAME_MAX); p += USER_NAME_MAX;
        for (int k = 0; k < USER_FACES_MAX; k++) { u->face_ids[k] = r16(p); p += 2; }
        if (u->id == 0) {
            memset(u, 0, sizeof(*u)); /* unused slot: contents are irrelevant, keep them canonical */
            continue;
        }
        if (u->id >= t.next_id || u->n_faces < 1 || u->n_faces > USER_FACES_MAX || u->name[USER_NAME_MAX - 1] != 0) return false;
        char clean[USER_NAME_MAX];
        if (!user_store_clean_name(u->name, clean) || strcmp(clean, u->name) != 0) return false;
        for (int k = u->n_faces; k < USER_FACES_MAX; k++) u->face_ids[k] = 0;
    }
    /* ids, fingerprint slots and face ids must be unique across the table */
    for (int i = 0; i < USER_MAX; i++) {
        if (!t.u[i].id) continue;
        for (int j = i + 1; j < USER_MAX; j++) {
            if (!t.u[j].id) continue;
            if (t.u[i].id == t.u[j].id || t.u[i].fp_slot == t.u[j].fp_slot) return false;
            for (int a = 0; a < t.u[i].n_faces; a++)
                for (int b = 0; b < t.u[j].n_faces; b++)
                    if (t.u[i].face_ids[a] == t.u[j].face_ids[b]) return false;
        }
    }
    *s = t;
    return true;
}

int facedb_stored_id_at(FILE *f, int position)
{
    if (!f || position < 1) return -1;
    uint16_t meta[3];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(meta, sizeof(uint16_t), 3, f) != 3) return -1;
    const long rec = (long)sizeof(uint16_t) + (long)sizeof(float) * meta[2];
    int seen = 0;
    for (int i = 0; i < meta[0]; i++) {
        uint16_t id = 0;
        if (fseek(f, (long)sizeof(meta) + rec * i, SEEK_SET) != 0 || fread(&id, sizeof(id), 1, f) != 1) return -1;
        if (id == 0) continue; /* deleted record: not part of the recognizer's list */
        if (++seen == position) return id;
    }
    return -1;
}
