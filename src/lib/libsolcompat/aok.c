/*
 * This variable suppresses unused variable assertions in debug builds.
 * It is declared in zfs_context.h.
 *
 * It was previously defined in libzpool/unique.c, but we need it available
 * for all consumers of the ZFS libraries (like zpool command) which might
 * not link against libzpool but do use libzfs and assertions.
 */
int aok;
