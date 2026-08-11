/* SD-backed Android asset API.
 *
 * Drop-in replacement for the KHUx version. Changes for Happy Wheels:
 *   - NxAsset retains the resolved path, so AAsset_openFileDescriptor can
 *     hand back a real fd (the cocos AudioEngine OpenSL path needs this).
 *   - AAssetManager_openDir / AAssetDir_getNextFileName / AAssetDir_close,
 *     used by FileUtilsAndroid when it enumerates a search-path directory.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"
#include "cocos_assets.h"
#include "util.h"

static char g_root[HW_PATH_MAX] = ".";
static int g_mgr_sentinel;

void cocos_assets_init(const char *data_root) {
  if (data_root && *data_root) {
    strncpy(g_root, data_root, sizeof(g_root) - 1);
    g_root[sizeof(g_root) - 1] = 0;
  }
}

typedef struct {
  FILE *fp;
  int64_t size;
  char path[HW_PATH_MAX];   /* resolved, for openFileDescriptor */
} NxAsset;

typedef struct {
  DIR *dir;
  char base[HW_PATH_MAX];
  char name[256];   /* returned pointer stays valid until the next call */
} NxAssetDir;

/* Resolve an asset-relative name to a real path under the data root.
 * Mirrors the lookup order of the original open_under_root(). */
static int resolve_path(const char *rel, char *out, size_t outsz) {
  struct stat st;

  if (rel[0] == '/' || strstr(rel, ":/")) {
    if (stat(rel, &st) == 0 && !S_ISDIR(st.st_mode)) {
      snprintf(out, outsz, "%s", rel);
      return 1;
    }
  }

  const char *f = rel;
  if (!strncmp(f, "assets/", 7)) f += 7;

  snprintf(out, outsz, "%s/assets/%s", g_root, f);
  if (stat(out, &st) == 0 && !S_ISDIR(st.st_mode)) return 1;

  snprintf(out, outsz, "%s/%s", g_root, rel);
  if (stat(out, &st) == 0 && !S_ISDIR(st.st_mode)) return 1;

  return 0;
}

void *AAssetManager_fromJava(void *env, void *assetManager) {
  (void)env; (void)assetManager;
  return &g_mgr_sentinel;
}

void *AAssetManager_open(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  if (!filename) return NULL;

  char path[HW_PATH_MAX];
  if (!resolve_path(filename, path, sizeof path)) {
    /* Not an error on its own: cocos probes every search path in turn, so a
     * run of these followed by an "open" line is the normal success pattern.
     * Only a probe-miss with no following open is a genuinely missing file. */
    debugPrintf("[asset] probe-miss %s\n", filename);
    return NULL;
  }

  FILE *fp = fopen(path, "rb");
  if (!fp) {
    debugPrintf("[asset] probe-miss %s\n", filename);
    return NULL;
  }
  debugPrintf("[asset] open %s\n", path);

  NxAsset *a = calloc(1, sizeof *a);
  if (!a) { fclose(fp); return NULL; }

  a->fp = fp;
  snprintf(a->path, sizeof a->path, "%s", path);
  fseek(fp, 0, SEEK_END);
  a->size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  return a;
}

int AAsset_read(void *asset, void *buf, size_t count) {
  NxAsset *a = asset;
  if (!a || !a->fp) return -1;
  return (int)fread(buf, 1, count, a->fp);
}

long AAsset_seek(void *asset, long off, int whence) {
  NxAsset *a = asset;
  if (!a || !a->fp) return -1;
  if (fseek(a->fp, off, whence) != 0) return -1;
  return ftell(a->fp);
}

int64_t AAsset_seek64(void *asset, int64_t off, int whence) {
  return (int64_t)AAsset_seek(asset, (long)off, whence);
}

int64_t AAsset_getLength(void *asset)   { NxAsset *a = asset; return a ? a->size : 0; }
int64_t AAsset_getLength64(void *asset) { NxAsset *a = asset; return a ? a->size : 0; }

int64_t AAsset_getRemainingLength64(void *asset) {
  NxAsset *a = asset;
  if (!a || !a->fp) return 0;
  long cur = ftell(a->fp);
  return (cur < 0) ? 0 : (a->size - cur);
}

/* On Android this returns an fd into the APK with a nonzero start offset.
 * Assets here are loose files on the SD card, so start is always 0 and the
 * length is the whole file. Callers (cocos AudioEngine) handle both shapes. */
int AAsset_openFileDescriptor(void *asset, long *outStart, long *outLength) {
  NxAsset *a = asset;
  if (!a || !a->path[0]) return -1;

  int fd = open(a->path, O_RDONLY);
  if (fd < 0) {
    /* Almost always descriptor exhaustion rather than a missing file: the
     * caller is expected to close what it gets from here. */
    debugLogNote("[asset] openFileDescriptor FAILED for %s (out of fds?)\n", a->path);
    return -1;
  }

  if (outStart)  *outStart  = 0;
  if (outLength) *outLength = (long)a->size;
  return fd;
}

int AAsset_openFileDescriptor64(void *asset, int64_t *outStart, int64_t *outLength) {
  long s = 0, l = 0;
  int fd = AAsset_openFileDescriptor(asset, &s, &l);
  if (fd < 0) return -1;
  if (outStart)  *outStart  = s;
  if (outLength) *outLength = l;
  return fd;
}

void AAsset_close(void *asset) {
  NxAsset *a = asset;
  if (!a) return;
  if (a->fp) fclose(a->fp);
  free(a);
}

/* ---- directory enumeration ---- */

void *AAssetManager_openDir(void *mgr, const char *dirName) {
  (void)mgr;
  if (!dirName) dirName = "";

  char path[HW_PATH_MAX];
  const char *d = dirName;
  if (!strncmp(d, "assets/", 7)) d += 7;

  if (*d)
    snprintf(path, sizeof path, "%s/assets/%s", g_root, d);
  else
    snprintf(path, sizeof path, "%s/assets", g_root);

  DIR *dir = opendir(path);
  if (!dir) {
    snprintf(path, sizeof path, "%s/%s", g_root, dirName);
    dir = opendir(path);
    if (!dir) return NULL;
  }

  NxAssetDir *ad = calloc(1, sizeof *ad);
  if (!ad) { closedir(dir); return NULL; }
  ad->dir = dir;
  snprintf(ad->base, sizeof ad->base, "%s", path);
  return ad;
}

/* Android returns file names only — subdirectories are skipped. */
const char *AAssetDir_getNextFileName(void *assetDir) {
  NxAssetDir *ad = assetDir;
  if (!ad || !ad->dir) return NULL;

  struct dirent *de;
  while ((de = readdir(ad->dir)) != NULL) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

    char full[HW_PATH_MAX];
    struct stat st;
    snprintf(full, sizeof full, "%s/%s", ad->base, de->d_name);
    if (stat(full, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) continue;

    snprintf(ad->name, sizeof ad->name, "%s", de->d_name);
    return ad->name;
  }
  return NULL;
}

void AAssetDir_rewind(void *assetDir) {
  NxAssetDir *ad = assetDir;
  if (!ad) return;
  /* Reopen rather than rewinddir(): the game never calls this, and not
   * depending on rewinddir keeps one more libc corner out of the build. */
  if (ad->dir) closedir(ad->dir);
  ad->dir = opendir(ad->base);
}

void AAssetDir_close(void *assetDir) {
  NxAssetDir *ad = assetDir;
  if (!ad) return;
  if (ad->dir) closedir(ad->dir);
  free(ad);
}
