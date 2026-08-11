/* Controller -> synthetic touch mapping.
 *
 * Happy Wheels draws its own on-screen controls; every input the game
 * understands is a touch landing on one of those sprites. So the controller is
 * mapped by injecting touches at the positions where those sprites sit, rather
 * than by driving a free cursor around.
 *
 * Positions live in controls.cfg as normalised 0..1 screen coordinates, so they
 * survive a resolution or docked/handheld change. The defaults were measured
 * off gameplay captures rather than estimated; controls that have not been seen
 * on a capture are left unplaced and emit nothing, because a guessed position
 * can land on a real button and press it.
 *
 * Three ways a control can be driven -- a button, a stick offset from a pad
 * centre, or a stick pushed in one direction onto a fixed button. See the
 * enums below.
 *
 * Learn mode: A + Plus toggles it. The cursor appears and parks on the control
 * being edited, D-pad left/right steps through them, the left stick moves the
 * cursor and ZR commits. A + Plus again saves controls.cfg and leaves. It works
 * docked as well as handheld, and can bind the stick directions -- neither of
 * which was true of the older hold-an-input-and-tap-the-screen scheme.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <switch.h>

#include "config.h"
#include "hw_input.h"
#include "android_native_cocos.h"
#include "nx_pointer.h"

/* Pointer IDs for injected touches. Kept clear of the touchscreen's own
 * finger IDs (0..9) so real and synthetic touches can coexist. */
#define PTR_BASE 64

/* The cursor's own pointer, one past the bindings. */
#define PTR_CURSOR (PTR_BASE + NBIND_MAX)
#define NBIND_MAX  24

/* Three ways a control can be driven.
 *
 * BIND_BUTTON   a gamepad button presses a fixed spot.
 * BIND_PAD      a stick offsets a touch from a centre point, for HUDs that
 *               draw one round pad and read the touch position within it.
 * BIND_STICKDIR a stick deflected in one direction presses a fixed spot, for
 *               HUDs that draw separate left/right buttons instead of a pad.
 *               The dominant axis wins, so pushing up does not also fire left.
 */
enum { BIND_BUTTON = 0, BIND_PAD, BIND_STICKDIR };
enum { DIR_LEFT = 1, DIR_RIGHT, DIR_UP, DIR_DOWN };
enum { STICK_L = 0, STICK_R };

typedef struct {
  const char *name;
  uint64_t    button;   /* BIND_BUTTON only */
  float       nx, ny;   /* normalised position of the on-screen button */
  int         kind;
  float       radius;   /* BIND_PAD throw, as a fraction of screen height */
  int         stick;    /* STICK_L / STICK_R, for PAD and STICKDIR */
  int         dir;      /* DIR_*, for STICKDIR */
} Binding;

/* Order defines the pointer ID: PTR_BASE + index.
 *
 * Positions are measured off gameplay captures and normalised, so they hold at
 * 720p and 1080p alike.
 *
 * One caveat worth knowing: this is a single file covering every character, and
 * different characters put different actions in the same HUD slots. The
 * bottom-right slot is the pogo rider's "move right" and the pump character's
 * "grab", for instance -- so a binding aimed at one will press the other. The
 * two sets below are kept on separate inputs to limit that, but it cannot be
 * eliminated without per-character profiles.
 */
