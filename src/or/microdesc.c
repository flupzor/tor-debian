/* Copyright (c) 2009-2010, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#include "or.h"
#include "config.h"
#include "microdesc.h"
#include "routerparse.h"

/** A data structure to hold a bunch of cached microdescriptors.  There are
 * two active files in the cache: a "cache file" that we mmap, and a "journal
 * file" that we append to.  Periodically, we rebuild the cache file to hold
 * only the microdescriptors that we want to keep */
struct microdesc_cache_t {
  /** Map from sha256-digest to microdesc_t for every microdesc_t in the
   * cache. */
  HT_HEAD(microdesc_map, microdesc_t) map;

  /** Name of the cache file. */
  char *cache_fname;
  /** Name of the journal file. */
  char *journal_fname;
  /** Mmap'd contents of the cache file, or NULL if there is none. */
  tor_mmap_t *cache_content;
  /** Number of bytes used in the journal file. */
  size_t journal_len;

  /** Total bytes of microdescriptor bodies we have added to this cache */
  uint64_t total_len_seen;
  /** Total number of microdescriptors we have added to this cache */
  unsigned n_seen;
};

/** Helper: computes a hash of <b>md</b> to place it in a hash table. */
static INLINE unsigned int
_microdesc_hash(microdesc_t *md)
{
  unsigned *d = (unsigned*)md->digest;
#if SIZEOF_INT == 4
  return d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[6] ^ d[7];
#else
  return d[0] ^ d[1] ^ d[2] ^ d[3];
#endif
}

/** Helper: compares <b>a</b> and </b> for equality for hash-table purposes. */
static INLINE int
_microdesc_eq(microdesc_t *a, microdesc_t *b)
{
  return !memcmp(a->digest, b->digest, DIGEST256_LEN);
}

HT_PROTOTYPE(microdesc_map, microdesc_t, node,
             _microdesc_hash, _microdesc_eq);
HT_GENERATE(microdesc_map, microdesc_t, node,
             _microdesc_hash, _microdesc_eq, 0.6,
             malloc, realloc, free);

/** Write the body of <b>md</b> into <b>f</b>, with appropriate annotations.
 * On success, return the total number of bytes written, and set
 * *<b>annotation_len_out</b> to the number of bytes written as
 * annotations. */
static ssize_t
dump_microdescriptor(FILE *f, microdesc_t *md, size_t *annotation_len_out)
{
  ssize_t r = 0;
  size_t written;
  /* XXXX drops unkown annotations. */
  if (md->last_listed) {
    char buf[ISO_TIME_LEN+1];
    char annotation[ISO_TIME_LEN+32];
    format_iso_time(buf, md->last_listed);
    tor_snprintf(annotation, sizeof(annotation), "@last-listed %s\n", buf);
    fputs(annotation, f);
    r += strlen(annotation);
    *annotation_len_out = r;
  } else {
    *annotation_len_out = 0;
  }

  md->off = (off_t) ftell(f);
  written = fwrite(md->body, 1, md->bodylen, f);
  if (written != md->bodylen) {
    log_warn(LD_DIR,
             "Couldn't dump microdescriptor (wrote %lu out of %lu): %s",
             (unsigned long)written, (unsigned long)md->bodylen,
             strerror(ferror(f)));
    return -1;
  }
  r += md->bodylen;
  return r;
}

/** Holds a pointer to the current microdesc_cache_t object, or NULL if no
 * such object has been allocated. */
static microdesc_cache_t *the_microdesc_cache = NULL;

/** Return a pointer to the microdescriptor cache, loading it if necessary. */
microdesc_cache_t *
get_microdesc_cache(void)
{
  if (PREDICT_UNLIKELY(the_microdesc_cache==NULL)) {
    microdesc_cache_t *cache = tor_malloc_zero(sizeof(microdesc_cache_t));
    HT_INIT(microdesc_map, &cache->map);
    cache->cache_fname = get_datadir_fname("cached-microdescs");
    cache->journal_fname = get_datadir_fname("cached-microdescs.new");
    microdesc_cache_reload(cache);
    the_microdesc_cache = cache;
  }
  return the_microdesc_cache;
}

/* There are three sources of microdescriptors:
   1) Generated by us while acting as a directory authority.
   2) Loaded from the cache on disk.
   3) Downloaded.
*/

/** Decode the microdescriptors from the string starting at <b>s</b> and
 * ending at <b>eos</b>, and store them in <b>cache</b>.  If <b>no-save</b>,
 * mark them as non-writable to disk.  If <b>where</b> is SAVED_IN_CACHE,
 * leave their bodies as pointers to the mmap'd cache.  If where is
 * <b>SAVED_NOWHERE</b>, do not allow annotations.  Return a list of the added
 * microdescriptors.  */
