/* Cocos2d-x JNI entry points and call signatures.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __COCOS_ENTRYPOINTS_H__
#define __COCOS_ENTRYPOINTS_H__

#include <stdint.h>

#define SYM_JNI_OnLoad          "JNI_OnLoad"
#define SYM_nativeInit          "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit"
#define SYM_nativeRender        "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender"
#define SYM_nativeSurfaceChanged "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnSurfaceChanged"
#define SYM_nativeOnPause       "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause"
#define SYM_nativeOnResume      "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume"
#define SYM_nativeTouchesBegin  "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin"
#define SYM_nativeTouchesEnd    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd"
#define SYM_nativeTouchesMove   "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove"
#define SYM_nativeTouchesCancel "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesCancel"
#define SYM_nativeKeyDown       "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown"
#define SYM_nativeSetApkPath    "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath"
#define SYM_nativeSetContext    "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetContext"
typedef int  (*fn_jnionload)(void *vm, void *reserved);
typedef void (*fn_cocos_init)(void *env, void *thiz, int w, int h);
typedef void (*fn_cocos_render)(void *env, void *thiz);
typedef void (*fn_cocos_surfchg)(void *env, void *thiz, int w, int h);
typedef void (*fn_cocos_lifecycle)(void *env, void *thiz);
typedef void (*fn_cocos_touch1)(void *env, void *thiz, int id, float x, float y);
typedef void (*fn_cocos_touchN)(void *env, void *thiz, void *ids, void *xs, void *ys);
typedef uint8_t (*fn_cocos_key)(void *env, void *thiz, int keycode);
typedef void (*fn_cocos_setapk)(void *env, void *thiz, void *japkPath);
typedef void (*fn_cocos_setctx)(void *env, void *thiz, void *context, void *assetManager);

#endif
