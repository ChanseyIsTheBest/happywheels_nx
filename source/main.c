/* Happy Wheels Switch wrapper entry point. */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "libc_shim.h"
#include "jni_fake.h"
#include "android_native_cocos.h"
#include "opensles.h"
#include "cocos_entrypoints.h"
#include "cocos_assets.h"
#include "cocos_jni.h"
#include "cocos_video.h"
#include "hw_input.h"
#include "watchdog.h"
#include "cxx_diag.h"
#include "nx_pointer.h"

void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;

/* Captured in __libnx_initheap, which runs before the logger exists. */
static size_t g_heap_total, g_heap_newlib, g_heap_so, g_heap_arena;

so_module cocos_mod;

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* Reserved for the loaded module, and taken straight out of the malloc heap.
 * libMyGame.so maps 9.8 MB of text, data and bss (readelf -lW, summed across
 * its three LOAD segments) and it is the only object ever loaded, so 48 MB was
 * leaving 38 MB permanently dead. 16 MB keeps a comfortable margin over the
 * real figure and hands the rest back to malloc, which is where the pressure
 * actually is. */
#define SO_REGION_BYTES (16u * 1024 * 1024)

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  const size_t big_align = MMAP_ARENA_ALIGN;
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + arena_sz + 256 * MB) {
    fake_heap_size = size - so_zone - arena_sz - big_align;
  } else {
    arena_sz = (size > so_zone + 512 * MB) ? 128 * MB : 0;
    fake_heap_size = size - so_zone - arena_sz;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }

  g_heap_total  = size;
  g_heap_newlib = fake_heap_size;
  g_heap_so     = so_zone;
  g_heap_arena  = arena_sz;
}

/* CNTVCT_EL0 is not readable from userspace here; rewrite the reads to
 * CNTPCT_EL0. Cocos reads it for its frame timer. */
static int nx_patch_cntvct(so_module *mod) {
  uintptr_t vb = (uintptr_t)mod->load_virtbase;
  int patched = 0;
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type != PT_LOAD || !(mod->phdr[i].p_flags & PF_X)) continue;
    uintptr_t s = vb + mod->phdr[i].p_vaddr;
    uintptr_t e = s + mod->phdr[i].p_memsz;
    for (uintptr_t a = s; a + 4 <= e; a += 4) {
      uint32_t w = *(volatile uint32_t *)a;
      if ((w & 0xffffffe0u) == 0xd53be040u) {
        uint32_t nw = (w & 0x1fu) | 0xd53be020u;
        so_patch_code((void *)a, &nw, sizeof nw);
        patched++;
      }
    }
  }
  return patched;
}

/* Report the remove-ads entitlement as held.
 *
 * AdController::getAdsRemoved() and IAPController::getAdsRemoved() are each a
 * single field read; both become "return true".
 *
 * This is a feature flag, not a check on the game. Google Play Billing does
 * not exist on this platform, so the game is really asking our own stub
 * whether the product is owned -- "no" and "yes" are both answers this port
 * invents, and neither is more truthful than the other. No purchase can be
 * made here and no revenue can flow either way. The ad plugins are already
 * inert, so what the entitlement buys -- not seeing ads -- is the state the
 * port is in regardless; the only thing the flag still controls is a wait
 * timer for an ad that cannot arrive.
 *
 * Set remove_ads to 0 in config.h and rebuild for the stock behaviour. */
/* Rewrite a bool-returning getter to "return true". */
static int patch_return_true(so_module *mod, const char *sym, const char *tag) {
  static const uint32_t ret_true[2] = {
    0x52800020u,   /* mov w0, #1 */
    0xd65f03c0u,   /* ret        */
  };
  uintptr_t addr = so_try_find_addr_rx(mod, sym);
  if (!addr) {
    debugLogNote("[%s] symbol not found, skipping: %s\n", tag, sym);
    return 0;
  }
  so_patch_code((void *)addr, ret_true, sizeof ret_true);
  return 1;
}

/* Overwrite instructions at a known offset inside a resolved symbol. */
static int patch_words_at(so_module *mod, const char *sym, unsigned off,
                          const uint32_t *words, size_t n, const char *tag) {
  uintptr_t addr = so_try_find_addr_rx(mod, sym);
  if (!addr) {
    debugLogNote("[%s] symbol not found, skipping: %s\n", tag, sym);
    return 0;
  }
  so_patch_code((void *)(addr + off), words, n * sizeof(uint32_t));
  return 1;
}