static Binding g_bind[] = {
  /* name          button                 nx       ny      kind           radius stick    dir       */

  /* --- pogo rider: two lean buttons, two move buttons, a jump. Measured. --- */
  { "lean_left",   0,                     0.0730f, 0.8486f, BIND_STICKDIR, 0.0f, STICK_R, DIR_LEFT  },
  { "lean_right",  0,                     0.2113f, 0.8486f, BIND_STICKDIR, 0.0f, STICK_R, DIR_RIGHT },
  { "move_left",   0,                     0.7867f, 0.8486f, BIND_STICKDIR, 0.0f, STICK_L, DIR_LEFT  },
  { "move_right",  0,                     0.9262f, 0.8514f, BIND_STICKDIR, 0.0f, STICK_L, DIR_RIGHT },
  { "jump",        HidNpadButton_ZR,      0.0625f, 0.6028f, BIND_BUTTON,   0.0f, 0,       0 },
  /* The bike's stop sits in the mirror of jump's slot, same row. Both are on ZR:
   * only one of them exists for any given character, so the other press lands
   * on empty screen. */
  { "stop",        HidNpadButton_ZR,      0.9371f, 0.6035f, BIND_BUTTON,   0.0f, 0,       0 },
  /* Retry, dead centre on the bottom row. */
  { "retry",       HidNpadButton_X,       0.4996f, 0.8493f, BIND_BUTTON,   0.0f, 0,       0 },

  /* --- pump character: four pose buttons in a diamond, plus the hand. Measured. --- */
  { "dpad_up",     HidNpadButton_Up,      0.1262f, 0.6618f, BIND_BUTTON,   0.0f, 0,       0 },
  { "dpad_left",   HidNpadButton_Left,    0.0625f, 0.7743f, BIND_BUTTON,   0.0f, 0,       0 },
  { "dpad_right",  HidNpadButton_Right,   0.1891f, 0.7743f, BIND_BUTTON,   0.0f, 0,       0 },
  { "dpad_down",   HidNpadButton_Down,    0.1258f, 0.8868f, BIND_BUTTON,   0.0f, 0,       0 },
  { "grab",        HidNpadButton_B,       0.9262f, 0.8493f, BIND_BUTTON,   0.0f, 0,       0 },

  /* --- corners, measured --- */
  { "eject",       HidNpadButton_R,       0.9367f, 0.1097f, BIND_BUTTON,   0.0f, 0,       0 },
  { "pause",       HidNpadButton_L,       0.0625f, 0.1111f, BIND_BUTTON,   0.0f, 0,       0 },

  /* --- not seen on either capture, so deliberately unplaced (-1) ---
   *
   * A guessed position is worse than none: "reset" was previously estimated at
   * (0.06, 0.08), which lands inside the pause disc measured at (0.0625,
   * 0.1111) -- pressing Minus would have paused the game. Unplaced controls
   * emit nothing at all until learn mode gives them a real position. */
  { "jet",         HidNpadButton_X,      -1.0f,   -1.0f,    BIND_BUTTON,   0.0f, 0,       0 },
  { "tuck",        HidNpadButton_ZL,     -1.0f,   -1.0f,    BIND_BUTTON,   0.0f, 0,       0 },
  { "reset",       HidNpadButton_Minus,  -1.0f,   -1.0f,    BIND_BUTTON,   0.0f, 0,       0 },
};
#define NBIND ((int)(sizeof(g_bind) / sizeof(g_bind[0])))

/* Each binding gets its own pointer id, so the engine feed must be able to
 * carry all of them at once -- both sticks plus every button really can be
 * held together. Caught the hard way: this was 13 against a capacity of 8. */
_Static_assert(NBIND <= NBIND_MAX, "raise NBIND_MAX");
/* +1 for the cursor's pointer, which can be live alongside the bindings. */
_Static_assert(NBIND + 1 <= COCOS_MAX_POINTERS,
               "more controller bindings than the pointer feed can carry");

static int   g_learn = 0;
static int   g_learn_prev_combo = 0;
static int   g_learn_sel = 0;      /* which binding learn mode is editing */
static char  g_cfg_path[HW_PATH_MAX + 32];

void hw_input_init(const char *data_root) {
  snprintf(g_cfg_path, sizeof g_cfg_path, "%s/%s", data_root, CONTROLS_NAME);

  FILE *fp = fopen(g_cfg_path, "r");
  if (!fp) { hw_input_save(); return; }

  char line[160];
  while (fgets(line, sizeof line, fp)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char name[64]; float x, y;
    if (sscanf(line, "%63s %f %f", name, &x, &y) != 3) continue;
    for (int i = 0; i < NBIND; i++) {
      if (!strcmp(g_bind[i].name, name)) {
        /* -1 means "not placed"; anything else must be on screen. */
        if (x < 0.0f || y < 0.0f)            { g_bind[i].nx = -1.0f; g_bind[i].ny = -1.0f; }
        else if (x <= 1.0f && y <= 1.0f)     { g_bind[i].nx = x;     g_bind[i].ny = y; }
        break;
      }
    }
  }
  fclose(fp);
}

