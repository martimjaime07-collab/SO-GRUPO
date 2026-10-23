#ifndef FILESYSTEM__H
#define FILESYSTEM__H

#include <stddef.h>
#include <sys/types.h>

/**
 * Checks whether a path exists and is a directory.
 *
 * @param path Directory path.
 *
 * @return 1 if it exists and is a directory, 0 otherwise.
 */
int path_exists(const char *path);

/**
 * Checks whether a path exists and is a regular file.
 *
 * @param path File path.
 *
 * @return 1 if it exists and is a regular file, 0 otherwise.
 */
int file_exists(const char *path);

/**
 * Converts the given path into an absolute path, resolving symbolic links,
 * relative components ('.' and '..'), and redundant separators. The resolved
 * path is copied into the provided buffer.
 *
 * @param path Path to resolve.
 * @param buffer Destination buffer where the absolute path will be stored.
 * @param size Size of the destination buffer, in bytes.
 *
 * @return 0 if the path was successfully resolved and copied to the buffer
 * @return 1 if the path could not be resolved or the buffer is too small.
 */
int absolute_path(const char *path, char *buffer, size_t size);

/**
 * Lists all files with a `.conf` extension in the given directory,
 * sorted alphabetically.
 *
 * On success, `*buffer` is set to a newly allocated array of `n` strings,
 * where `n` is the returned value. Each string is the name of a `.conf`
 * file (without the directory). The caller is responsible for freeing each string and the array
 * itself.
 *
 * On failure (e.g. the directory cannot be opened, or memory allocation
 * fails), `*buffer` is set to NULL and -1 is returned.
 *
 * If the directory contains no `.conf` files, 0 is returned and `*buffer`
 * is still an allocated (empty) array that the caller must free.
 *
 * @param path   Directory to search.
 * @param buffer Output parameter receiving the allocated array of paths.
 *
 * @return The number of `.conf` files found, or -1 on error.
 */
ssize_t list_conf_files(const char *path, char ***buffer);

/**
 * Writes into buffer the base folder of CloudIST for the current user:
 * /tmp/CloudIST-<user> (on the RNL cluster, <user> is the Tecnico ID).
 *
 * @return 0 on success, -1 on error.
 */
int cloudist_base_dir(char *buffer, size_t size);

/**
 * Creates the folder /tmp/CloudIST/<res_id>/<vm_id> (and the missing parents)
 * and recursively copies the contents of vm_folder into it, keeping the
 * subfolders and the file permissions.
 *
 * @param res_id    Reservation id.
 * @param vm_id     VM id.
 * @param vm_folder Input folder given in the D command.
 *
 * @return 0 on success, -1 on error.
 */
int create_dir(const char *res_id, const char *vm_id, const char *vm_folder);

#endif // FILESYSTEM__H