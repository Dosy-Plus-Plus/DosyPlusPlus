/* filelist.c - streaming files.xml generation. */
#include "filelist.h"
#include "bz.h"
#include "../common.h"

#include <stdio.h>
#include <string.h>

int filelist_escape(char *dst, size_t dstsz, const char *src)
{
    size_t o = 0;

    if (dstsz == 0)
        return -1;

    for (; *src; src++) {
        const char *rep = NULL;
        size_t rlen;

        switch (*src) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        default:   break;
        }

        if (rep) {
            rlen = strlen(rep);
            if (o + rlen >= dstsz)
                return -1;
            memcpy(dst + o, rep, rlen);
            o += rlen;
            continue;
        }

        /* Control characters are not legal in XML at all; a filename
           carrying one would make the whole list unparseable for the peer,
           so drop them rather than emit invalid XML. */
        if ((unsigned char)*src < 0x20)
            continue;

        if (o + 1 >= dstsz)
            return -1;
        dst[o++] = *src;
    }

    dst[o] = '\0';
    return (int)o;
}

/* files.xml nests Directory elements. Emitting that from a flat, sorted
   index means tracking how much of the previous path still applies:
   close what no longer matches, open what is new. Doing it this way is
   what keeps the whole thing streaming. */
static int common_prefix_dirs(const char *a, const char *b)
{
    int depth = 0;
    int i = 0;

    for (;;) {
        if (a[i] == '/' && b[i] == '/')
            depth++;
        else if (a[i] != b[i] || a[i] == '\0')
            break;
        i++;
    }
    return depth;
}

static void indent(FILE *f, int depth)
{
    int i;

    for (i = 0; i < depth; i++)
        fputc('\t', f);
}

/* Write the nth path component into out. */
static void component(const char *path, int n, char *out, size_t outsz)
{
    const char *p = path;
    int at = 0;
    size_t o = 0;

    while (*p && at < n) {
        if (*p == '/')
            at++;
        p++;
    }
    while (*p && *p != '/' && o + 1 < outsz)
        out[o++] = *p++;
    out[o] = '\0';
}

static int depth_of(const char *path)
{
    int d = 0;

    for (; *path; path++)
        if (*path == '/')
            d++;
    return d;
}

int filelist_write(const share_index *ix, const char *path, const char *cid)
{
    FILE *f;
    unsigned int i;
    int written = 0;
    int open_depth = 0;
    char prev[256];

    f = fopen(path, "w");
    if (!f)
        return -1;

    prev[0] = '\0';

    fprintf(f, "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\r\n");
    fprintf(f, "<FileListing Version=\"1\" CID=\"%s\" Base=\"/\" "
               "Generator=\"DOSy++ " DOSYPP_VERSION "\">\r\n", cid ? cid : "");

    for (i = 0; i < ix->count; i++) {
        const share_file *sf = &ix->files[i];
        const char *vp = share_virt(ix, sf);
        char esc[512];
        char b32[BASE32_TIGER_LEN + 1];
        int shared, want, d;

        /* Only hashed files appear. A peer reading this list will ask for
           files by hash, so an entry without one is an entry we could not
           serve. */
        if (!sf->hashed)
            continue;

        want = depth_of(vp);                 /* directories above the file */
        shared = common_prefix_dirs(prev, vp);
        if (shared > want)
            shared = want;

        while (open_depth > shared) {
            open_depth--;
            indent(f, open_depth + 1);
            fprintf(f, "</Directory>\r\n");
        }

        for (d = open_depth; d < want; d++) {
            char comp[128];

            component(vp, d, comp, sizeof comp);
            if (filelist_escape(esc, sizeof esc, comp) < 0)
                continue;
            indent(f, d + 1);
            fprintf(f, "<Directory Name=\"%s\">\r\n", esc);
        }
        open_depth = want;

        {
            char comp[128];

            component(vp, want, comp, sizeof comp);
            if (filelist_escape(esc, sizeof esc, comp) < 0)
                continue;
            base32_encode(b32, sizeof b32, sf->tth, TTH_HASH_LEN);
            indent(f, want + 1);
            fprintf(f, "<File Name=\"%s\" Size=\"%lu\" TTH=\"%s\"/>\r\n",
                    esc, (unsigned long)sf->size, b32);
        }

        snprintf(prev, sizeof prev, "%s", vp);
        written++;
    }

    while (open_depth > 0) {
        open_depth--;
        indent(f, open_depth + 1);
        fprintf(f, "</Directory>\r\n");
    }

    fprintf(f, "</FileListing>\r\n");

    if (fclose(f) != 0)
        return -1;
    return written;
}