void hw_input_save(void) {
  FILE *fp = fopen(g_cfg_path, "w");
  if (!fp) return;

  fprintf(fp,
    "# Screen positions of the game's on-screen controls, normalised 0..1.\n"
    "#   name  x  y\n"
    "#\n"
    "# To correct a position, press A + Plus to enter learn mode. The cursor\n"
    "# appears and parks on the control it is editing; D-pad left/right steps\n"
    "# through them in the order below, the left stick moves the cursor, and ZR\n"
    "# binds the current control to wherever the cursor is. A + Plus again\n"
    "# saves and leaves.\n"
    "#\n"
    "# Because the cursor parks itself on each control's stored position, you\n"
    "# can see which one you are editing without any labels on screen.\n"
    "#\n"
    "# Which input drives each control is fixed in the build; this file only\n"
    "# says where on screen each one presses.\n"
    "#\n"
    "#   lean_left / lean_right    right stick left / right\n"
    "#   move_left / move_right    left stick left / right\n"
    "#   jump / stop               ZR (opposite sides; only one exists at a time)\n"
    "#   retry                     X\n"
    "#   dpad_up/left/right/down   the D-pad, on the four-button diamond\n"
    "#   grab                      B\n"
    "#   eject / pause             R / L  (the two top corners)\n"
    "#   reset                     Minus\n"
    "#   jet / tuck                X / ZL\n"
    "#\n"
    "# One file covers every character, and characters reuse HUD slots for\n"
    "# different actions -- the bottom-right slot is the pogo rider's move_right\n"
    "# and the pump character's grab, so those two will press each other. Use\n"
    "# learn mode when a character's controls sit somewhere else.\n\n");
  for (int i = 0; i < NBIND; i++)
    fprintf(fp, "%-10s %.4f %.4f\n", g_bind[i].name, g_bind[i].nx, g_bind[i].ny);
  fclose(fp);
}

int hw_input_learn_active(void) { return g_learn; }

/* Learn mode, driven by the cursor.
 *
 * The old scheme -- hold the input, then tap the touchscreen where its
 * on-screen button is -- only worked in handheld, and could not bind the two
 * stick pads at all, since deflecting a stick to identify it is the same
 * gesture as using it.
 *
 * Now: D-pad left/right steps through the bindings and warps the cursor onto
 * the position currently stored for each one, so you can see which control you
 * are editing without any text on screen -- the cursor sits on it. Move the
 * cursor with the left stick, press ZR to commit. L+R+Minus again saves and
 * leaves.
 */
static void learn_update(PadState *pad, int w, int h) {
  const uint64_t pressed = padGetButtonsDown(pad);

  if (pressed & (HidNpadButton_Left | HidNpadButton_Right)) {
    g_learn_sel += (pressed & HidNpadButton_Right) ? 1 : -1;
    if (g_learn_sel < 0)      g_learn_sel = NBIND - 1;
    if (g_learn_sel >= NBIND) g_learn_sel = 0;
    /* Park the cursor on whatever that binding currently points at, or the
     * middle of the screen if it has never been placed. */
    if (g_bind[g_learn_sel].nx < 0.0f || g_bind[g_learn_sel].ny < 0.0f)
      nxp_warp(w * 0.5f, h * 0.5f);
    else
      nxp_warp(g_bind[g_learn_sel].nx * (float)w,
               g_bind[g_learn_sel].ny * (float)h);
  }

  if ((pressed & HidNpadButton_ZR) && w > 0 && h > 0) {
    float cx, cy;
    nxp_cursor_pos(&cx, &cy);
    g_bind[g_learn_sel].nx = cx / (float)w;
    g_bind[g_learn_sel].ny = cy / (float)h;
  }

  hw_input_feed(0, NULL, NULL, NULL);   /* no gameplay input while learning */
}

