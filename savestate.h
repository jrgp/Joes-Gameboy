#ifndef SAVESTATE_H
#define SAVESTATE_H

#include <stdbool.h>
#include <stddef.h>

#define SAVESTATE_FORMAT_VERSION 1

/*
 * Save all emulator state to a CBOR file at `path`.
 * Returns true on success, false on failure (error printed to stderr).
 */
bool save_state(const char *path);

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

#endif /* SAVESTATE_H */