int filelist_publish(const share_index *ix, const char *xml_path,
                     const char *bz2_path, const char *cid)
{
    int written = filelist_write(ix, xml_path, cid);

    if (written < 0)
        return -1;

    /* The compressed copy is the one that matters: a peer asks for
       files.xml.bz2 and will not read anything else. Writing the plain XML
       and quietly failing here would leave us advertising a share nobody
       can browse. */
    if (bz_compress_file(xml_path, bz2_path) != 0)
        return -1;

    return written;
}

/* --------------------------------------------------- partial file lists
 *
 * The flat, sorted index makes this easy in a way a tree would not: every
 * file under `base` is a contiguous run of entries whose virtual path
 * starts with it. A non-recursive list then needs one pass, emitting the
 * files whose remaining path has no slash in it and one element per
 * distinct first component of those that do.
 */

/* The next path component of `rel`, and whether anything follows it. */
static void first_component(const char *rel, char *out, size_t outsz,
                            int *has_more)
{
    size_t o = 0;

    *has_more = 0;
    while (*rel && *rel != '/') {
        if (o + 1 < outsz)
            out[o++] = *rel;
        rel++;
    }
    out[o] = '\0';
    if (*rel == '/')
        *has_more = 1;
}

int filelist_write_partial(const share_index *ix, const char *base,
                           int recursive, const char *path,
                           const char *cid)
{
    FILE *f;
    unsigned int i;
    int written = 0;
    int found_any = 0;
    size_t baselen;
    char norm[260];
    char esc_base[520];
    char last_dir[128];

    /* The base as the index spells paths: no leading slash, one trailing
       slash, so a prefix comparison is all that is needed. "/" becomes the
       empty string, which every path starts with. */
    {
        const char *b = base ? base : "/";
        size_t n;

        while (*b == '/')
            b++;
        snprintf(norm, sizeof norm, "%s", b);
        n = strlen(norm);
        while (n > 0 && norm[n - 1] == '/')
            norm[--n] = '\0';
        if (n > 0) {
            if (n + 2 >= sizeof norm)
                return -1;
            norm[n++] = '/';
            norm[n] = '\0';
        }
    }
    baselen = strlen(norm);

    /* Does the directory exist at all? A share with no files under it is
       not the same as a share that has never heard of it, and a client
       asking for a path we do not have deserves to be told so. */
    for (i = 0; i < ix->count; i++) {
        const char *vp = share_virt(ix, &ix->files[i]);

        if (baselen == 0 || strncmp(vp, norm, baselen) == 0) {
            found_any = 1;
            break;
        }
    }
    if (!found_any)
        return -1;

    f = fopen(path, "w");
    if (!f)
        return -1;

    if (filelist_escape(esc_base, sizeof esc_base,
                        base && base[0] ? base : "/") < 0) {
        fclose(f);
        return -1;
    }

    fprintf(f, "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\r\n");
    fprintf(f, "<FileListing Version=\"1\" CID=\"%s\" Base=\"%s\" "
               "Generator=\"DOSy++ " DOSYPP_VERSION "\">\r\n",
            cid ? cid : "", esc_base);

    last_dir[0] = '\0';

    if (recursive) {
        /* The whole subtree, nested, which is the full writer's job done
           over a slice of the index. The depths are relative to the base:
           a recursive list of /a/b/ starts its nesting inside b. */
        int open_depth = 0;
        char prev[260];

        prev[0] = '\0';

        for (i = 0; i < ix->count; i++) {
            const share_file *sf = &ix->files[i];
            const char *vp = share_virt(ix, sf);
            const char *rel;
            char esc[512];
            char b32[BASE32_TIGER_LEN + 1];
            int shared, want, d;

            if (!sf->hashed)
                continue;
            if (baselen && strncmp(vp, norm, baselen) != 0)
                continue;

            rel = vp + baselen;
            want = depth_of(rel);
            shared = common_prefix_dirs(prev, rel);
            if (shared > want)
                shared = want;

            while (open_depth > shared) {
                open_depth--;
                indent(f, open_depth + 1);
                fprintf(f, "</Directory>\r\n");
            }
            for (d = open_depth; d < want; d++) {
                char comp[128];

                component(rel, d, comp, sizeof comp);
                if (filelist_escape(esc, sizeof esc, comp) < 0)
                    continue;
                indent(f, d + 1);
                fprintf(f, "<Directory Name=\"%s\">\r\n", esc);
            }
            open_depth = want;

            {
                char comp[128];

                component(rel, want, comp, sizeof comp);
                if (filelist_escape(esc, sizeof esc, comp) < 0)
                    continue;
                base32_encode(b32, sizeof b32, sf->tth, TTH_HASH_LEN);
                indent(f, want + 1);
                fprintf(f, "<File Name=\"%s\" Size=\"%lu\" TTH=\"%s\"/>\r\n",
                        esc, (unsigned long)sf->size, b32);
            }

            snprintf(prev, sizeof prev, "%s", rel);
            written++;
        }

        while (open_depth > 0) {
            open_depth--;
            indent(f, open_depth + 1);
            fprintf(f, "</Directory>\r\n");
        }
    } else {
        /* One level. Files first would be tidier to read, but the index is
           in path order and streaming it twice means reading it twice; a
           client sorts the entries anyway. */
        for (i = 0; i < ix->count; i++) {
            const share_file *sf = &ix->files[i];
            const char *vp = share_virt(ix, sf);
            const char *rel;
            char comp[128];
            char esc[512];
            int has_more;

            if (!sf->hashed)
                continue;
            if (baselen && strncmp(vp, norm, baselen) != 0)
                continue;

            rel = vp + baselen;
            first_component(rel, comp, sizeof comp, &has_more);
            if (comp[0] == '\0')
                continue;

            if (filelist_escape(esc, sizeof esc, comp) < 0)
                continue;

            if (has_more) {
                /* A subdirectory. One element per directory, not per file
                   inside it, and marked so the peer knows to ask. */
                if (strcmp(comp, last_dir) == 0)
                    continue;
                snprintf(last_dir, sizeof last_dir, "%s", comp);
                indent(f, 1);
                fprintf(f, "<Directory Name=\"%s\" Incomplete=\"1\"/>\r\n",
                        esc);
                continue;
            }

            {
                char b32[BASE32_TIGER_LEN + 1];

                base32_encode(b32, sizeof b32, sf->tth, TTH_HASH_LEN);
                indent(f, 1);
                fprintf(f, "<File Name=\"%s\" Size=\"%lu\" TTH=\"%s\"/>\r\n",
                        esc, (unsigned long)sf->size, b32);
            }
            written++;
        }
    }

    fprintf(f, "</FileListing>\r\n");

    if (fclose(f) != 0)
        return -1;
    return written;
}

int filelist_publish_partial(const share_index *ix, const char *base,
                             int recursive, const char *xml_path,
                             const char *bz2_path, const char *cid)
{
    int written = filelist_write_partial(ix, base, recursive, xml_path, cid);

    if (written < 0)
        return -1;

    if (bz_compress_file(xml_path, bz2_path) != 0)
        return -1;

    return written;
}