void hw_input_update(PadState *pad, int w, int h, int touch_active,
                     float touch_x, float touch_y) {
  (void)touch_active; (void)touch_x; (void)touch_y;

  uint64_t buttons = padGetButtons(pad);
  HidAnalogStickState ls = padGetStickPos(pad, 0);
  HidAnalogStickState rs = padGetStickPos(pad, 1);

  /* Learn-mode toggle: A + Plus, on the rising edge.
   *
   * It used to be L + R + Minus, which stopped working well once L and R
   * became pause and eject: every button in the combo then had to be silenced
   * whenever both shoulders were held, so rolling from one to the other could
   * swallow a press. A and Plus drive nothing at all, so nothing needs
   * suppressing and the shoulder buttons stay honest. */
  int combo = (buttons & HidNpadButton_A) && (buttons & HidNpadButton_Plus);
  if (combo && !g_learn_prev_combo) {
    g_learn = !g_learn;
    if (g_learn) {
      g_learn_sel = 0;
      nxp_set_visible(1);              /* you cannot aim what you cannot see */
      if (g_bind[0].nx < 0.0f) nxp_warp(w * 0.5f, h * 0.5f);
      else nxp_warp(g_bind[0].nx * (float)w, g_bind[0].ny * (float)h);
    } else {
      hw_input_save();
    }
  }
  g_learn_prev_combo = combo;

  if (g_learn) { learn_update(pad, w, h); return; }

  /* Cursor mode is modal on purpose. The left stick cannot drive the cursor and
   * the move pad at once, and worse, a held button would otherwise inject a
   * touch at its HUD position while a menu is up -- pressing whatever happens
   * to sit there. So while the cursor is visible the bindings are silent and
   * only the cursor reports. */
  if (nxp_cursor_visible()) {
    if (buttons & HidNpadButton_ZR) {
      float cx, cy;
      nxp_cursor_pos(&cx, &cy);
      const int   id = PTR_CURSOR;
      const float x = cx, y = cy;
      hw_input_feed(1, &id, &x, &y);
    } else {
      hw_input_feed(0, NULL, NULL, NULL);
    }
    return;
  }

  int   ids[NBIND];
  float xs[NBIND], ys[NBIND];
  int   n = 0;

  const float STICK_DEAD = 0.18f;

  for (int i = 0; i < NBIND; i++) {
    if (g_bind[i].nx < 0.0f || g_bind[i].ny < 0.0f)
      continue;                      /* unplaced: emits nothing */

    float px = g_bind[i].nx * (float)w;
    float py = g_bind[i].ny * (float)h;

    const HidAnalogStickState st = (g_bind[i].stick == STICK_R) ? rs : ls;
    const float sx = st.x / 32767.0f;
    const float sy = st.y / 32767.0f;

    if (g_bind[i].kind == BIND_PAD) {
      float mag = sqrtf(sx * sx + sy * sy);
      if (mag < STICK_DEAD) continue;
      float ux = sx, uy = sy;
      if (mag > 1.0f) { ux /= mag; uy /= mag; }
      const float r = g_bind[i].radius * (float)h;
      /* Screen Y grows downward; stick Y grows upward. */
      px += ux * r;
      py -= uy * r;

    } else if (g_bind[i].kind == BIND_STICKDIR) {
      /* The dominant axis decides, so a diagonal picks one control rather than
       * firing a horizontal and a vertical at once. */
      const float ax = sx < 0 ? -sx : sx;
      const float ay = sy < 0 ? -sy : sy;
      int fired = 0;
      switch (g_bind[i].dir) {
        case DIR_LEFT:  fired = (sx < -STICK_DEAD) && (ax >= ay); break;
        case DIR_RIGHT: fired = (sx >  STICK_DEAD) && (ax >= ay); break;
        case DIR_UP:    fired = (sy >  STICK_DEAD) && (ay >  ax); break;
        case DIR_DOWN:  fired = (sy < -STICK_DEAD) && (ay >  ax); break;
        default: break;
      }
      if (!fired) continue;

    } else {
      if (!(buttons & g_bind[i].button)) continue;
    }

    if (px < 0.0f) px = 0.0f;
    if (px > (float)w) px = (float)w;
    if (py < 0.0f) py = 0.0f;
    if (py > (float)h) py = (float)h;

    ids[n] = PTR_BASE + i;
    xs[n]  = px;
    ys[n]  = py;
    n++;
  }

  hw_input_feed(n, ids, xs, ys);
}
