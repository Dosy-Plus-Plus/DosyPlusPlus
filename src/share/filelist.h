/* filelist.h - files.xml, written straight to disk.
 *
 * Streamed, never assembled in memory. A 10,000-file share produces several
 * megabytes of XML; building that in a buffer on a 16MB machine is the sort
 * of thing that works in testing and dies on the target.
 *
 * Written twice: once as plain XML, then compressed to .bz2, because that
 * is what a peer asking for a list receives. The compressed copy is what
 * goes on the wire; the plain one stays because it costs nothing to keep
 * and is the only way to see what we are publishing.
 *
 * The local names are 8.3 -- DOS does not have "files.xml.bz2" -- while the
 * name on the wire is the long one every client expects. The two are not
 * the same string and conflating them produces a list nobody can fetch.
 */
#ifndef DOSYPP_SHARE_FILELIST_H
#define DOSYPP_SHARE_FILELIST_H

#include "index.h"

#define FILELIST_FILE "FILES.XML"

/* The compressed copy, as it is stored locally (8.3) and as it is named on
   the wire (what a peer asks for in CGET). */
#define FILELIST_BZ2       "FILES.BZ2"
#define FILELIST_WIRE_NAME "files.xml.bz2"

/* Where the leaf hashes we are about to serve are staged. A peer asking for
   `tthl` wants the Merkle tree's intermediate hashes so it can verify parts
   of a file independently; we have no store of them, so they are recomputed
   into this file and served from it. 8.3, and overwritten per request. */
#define TTHL_FILE "TTHL.TMP"

/* Where a partial list is staged before it is served. A peer asking for one
   directory gets a list generated on the spot: there is no point keeping
   per-directory lists for a share nobody may ever browse, and a 16MB
   machine has nowhere to keep them. Both 8.3, both overwritten per
   request. */
#define FILELIST_PART_XML "PARTLIST.XML"
#define FILELIST_PART_BZ2 "PARTLIST.BZ2"

/* Where a peer's fetched list is kept while it is being read. Also 8.3, and
   deliberately distinct from our own: confusing the two would mean
   publishing someone else's share as ours. */
#define FILELIST_PEER_BZ2 "PEERLIST.BZ2"

/* Write the share as files.xml. `cid` is our own, which the format carries.
   Returns the number of files written, or -1 on failure. */
int filelist_write(const share_index *ix, const char *path,
                   const char *cid);

/* Write files.xml and then files.xml.bz2 beside it. Returns the number of
   files written, or -1 if either step failed -- a missing or stale .bz2
   means peers get an old list or none, so the failure must be visible.
   bz_error() carries the reason when compression is what failed. */
int filelist_publish(const share_index *ix, const char *xml_path,
                     const char *bz2_path, const char *cid);

/* Write the part of the share under `base` as a partial files.xml.
 *
 * `base` is a virtual path with leading and trailing slashes, as the
 * protocol sends it: "/" for the root, "/warez/apps/" for a subdirectory.
 * The list carries it as the Base attribute, which is how the peer knows
 * what it is looking at.
 *
 * Not recursive by default, which is the point: the immediate files are
 * listed in full and each immediate subdirectory appears as an empty
 * element marked Incomplete="1" -- "there is more here, ask for it". With
 * `recursive` the whole subtree is written, which is what a client asking
 * with RE1 wants.
 *
 * Returns the number of files written, 0 for a directory that exists but
 * holds no hashed files, or -1 if `base` names nothing in the share --
 * which the caller must report as a refusal rather than sending an empty
 * list, because "that directory is not there" and "that directory is
 * empty" are different answers. */
int filelist_write_partial(const share_index *ix, const char *base,
                           int recursive, const char *path,
                           const char *cid);

/* The same, then compressed, which is what goes on the wire. */
int filelist_publish_partial(const share_index *ix, const char *base,
                             int recursive, const char *xml_path,
                             const char *bz2_path, const char *cid);

/* Escape XML metacharacters into dst. Exposed because getting this wrong
   produces a file list that peers silently reject, and it is worth a test
   of its own. */
int filelist_escape(char *dst, size_t dstsz, const char *src);

#endif /* DOSYPP_SHARE_FILELIST_H */
