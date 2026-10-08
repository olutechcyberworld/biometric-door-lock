// Host test for user_store. Build/run per tests/run_host_tests.sh.
#include <cstdio>
#include <cstring>
#include "user_store.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    user_store_t s;
    user_store_init(&s);
    CHECK(user_store_count(&s) == 0);

    char n[USER_NAME_MAX];
    CHECK(user_store_clean_name("  Olumide  ", n) && !strcmp(n, "Olumide"));
    CHECK(!user_store_clean_name("", n) && !user_store_clean_name("   ", n) && !user_store_clean_name(nullptr, n));
    CHECK(!user_store_clean_name("a\"b", n) && !user_store_clean_name("a\\b", n) && !user_store_clean_name("a\nb", n));
    CHECK(user_store_clean_name("1234567890123456789012 3", n) == false); // 24 chars: one too many
    CHECK(user_store_clean_name("12345678901234567890123", n));            // 23 chars: fits

    uint16_t fa[5] = {1, 2, 3, 4, 5}, fb[5] = {6, 7, 8, 9, 10};
    uint16_t a = user_store_add(&s, "Ada", 3, fa, 5);
    CHECK(a == 1);
    uint16_t b = user_store_add(&s, "Bayo", 4, fb, 5);
    CHECK(b == 2 && user_store_count(&s) == 2);

    // binding: the face must belong to the SAME user as the finger
    CHECK(user_store_face_matches_finger(&s, 3, 2));
    CHECK(user_store_face_matches_finger(&s, 4, 9));
    CHECK(!user_store_face_matches_finger(&s, 3, 9)); // Ada's finger, Bayo's face -> no
    CHECK(!user_store_face_matches_finger(&s, 4, 1)); // Bayo's finger, Ada's face -> no
    CHECK(!user_store_face_matches_finger(&s, 99, 1)); // finger not in any user
    CHECK(!user_store_face_matches_finger(&s, 3, 99)); // face not in any user

    CHECK(user_store_has_face(&s, 7) && user_store_has_face(&s, 1) && !user_store_has_face(&s, 99) && !user_store_has_face(&s, 0));

    // rejected adds: reused finger slot, reused face id, duplicate inside the list, bad counts, bad name
    uint16_t fc[2] = {20, 21}, fdup[2] = {30, 30}, fclash[1] = {5};
    CHECK(user_store_add(&s, "C", 3, fc, 2) == 0);
    CHECK(user_store_add(&s, "C", 5, fclash, 1) == 0);
    CHECK(user_store_add(&s, "C", 5, fdup, 2) == 0);
    CHECK(user_store_add(&s, "C", 5, fc, 0) == 0 && user_store_add(&s, "C", 5, fc, 6) == 0);
    CHECK(user_store_add(&s, "", 5, fc, 2) == 0);
    CHECK(user_store_count(&s) == 2);

    // serialise / deserialise round trip
    uint8_t blob[USER_BLOB_LEN];
    CHECK(user_store_serialize(&s, blob) == USER_BLOB_LEN);
    user_store_t r;
    user_store_init(&r);
    CHECK(user_store_deserialize(&r, blob, sizeof(blob)));
    user_rec_t rec;
    CHECK(user_store_get(&r, 2, &rec) && !strcmp(rec.name, "Bayo") && rec.fp_slot == 4 && rec.n_faces == 5 && rec.face_ids[4] == 10);
    CHECK(user_store_face_matches_finger(&r, 3, 5));

    // corrupt / foreign blobs are refused and leave the target untouched
    uint8_t bad[USER_BLOB_LEN];
    memcpy(bad, blob, sizeof(bad)); bad[0] = 'X';
    user_store_t t; user_store_init(&t);
    CHECK(!user_store_deserialize(&t, bad, sizeof(bad)) && user_store_count(&t) == 0);
    CHECK(!user_store_deserialize(&t, blob, sizeof(blob) - 1));
    memcpy(bad, blob, sizeof(bad)); bad[3 + 2 + 2 + 2] = 0; bad[3 + 2 + 2 + 2 + 1] = 0; // user 1's fp slot -> 0? still unique
    memcpy(bad, blob, sizeof(bad)); bad[5 + 39 + 0] = 1; bad[5 + 39 + 1] = 0;           // user 2 gets id 1 -> duplicate id
    CHECK(!user_store_deserialize(&t, bad, sizeof(bad)));
    memcpy(bad, blob, sizeof(bad)); bad[5 + 4] = 9;                                      // user 1: n_faces = 9
    CHECK(!user_store_deserialize(&t, bad, sizeof(bad)));

    // remove hands the record back; ids are never reused
    CHECK(user_store_remove(&s, a, &rec) && rec.fp_slot == 3 && rec.n_faces == 5);
    CHECK(!user_store_remove(&s, a, nullptr) && !user_store_get(&s, a, nullptr));
    CHECK(!user_store_face_matches_finger(&s, 3, 1)); // her finger no longer opens anything
    CHECK(user_store_count(&s) == 1);
    uint16_t c = user_store_add(&s, "Chi", 3, fa, 5); // finger slot + faces are free again
    CHECK(c == 3);

    // table full
    user_store_t f; user_store_init(&f);
    for (uint16_t i = 0; i < USER_MAX; i++) {
        uint16_t face = (uint16_t)(100 + i);
        CHECK(user_store_add(&f, "U", (uint16_t)(10 + i), &face, 1) == i + 1);
    }
    uint16_t face = 500;
    CHECK(user_store_add(&f, "U", 500, &face, 1) == 0);

    // face database: recognizer positions vs stored ids after deletions (the bug that broke the first real unlock)
    {
        FILE *f = tmpfile();
        CHECK(f != nullptr);
        uint16_t meta[3] = {6, 4, 3}; // total 6 ever stored, 4 still valid, 3 floats per feature
        fwrite(meta, sizeof(uint16_t), 3, f);
        const uint16_t ids[6] = {1, 0, 3, 0, 5, 6}; // ids 2 and 4 were deleted
        for (int i = 0; i < 6; i++) {
            float feat[3] = {0, 0, 0};
            fwrite(&ids[i], sizeof(uint16_t), 1, f);
            fwrite(feat, sizeof(float), 3, f);
        }
        fflush(f);
        CHECK(facedb_stored_id_at(f, 1) == 1);
        CHECK(facedb_stored_id_at(f, 2) == 3); // position 2 is stored id 3
        CHECK(facedb_stored_id_at(f, 3) == 5);
        CHECK(facedb_stored_id_at(f, 4) == 6);
        CHECK(facedb_stored_id_at(f, 5) == -1 && facedb_stored_id_at(f, 0) == -1 && facedb_stored_id_at(f, -3) == -1);
        CHECK(facedb_stored_id_at(nullptr, 1) == -1);
        fclose(f);
    }

    printf("user_store: all tests passed\n");
    return 0;
}
