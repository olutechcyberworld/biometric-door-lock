#pragma once
// NVS-backed instance of components/user_store: who is enrolled and which finger belongs to which face(s).
#include <cstddef>
#include <cstdint>
#include "user_store.h"

void user_db_start(); // after NVS is initialised (wifi_manager_start does it)

// The door's binding rule. With NO users recorded (a board enrolled only through the console) this returns true for
// any pair, exactly like before, and logs a one-line warning; as soon as one user exists the face MUST belong to the
// user that owns the matched finger.
bool user_db_authorize(uint16_t fp_slot, uint16_t face_id);

int user_db_count();
bool user_db_has_fp_slot(uint16_t fp_slot);
uint16_t user_db_add(const char *name, uint16_t fp_slot, const uint16_t *face_ids, uint8_t n_faces); // 0 = failed
bool user_db_remove(uint16_t id, user_rec_t *removed);                                                // persists
// {"type":"user_list","users":[{"id":"1","name":"Ada"}]}  - false if cap is too small
bool user_db_list_json(char *out, size_t cap);
bool user_db_get(uint16_t id, user_rec_t *out);
// Serial 'U' twice within 5 s: delete every fingerprint template that belongs to no user. Refuses while no user exists.
void user_db_console_prune();
bool user_db_has_face_id(uint16_t face_id);
// Serial 'F' twice within 5 s: clean the face database. With no users it wipes it completely (ids restart at 1); with users
// it deletes only faces that belong to no user (console leftovers) and keeps what the app enrolled.
void user_db_console_prune_faces();