smartlist_t *
microdescs_add_to_cache(microdesc_cache_t *cache,
                        const char *s, const char *eos, saved_location_t where,
                        int no_save)
{
  /*XXXX need an argument that sets last_listed as appropriate. */

  smartlist_t *descriptors, *added;
  const int allow_annotations = (where != SAVED_NOWHERE);
  const int copy_body = (where != SAVED_IN_CACHE);

  descriptors = microdescs_parse_from_string(s, eos,
                                             allow_annotations,
                                             copy_body);

  added = microdescs_add_list_to_cache(cache, descriptors, where, no_save);
  smartlist_free(descriptors);
  return added;
}

/* As microdescs_add_to_cache, but takes a list of micrdescriptors instead of
 * a string to encode.  Frees any members of <b>descriptors</b> that it does
 * not add. */
smartlist_t *
microdescs_add_list_to_cache(microdesc_cache_t *cache,
                             smartlist_t *descriptors, saved_location_t where,
                             int no_save)
{
  smartlist_t *added;
  open_file_t *open_file = NULL;
  FILE *f = NULL;
  //  int n_added = 0;
  ssize_t size = 0;

  if (where == SAVED_NOWHERE && !no_save) {
    f = start_writing_to_stdio_file(cache->journal_fname,
                                    OPEN_FLAGS_APPEND|O_BINARY,
                                    0600, &open_file);
    if (!f) {
      log_warn(LD_DIR, "Couldn't append to journal in %s: %s",
               cache->journal_fname, strerror(errno));
      return NULL;
    }
  }

  added = smartlist_create();
  SMARTLIST_FOREACH_BEGIN(descriptors, microdesc_t *, md) {
    microdesc_t *md2;
    md2 = HT_FIND(microdesc_map, &cache->map, md);
    if (md2) {
      /* We already had this one. */
      if (md2->last_listed < md->last_listed)
        md2->last_listed = md->last_listed;
      microdesc_free(md);
      continue;
    }

    /* Okay, it's a new one. */
    if (f) {
      size_t annotation_len;
      size = dump_microdescriptor(f, md, &annotation_len);
      if (size < 0) {
        /* XXX handle errors from dump_microdescriptor() */
        /* log?  return -1?  die?  coredump the universe? */
        continue;
      }
      md->saved_location = SAVED_IN_JOURNAL;
      cache->journal_len += size;
    } else {
      md->saved_location = where;
    }

    md->no_save = no_save;

    HT_INSERT(microdesc_map, &cache->map, md);
    smartlist_add(added, md);
    ++cache->n_seen;
    cache->total_len_seen += md->bodylen;
  } SMARTLIST_FOREACH_END(md);

  if (f)
    finish_writing_to_file(open_file); /*XXX Check me.*/

  {
    size_t old_content_len =
      cache->cache_content ? cache->cache_content->size : 0;
    if (cache->journal_len > 16384 + old_content_len &&
        cache->journal_len > old_content_len * 2) {
      microdesc_cache_rebuild(cache);
    }
  }

  return added;
}

/** Remove every microdescriptor in <b>cache</b>. */
void
microdesc_cache_clear(microdesc_cache_t *cache)
{
  microdesc_t **entry, **next;
  for (entry = HT_START(microdesc_map, &cache->map); entry; entry = next) {
    microdesc_t *md = *entry;
    next = HT_NEXT_RMV(microdesc_map, &cache->map, entry);
    microdesc_free(md);
  }
  HT_CLEAR(microdesc_map, &cache->map);
  if (cache->cache_content) {
    tor_munmap_file(cache->cache_content);
    cache->cache_content = NULL;
  }
  cache->total_len_seen = 0;
  cache->n_seen = 0;
}

/** Reload the contents of <b>cache</b> from disk.  If it is empty, load it
 * for the first time.  Return 0 on success, -1 on failure. */
int
microdesc_cache_reload(microdesc_cache_t *cache)
{
  struct stat st;
  char *journal_content;
  smartlist_t *added;
  tor_mmap_t *mm;
  int total = 0;

  microdesc_cache_clear(cache);

  mm = cache->cache_content = tor_mmap_file(cache->cache_fname);
  if (mm) {
    added = microdescs_add_to_cache(cache, mm->data, mm->data+mm->size,
                                    SAVED_IN_CACHE, 0);
    if (added) {
      total += smartlist_len(added);
      smartlist_free(added);
    }
  }

  journal_content = read_file_to_str(cache->journal_fname,
                                     RFTS_IGNORE_MISSING, &st);
  if (journal_content) {
    added = microdescs_add_to_cache(cache, journal_content,
                                    journal_content+st.st_size,
                                    SAVED_IN_JOURNAL, 0);
    if (added) {
      total += smartlist_len(added);
      smartlist_free(added);
    }
    tor_free(journal_content);
  }
  log_notice(LD_DIR, "Reloaded microdescriptor cache.  Found %d descriptors.",
             total);
  return 0;
}

