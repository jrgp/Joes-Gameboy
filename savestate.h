#ifndef SAVESTATE_H
#define SAVESTATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAVESTATE_FORMAT_VERSION 1
#define SS_NUM_SLOTS             5    /* number of named save slots */

/*
 * Save all emulator state to a CBOR file at `path`.
 * Returns true on success, false on failure (error printed to stderr).
 */
bool save_state(const char *path);

/*
 * Save state to `path` with an optional slot name and timestamp embedded
 * as metadata fields ("slot_name", "save_ts") in the CBOR map.
 * Pass NULL or "" for slot_name to omit metadata (identical to save_state).
 * Existing save files without metadata remain valid and loadable.
 */
bool save_state_slot(const char *path, const char *slot_name);

/*
 * Load emulator state from a CBOR file at `path`.
 * Also loads the ROM that was active when the state was saved.
 * Returns true on success, false on failure (error printed to stderr).
 */
bool load_state(const char *path);

/*
 * Returns true if `path` looks like a save-state file (.cbor extension).
 */
bool savestate_is_cbor_path(const char *path);

/*
 * Derive the default save-state path for a ROM: replace extension with .cbor.
 * Writes into `out` (size `out_size`).  Returns out.
 */
char *savestate_default_path(const char *rom_path, char *out, size_t out_size);

/*
 * Derive the slot-N save-state path for a ROM: "game.slotN.cbor".
 * slot must be 1..SS_NUM_SLOTS.
 * Writes into `out` (size `out_size`).  Returns out.
 */
char *savestate_slot_path(const char *rom_path, int slot, char *out, size_t out_size);

/*
 * Read only the metadata fields from a CBOR save-state file, without
 * loading any emulator state.  Returns false if the file does not exist
 * or cannot be parsed (name_out set to "" and *ts_out set to 0).
 * Either output pointer may be NULL.
 */
bool slot_read_meta(const char *path, char *name_out, size_t name_size,
                    int64_t *ts_out);

#endif /* SAVESTATE_H */