/* Report the remove-ads entitlement as held.
 *
 * Patching getAdsRemoved() alone was not enough, and the failure looked like a
 * hang rather than an ad: AdController::showAd reads the adsRemoved *field*
 * directly (ldrb w8, [this, #1]) instead of calling its own getter, as do the
 * other internal users. Of the seven showAd call sites only one consults the
 * getter first, so dying, pausing or restarting a level still queued an
 * interstitial that can never load here -- and the game sits on a blank screen
 * waiting for it.
 *
 * So the field itself has to be true. AdController::init() computes it from
 * UserDefault's "remove_ads" and stores it, then may clear it again a few
 * instructions later; both are neutralised, and setAdsRemoved() is pinned so
 * nothing can turn it back off afterwards. */
static int patch_ads_removed(so_module *mod) {
  int n = 0;

  /* The getters, for the call sites that do use them. */
  n += patch_return_true(mod, "_ZN12AdController13getAdsRemovedEv", "ads");
  n += patch_return_true(mod, "_ZN13IAPController13getAdsRemovedEv", "ads");

  /* init(): "and w10, w0, #1" -> "mov w10, #1", so the stored field ignores
   * the preference, and the later "strb wzr, [x19, #1]" becomes a nop. */
  static const uint32_t mov_w10_1[1] = { 0x5280002Au };
  static const uint32_t nop_one[1]   = { 0xD503201Fu };
  n += patch_words_at(mod, "_ZN12AdController4initEv", 0x28, mov_w10_1, 1, "ads");
  n += patch_words_at(mod, "_ZN12AdController4initEv", 0x40, nop_one,   1, "ads");

  /* showAd(AdType): the decisive one. Its callers ignore the return value, so
   * the wait it produces is set up inside the function -- it asks the Java ad
   * helper for an interstitial and the game then sits until
   * Gameplay::interstitialDidEnd arrives. That can only be reached from
   * Java_com_fancyforce_AdHelper_jniInterstitialDidEnd, which nothing here
   * ever calls, so the game waits for ever on a blank screen.
   *
   * Six of the seven call sites -- handleDead, pauseGameplay,
   * handleLevelComplete, beginGameplayFollowingInterstitial, Mascot's window
   * button, and hwWindowWasDismissed -- do not check adsRemoved first, so
   * pinning the field is not sufficient on its own. The field is also never
   * set by the constructor and only ever written by init(), which runs from
   * PrivacyPolicyScene rather than at startup.
   *
   * Returning false here is precisely what the ads-removed path inside showAd
   * does: it sets w0 to zero and returns without touching anything else. */
  static const uint32_t ret_false[2] = {
    0x52800000u,   /* mov w0, #0 */
    0xd65f03c0u,   /* ret        */
  };
  n += patch_words_at(mod, "_ZN12AdController6showAdE6AdType", 0x0, ret_false, 2, "ads");

  /* getBannerAdSize(): returns a zero size when adsRemoved is set, and the
   * game shifts its upper UI down to make room whenever it is not -- the pause
   * button moves 96px, so a fixed control position would be wrong half the
   * time. It reads the field directly, and the field is only ever written by
   * init(), which runs from PrivacyPolicyScene rather than at startup, so
   * relying on it is not safe. Forcing the test true costs one instruction. */
  static const uint32_t mov_w9_1[1] = { 0x52800029u };   /* mov w9, #1 */
  n += patch_words_at(mod, "_ZN12AdController15getBannerAdSizeEv", 0xC, mov_w9_1, 1, "ads");

  /* setAdsRemoved(bool): force the argument to true before it is stored, so a
   * later call cannot clear it. The tail call at +0x08 is left in place. */
  static const uint32_t force_true[2] = {
    0x52800021u,   /* mov  w1, #1        */
    0x39000401u,   /* strb w1, [x0, #1]  */
  };
  n += patch_words_at(mod, "_ZN12AdController13setAdsRemovedEb", 0x0, force_true, 2, "ads");

  return n;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(const char *data_root) {
  char path[768];
  struct stat st;

  snprintf(path, sizeof path, "%s/%s", data_root, SO_COCOS);
  if (stat(path, &st) < 0)
    fatal_error("Missing %s in %s\n\n"
                "%s comes from the arm64 native library split of your own\n"
                "copy of the game -- usually config.arm64_v8a.apk inside the\n"
                "XAPK, not the base APK. See README.md.",
                SO_COCOS, data_root, SO_COCOS);

  snprintf(path, sizeof path, "%s/assets", data_root);
  if (stat(path, &st) < 0)
    fatal_error("Missing assets directory in %s\n\n"
                "Copy the whole assets/ folder out of the base APK.", data_root);

  /* The bucket the game will actually read from. Failing here is far kinder
   * than a black screen forty seconds into loading. */
  snprintf(path, sizeof path, "%s/assets/shared", data_root);
  if (stat(path, &st) < 0)
    fatal_error("assets/shared is missing.\n\n"
                "The assets folder looks incomplete -- it should contain\n"
                "shared/, large/, medium/, small/, tiny/ and sounds/.");
}

/* resolved Cocos2d-x native entry points */
static fn_cocos_init       Cocos_nativeInit;
static fn_cocos_render     Cocos_nativeRender;
static fn_cocos_surfchg    Cocos_nativeSurfaceChanged;
static fn_cocos_lifecycle  Cocos_nativeOnResume, Cocos_nativeOnPause;
static fn_cocos_touch1     Cocos_touchesBegin, Cocos_touchesEnd;
static fn_cocos_touchN     Cocos_touchesMove, Cocos_touchesCancel;
static fn_cocos_key        Cocos_nativeKeyDown;
static fn_cocos_setapk     Cocos_nativeSetApkPath;
static fn_cocos_setctx     Cocos_nativeSetContext;

int main(int argc, char *argv[]) {
  if (hw_init_data_root(argc > 0 ? argv[0] : NULL) != 0)
    fatal_error("Could not determine the game folder from the NRO path.");
  const char *data_root = hw_data_root();

  if (chdir(data_root) != 0)
    fatal_error("Could not open %s", data_root);

  debugLogNote("=== Happy Wheels NX starting ===\n");
  debugLogNote("heap: total %zu MB = malloc %zu MB + so %zu MB + mmap arena %zu MB\n",
               g_heap_total / (1024 * 1024), g_heap_newlib / (1024 * 1024),
               g_heap_so / (1024 * 1024), g_heap_arena / (1024 * 1024));
  debugLogNote("data root: %s\n", data_root);

  debugLogNote("config: handheld=%dp docked=%dp dpi=%d remove_ads=%d\n",
               config.handheld_res, config.docked_res, config.dpi, config.remove_ads);

  check_syscalls();
  cocos_native_update_mode();
  screen_width  = (int)cocos_native_width();
  screen_height = (int)cocos_native_height();

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    fatal_error("SDL initialization failed: %s", SDL_GetError());

  check_data(data_root);

  {
    char path[768];
    snprintf(path, sizeof path, "%s/%s", data_root, SO_COCOS);
    if (so_load(&cocos_mod, path, heap_so_base, heap_so_limit) < 0)
      fatal_error("Could not load %s", SO_COCOS);
    heap_so_base  = (char *)heap_so_base + ALIGN_MEM(cocos_mod.load_size, 0x1000);
    heap_so_limit -= ALIGN_MEM(cocos_mod.load_size, 0x1000);
  }
  debugLogNote("loaded %s at %p (%u bytes)\n", SO_COCOS,
               cocos_mod.load_virtbase, (unsigned)cocos_mod.load_size);
  crx_resolve_imports(&cocos_mod);

  so_finalize(&cocos_mod);
  so_flush_caches(&cocos_mod);

  nx_patch_cntvct(&cocos_mod);
  if (config.remove_ads) {
    int n = patch_ads_removed(&cocos_mod);
    debugLogNote("[ads] ads disabled (%d/7 patch sites)\n", n);
  }
  so_flush_caches(&cocos_mod);

  cxx_diag_install(&cocos_mod);

  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  so_execute_init_array(&cocos_mod);
  so_free_temp(&cocos_mod);

  debugLogNote("init_array done\n");
  jni_init();
  cocos_jni_init(data_root);

  #define RES(sym) so_find_addr_rx(&cocos_mod, sym)
  Cocos_nativeInit           = (fn_cocos_init)      RES(SYM_nativeInit);
  Cocos_nativeRender         = (fn_cocos_render)    RES(SYM_nativeRender);
  Cocos_nativeSurfaceChanged = (fn_cocos_surfchg)   RES(SYM_nativeSurfaceChanged);
  Cocos_nativeOnResume       = (fn_cocos_lifecycle) so_try_find_addr_rx(&cocos_mod, SYM_nativeOnResume);
  Cocos_nativeOnPause        = (fn_cocos_lifecycle) so_try_find_addr_rx(&cocos_mod, SYM_nativeOnPause);
  Cocos_touchesBegin         = (fn_cocos_touch1)    RES(SYM_nativeTouchesBegin);
  Cocos_touchesEnd           = (fn_cocos_touch1)    RES(SYM_nativeTouchesEnd);
  Cocos_touchesMove          = (fn_cocos_touchN)    RES(SYM_nativeTouchesMove);
  Cocos_touchesCancel        = (fn_cocos_touchN)    RES(SYM_nativeTouchesCancel);
  Cocos_nativeKeyDown        = (fn_cocos_key)       so_try_find_addr_rx(&cocos_mod, SYM_nativeKeyDown);
  /* Happy Wheels exports no nativeSetApkPath -- it reads assets through the
   * AssetManager instead -- so this one is optional. */
  Cocos_nativeSetApkPath     = (fn_cocos_setapk)    so_try_find_addr_rx(&cocos_mod, SYM_nativeSetApkPath);
  Cocos_nativeSetContext     = (fn_cocos_setctx)    RES(SYM_nativeSetContext);
  #undef RES

  extern void *fake_env, *fake_vm;
  void *thiz     = jni_make_object("thiz");
  void *context  = jni_make_activity_object();
  void *assetmgr = jni_make_object("AssetManager");

  {
    fn_jnionload Cocos_JNI_OnLoad = (fn_jnionload)so_try_find_addr_rx(&cocos_mod, SYM_JNI_OnLoad);
    if (Cocos_JNI_OnLoad) Cocos_JNI_OnLoad(fake_vm, NULL);
  }

  /* SDKBOX caches the JavaVM in a global of its own, normally from
   * SDKBox.nativeInit() on the Java side. Nothing calls that here, so the
   * global stayed NULL and the first plugin touch -- AppDelegate calls
   * PluginReview::init(), which constructs SdkboxCore -- dereferenced it
   * inside JNIUtils::cacheEnv().
   *
   * The plugins are stubbed (see hw_sdkbox.c), but the game calls into them
   * unconditionally, so they have to initialise rather than be skipped.
   * Handing them the fake VM lets every later JNI call resolve to the inert
   * class and fail the way an absent plugin does on a device without Play
   * services. JNIUtils::initialize() only needs NewGlobalRef, GetObjectClass
   * and GetMethodID, all of which the fake environment implements. */
  {
    typedef void (*fn_sdkbox_init)(void *vm, void *env, void *a, void *b, void *c);
    fn_sdkbox_init sdkbox_init = (fn_sdkbox_init)so_try_find_addr_rx(
        &cocos_mod, "_ZN6sdkbox8JNIUtils10initializeEP7_JavaVMP7_JNIEnvP8_jobjectS6_S6_");
    if (sdkbox_init)
      sdkbox_init(fake_vm, fake_env, context, context, context);
  }

  if (Cocos_nativeSetContext) Cocos_nativeSetContext(fake_env, thiz, context, assetmgr);
  if (Cocos_nativeSetApkPath) Cocos_nativeSetApkPath(fake_env, thiz, jni_make_string(data_root));

  /* Tell cocos what the audio device is.
   *
   * getDeviceSampleRate() and getDeviceAudioBufferSizeInFrames() just read two
   * globals that only nativeSetAudioDeviceInfo() ever writes. On Android the
   * Java side calls it at startup; nothing did here, so cocos built its whole
   * audio pipeline around a sample rate of 0 and a buffer of 0 frames. That
   * would have kept audio broken even with the PCM path enabled.
   *
   * The values match what opensles.c asks SDL for, so decoded audio needs no
   * resampling on the way out. */
  {
    typedef void (*fn_setaudio)(void *env, void *cls, int low_latency,
                                int sample_rate, int buffer_frames);
    fn_setaudio set_audio = (fn_setaudio)so_try_find_addr_rx(
        &cocos_mod, "Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetAudioDeviceInfo");
    if (set_audio) {
      set_audio(fake_env, jni_make_object("Cocos2dxHelper"), 1, 48000, 1024);
      debugLogNote("[audio] device info: 48000 Hz, 1024 frames\n");
    } else {
      debugLogNote("[audio] nativeSetAudioDeviceInfo missing -- rate/buffer stay 0\n");
    }
  }

  cocos_assets_init(data_root);
  hw_input_init(data_root);

  /* Before the engine spawns threads: this reads cursor.png from the SD card,
   * and the decode plus GL upload happen later on the render thread. */
  {
    NxpConfig np = {
      .screen_w = screen_width,
      .screen_h = screen_height,
      .data_dir = data_root,
    };
    nxp_init(&np);
  }
  debugLogNote("jni + sdkbox init done, entering GL init\n");

  if (cocos_gl_init() != 0)
    fatal_error("GLES2 context creation failed");

  debugLogNote("GL ready %dx%d, calling nativeInit\n", screen_width, screen_height);
#if DEBUG_LOG
  watchdog_start();
  watchdog_mark("nativeInit");
#endif
  Cocos_nativeInit(fake_env, thiz, screen_width, screen_height);
  debugLogNote("nativeInit returned\n");
  if (Cocos_nativeOnResume) Cocos_nativeOnResume(fake_env, thiz);

  CocosInputApi api = {
    .begin = Cocos_touchesBegin, .end = Cocos_touchesEnd,
    .move = Cocos_touchesMove, .cancel = Cocos_touchesCancel,
    .key = Cocos_nativeKeyDown, .env = fake_env, .thiz = thiz,
  };
  cocos_input_init(&api);

#if DEBUG_LOG
  unsigned frame = 0;
  watchdog_mark("main loop");
#endif
  int prev_docked = (appletGetOperationMode() == AppletOperationMode_Console);
  while (appletMainLoop() && !jni_quit_requested) {
    int docked = (appletGetOperationMode() == AppletOperationMode_Console);
    if (docked != prev_docked) {
      cocos_native_update_mode();
      const int w = (int)cocos_native_width();
      const int h = (int)cocos_native_height();
      /* With handheld_res == docked_res the framebuffer is unchanged, so say
       * nothing: a surface-changed event makes cocos rebuild its GLView and
       * re-lay-out the UI, which would move the on-screen controls that
       * controls.cfg was calibrated against. */
      if (w != screen_width || h != screen_height) {
        screen_width  = w;
        screen_height = h;
        if (Cocos_nativeSurfaceChanged)
          Cocos_nativeSurfaceChanged(fake_env, thiz, screen_width, screen_height);
        debugLogNote("[gl] surface changed to %dx%d\n", w, h);
      }
      prev_docked = docked;
    }

    cocos_feed_hid();
    Cocos_nativeRender(fake_env, thiz);
    cocos_gl_swap();

#if DEBUG_LOG
    /* Once, a few frames in: says whether the cursor overlay built its shader.
     * Without it, "no cursor" could equally be the toggle or the drawing. */
    if (frame == 120)
      debugLogNote("[nxp] cursor visible=%d gl=%d (1 ok, 0 not drawn yet, -1 failed)\n",
                   nxp_cursor_visible(), nxp_gl_state());
#endif

    /* Flush every frame. The buffer write is skipped when empty, so this
     * costs a mutex; without it a hang loses up to 8 KB of the tail -- which
     * is exactly the part that matters. */
#if DEBUG_LOG
    watchdog_frame(++frame);
    debugLogFlush();
#endif
  }

  {
    size_t total = 0;
    const size_t used = mmap_arena_pages_used(&total);
    debugLogNote("[heap] mmap arena high-water: %zu of %zu pages (%zu of %zu MB)\n",
                 used, total, used * 4096 / (1024 * 1024), total * 4096 / (1024 * 1024));
  }
  if (Cocos_nativeOnPause) Cocos_nativeOnPause(fake_env, thiz);
#if DEBUG_LOG
  jni_dump_unimplemented(data_root);
  mmap_arena_report();
  pthread_key_report();
  watchdog_stop();
#endif
  debugLogNote("=== clean exit ===\n");
  debugLogClose();
  hw_input_save();
  cocos_video_shutdown();
  cocos_gl_deinit();
  opensles_shutdown();
  SDL_Quit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