/** Regenerate the main cache file for <b>cache</b>, clear the journal file,
 * and update every microdesc_t in the cache with pointers to its new
 * location. */
int
microdesc_cache_rebuild(microdesc_cache_t *cache)
{
  open_file_t *open_file;
  FILE *f;
  microdesc_t **mdp;
  smartlist_t *wrote;
  ssize_t size;
  off_t off = 0;
  int orig_size, new_size;

  log_info(LD_DIR, "Rebuilding the microdescriptor cache...");
  orig_size = (int)(cache->cache_content ? cache->cache_content->size : 0);
  orig_size += (int)cache->journal_len;

  f = start_writing_to_stdio_file(cache->cache_fname,
                                  OPEN_FLAGS_REPLACE|O_BINARY,
                                  0600, &open_file);
  if (!f)
    return -1;

  wrote = smartlist_create();

  HT_FOREACH(mdp, microdesc_map, &cache->map) {
    microdesc_t *md = *mdp;
    size_t annotation_len;
    if (md->no_save)
      continue;

    size = dump_microdescriptor(f, md, &annotation_len);
    if (size < 0) {
      /* XXX handle errors from dump_microdescriptor() */
      /* log?  return -1?  die?  coredump the universe? */
      continue;
    }
    md->off = off + annotation_len;
    off += size;
    if (md->saved_location != SAVED_IN_CACHE) {
      tor_free(md->body);
      md->saved_location = SAVED_IN_CACHE;
    }
    smartlist_add(wrote, md);
  }

  finish_writing_to_file(open_file); /*XXX Check me.*/

  if (cache->cache_content)
    tor_munmap_file(cache->cache_content);
  cache->cache_content = tor_mmap_file(cache->cache_fname);

  if (!cache->cache_content && smartlist_len(wrote)) {
    log_err(LD_DIR, "Couldn't map file that we just wrote to %s!",
            cache->cache_fname);
    smartlist_free(wrote);
    return -1;
  }
  SMARTLIST_FOREACH_BEGIN(wrote, microdesc_t *, md) {
    tor_assert(md->saved_location == SAVED_IN_CACHE);
    md->body = (char*)cache->cache_content->data + md->off;
    tor_assert(!memcmp(md->body, "onion-key", 9));
  } SMARTLIST_FOREACH_END(md);

  smartlist_free(wrote);

  write_str_to_file(cache->journal_fname, "", 1);
  cache->journal_len = 0;

  new_size = (int)cache->cache_content->size;
  log_info(LD_DIR, "Done rebuilding microdesc cache. "
           "Saved %d bytes; %d still used.",
           orig_size-new_size, new_size);

  return 0;
}

/** Deallocate a single microdescriptor.  Note: the microdescriptor MUST have
 * previously been removed from the cache if it had ever been inserted. */
void
microdesc_free(microdesc_t *md)
{
  if (!md)
    return;
  /* Must be removed from hash table! */
  if (md->onion_pkey)
    crypto_free_pk_env(md->onion_pkey);
  if (md->body && md->saved_location != SAVED_IN_CACHE)
    tor_free(md->body);

  if (md->family) {
    SMARTLIST_FOREACH(md->family, char *, cp, tor_free(cp));
    smartlist_free(md->family);
  }
  tor_free(md->exitsummary);

  tor_free(md);
}

/** Free all storage held in the microdesc.c module. */
void
microdesc_free_all(void)
{
  if (the_microdesc_cache) {
    microdesc_cache_clear(the_microdesc_cache);
    tor_free(the_microdesc_cache->cache_fname);
    tor_free(the_microdesc_cache->journal_fname);
    tor_free(the_microdesc_cache);
  }
}

/** If there is a microdescriptor in <b>cache</b> whose sha256 digest is
 * <b>d</b>, return it.  Otherwise return NULL. */
microdesc_t *
microdesc_cache_lookup_by_digest256(microdesc_cache_t *cache, const char *d)
{
  microdesc_t *md, search;
  if (!cache)
    cache = get_microdesc_cache();
  memcpy(search.digest, d, DIGEST256_LEN);
  md = HT_FIND(microdesc_map, &cache->map, &search);
  return md;
}

/** Return the mean size of decriptors added to <b>cache</b> since it was last
 * cleared.  Used to estimate the size of large downloads. */
size_t
microdesc_average_size(microdesc_cache_t *cache)
{
  if (!cache)
    cache = get_microdesc_cache();
  if (!cache->n_seen)
    return 512;
  return (size_t)(cache->total_len_seen / cache->n_seen);
}

