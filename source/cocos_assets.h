/* SD-backed Android asset API. */

#ifndef __COCOS_ASSETS_H__
#define __COCOS_ASSETS_H__

#include <stdint.h>
#include <stddef.h>

void cocos_assets_init(const char *data_root);

void   *AAssetManager_fromJava(void *env, void *assetManager);
void   *AAssetManager_open(void *mgr, const char *filename, int mode);
int     AAsset_read(void *asset, void *buf, size_t count);
long    AAsset_seek(void *asset, long off, int whence);
int64_t AAsset_seek64(void *asset, int64_t off, int whence);
int64_t AAsset_getLength(void *asset);
int64_t AAsset_getLength64(void *asset);
int64_t AAsset_getRemainingLength64(void *asset);
int     AAsset_openFileDescriptor(void *asset, long *outStart, long *outLength);
int     AAsset_openFileDescriptor64(void *asset, int64_t *outStart, int64_t *outLength);
void    AAsset_close(void *asset);

void       *AAssetManager_openDir(void *mgr, const char *dirName);
const char *AAssetDir_getNextFileName(void *assetDir);
void        AAssetDir_rewind(void *assetDir);
void        AAssetDir_close(void *assetDir);

#endif
