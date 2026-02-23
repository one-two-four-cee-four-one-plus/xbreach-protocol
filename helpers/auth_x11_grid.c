/*
Copyright 2014 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

/*!
 * \brief Cyberpunk 2077 Breach Protocol auth module for XSecureLock.
 *
 * Forked from auth_x11.c. Replaces the standard password prompt with a
 * Cyberpunk 2077 "Breach Protocol" hacking minigame visualization.
 * The actual password is still typed normally underneath — the grid UI
 * is a purely visual/decorative layer.
 */

#include <X11/X.h>     // for Success, None, Atom, KBBellPitch
#include <X11/Xlib.h>  // for DefaultScreen, Screen, XFree, True
#include <locale.h>    // for NULL, setlocale, LC_CTYPE, LC_TIME
#include <stdio.h>
#include <stdlib.h>      // for free, rand, mblen, size_t, EXIT_...
#include <string.h>      // for strlen, memcpy, memset, strcspn
#include <sys/select.h>  // for timeval, select, fd_set, FD_SET
#include <sys/time.h>    // for gettimeofday, timeval
#include <time.h>        // for time, nanosleep, localtime_r
#include <unistd.h>      // for close, _exit, dup2, pipe, dup

#if __STDC_VERSION__ >= 199901L
#include <inttypes.h>
#include <stdint.h>
#endif

#ifdef HAVE_XFT_EXT
#include <X11/Xft/Xft.h>             // for XftColorAllocValue, XftColorFree
#include <X11/extensions/Xrender.h>  // for XRenderColor, XGlyphInfo
#include <fontconfig/fontconfig.h>   // for FcChar8
#endif

#ifdef HAVE_XKB_EXT
#include <X11/XKBlib.h>             // for XkbFreeKeyboard, XkbGetControls
#include <X11/extensions/XKB.h>     // for XkbUseCoreKbd, XkbGroupsWrapMask
#include <X11/extensions/XKBstr.h>  // for _XkbDesc, XkbStateRec, _XkbControls
#endif

#include "../env_info.h"          // for GetHostName, GetUserName
#include "../env_settings.h"      // for GetIntSetting, GetStringSetting
#include "../logging.h"           // for Log, LogErrno
#include "../mlock_page.h"        // for MLOCK_PAGE
#include "../util.h"              // for explicit_bzero
#include "../wait_pgrp.h"         // for WaitPgrp
#include "../wm_properties.h"     // for SetWMProperties
#include "../xscreensaver_api.h"  // for ReadWindowID
#include "authproto.h"            // for WritePacket, ReadPacket, PTYPE_R...
#include "monitors.h"             // for Monitor, GetMonitors, IsMonitorC...

/*! ===========================================================
 *  TYPE DEFINITIONS
 *  =========================================================== */

//! A position in the grid.
typedef struct {
  int row;
  int col;
} GridPos;

//! Axis for selection highlight.
enum Axis { AXIS_HORIZONTAL = 0, AXIS_VERTICAL = 1 };

//! A target sequence to complete.
#define MAX_TARGET_LEN 4

typedef struct {
  const char *name;
  int codes[MAX_TARGET_LEN];  // indices into HEX_CODES
  int length;
} TargetSequence;

//! Color identifiers for drawing.
enum DrawColor {
  COLOR_FOREGROUND = 0,
  COLOR_WARNING,
  COLOR_CYBER_GREEN,
  COLOR_CYBER_DIM,
  COLOR_CYBER_YELLOW,
  COLOR_CYBER_HIGHLIGHT,
  COLOR_CYBER_RED,
  COLOR_CYBER_COMPLETE,
  COLOR_BACKGROUND,
  COLOR_CONTENT_BG,
  COLOR_PANEL_BG,
  COLOR_COUNT
};

//! Number of args.
int argc;

//! Args.
char *const *argv;

//! The authproto helper to use.
const char *authproto_executable;

//! The maximum time to wait at a prompt for user input in seconds.
int prompt_timeout;

/*! ===========================================================
 *  CONFIGURATION CONSTANTS
 *  =========================================================== */

// --- Common: Colors ---

#define CFG_COLOR_BACKGROUND      "black"    /* Window background */
#define CFG_COLOR_FOREGROUND      "white"    /* Text on highlighted cells */
#define CFG_COLOR_WARNING         "red"      /* Warning messages */
#define CFG_COLOR_CYBER_GREEN     "#d0ed57"  /* Headers, normal cells */
#define CFG_COLOR_CYBER_DIM       "#00ffcc"  /* Used cells, slot outlines */
#define CFG_COLOR_CYBER_YELLOW    "#dbfd4f"  /* Current cell, filled slots, timer */
#define CFG_COLOR_CYBER_HIGHLIGHT "#212031"  /* Active row/column background */
#define CFG_COLOR_CYBER_RED       "#ff3333"  /* Low-time warning */
#define CFG_COLOR_CYBER_COMPLETE  "#00ffcc"  /* Completed sequences */
#define CFG_COLOR_CONTENT_BG      "#0e0e17"  /* Background behind content region */

// --- Common: Fonts ---

#define CFG_FONT_NAME             "monospace"  /* Default Xft font name */
#define CFG_FONT_CORE             "fixed"      /* Fallback X11 core font */

// --- Common: Layout ---

#define SECTION_AUTO              (-1)   /* Position sentinel: auto-center */
#define CFG_LINE_SPACING          4      /* Extra vertical space between text lines */
#define CFG_OUTLINE_THICKNESS     1      /* Default rectangle outline width in pixels */
#define CFG_REGION_W              2160   /* Content region width (0 = auto from layout) */
#define CFG_REGION_H              1350   /* Content region height (0 = auto from layout) */

// --- Common: Text Labels ---

#define CFG_TEXT_PAM_SAYS          "PAM says"
#define CFG_TEXT_ERROR             "Error"
#define CFG_TEXT_PROCESSING        "Processing..."
#define CFG_TEXT_MLOCK_WARN        "Password will not be stored securely."
#define CFG_TEXT_MLOCK_ERR         "Password has not been stored securely."

// --- Common: Auth ---

#define CFG_DEFAULT_TIMEOUT       100    /* Default auth timeout (seconds) */

// --- Element Visibility (1 = show, 0 = hide) ---

#define CFG_SHOW_PANEL            1      /* Outer panel border */
#define CFG_SHOW_MATRIX           1      /* 5x5 code matrix grid */
#define CFG_SHOW_TIMER            1      /* SS.CC countdown text */
#define CFG_SHOW_BAR              1      /* Progress bar */
#define CFG_SHOW_SEQUENCES        1      /* Sequence required section */
#define CFG_SHOW_RIGHT_PANEL      1      /* Right panel outline */

// --- Panel (outer border) ---

#define CFG_PANEL_X               100
#define CFG_PANEL_Y               150
#define CFG_PANEL_W               (2160 - 200)
#define CFG_PANEL_H               (1350 - 300)
#define CFG_COLOR_PANEL_BG        "#dbfd4f"  /* Panel outline color */
#define CFG_PANEL_GAP             0      /* Gap between matrix and right panel (auto: cell_w) */

// --- Code Matrix (5x5 grid) ---

#define CFG_MATRIX_X              300    /* Position (-1 = auto) */
#define CFG_MATRIX_Y              600
#define CFG_GRID_CELL_W           100    /* Cell width (0 = auto) */
#define CFG_GRID_CELL_H           100    /* Cell height (0 = auto) */
#define CFG_GRID_PAD_H            16     /* Added to text width for auto cell_w */
#define CFG_GRID_PAD_V            4      /* Added to text height for auto cell_h */
#define CFG_GRID_OUTLINE_THICKNESS 2     /* 0 = no grid outline */
#define CFG_GRID_OUTLINE_COLOR    COLOR_CYBER_GREEN
#define CFG_GRID_OUTLINE_PAD_LEFT  100
#define CFG_GRID_OUTLINE_PAD_RIGHT 100
#define CFG_GRID_OUTLINE_PAD_TOP   0
#define CFG_GRID_OUTLINE_PAD_BOTTOM 0
#define CFG_GRID_CELL_FG          COLOR_CYBER_GREEN    /* Normal cell text */
#define CFG_GRID_CELL_USED_FG     COLOR_CONTENT_BG      /* Used cell text */
#define CFG_GRID_CELL_USED_OUTLINE COLOR_CONTENT_BG     /* Used cell outline */
#define CFG_GRID_CELL_ACTIVE_FG   COLOR_FOREGROUND     /* Current cell text */
#define CFG_GRID_CELL_ACTIVE_BG   COLOR_CYBER_YELLOW   /* Current cell background */
#define CFG_GRID_HIGHLIGHT_BG     COLOR_CYBER_HIGHLIGHT /* Active row/column bg */
#define CFG_TEXT_CODE_MATRIX       "CODE MATRIX"

// --- Buffer (slot boxes) ---

#define CFG_BUFFER_X              1100   /* Position (-1 = auto) */
#define CFG_BUFFER_Y              440
#define CFG_BUF_CELL_W            60     /* Slot width (0 = auto) */
#define CFG_BUF_CELL_H            60     /* Slot height (0 = auto) */
#define CFG_BUF_PAD_H             0     /* Added to text width for auto slot_w */
#define CFG_BUF_PAD_V             4      /* Added to text height for auto slot_h */
#define CFG_SLOT_GAP              4      /* Gap between buffer slots */
#define CFG_BUFFER_SLOT_DASHED    1      /* 1 = dashed outline on empty slots */
#define CFG_BUFFER_DASH_LEN       8      /* Dash segment length in pixels */
#define CFG_BUFFER_DASH_GAP       4      /* Gap between dashes in pixels */
#define CFG_BUFFER_HEADER_FG      COLOR_CYBER_GREEN    /* "BUFFER" header text */
#define CFG_BUFFER_SLOT_OUTLINE   COLOR_CYBER_DIM      /* Empty/filled slot border */
#define CFG_BUFFER_SLOT_FILLED_FG COLOR_CYBER_YELLOW   /* Filled slot hex text */
#define CFG_BUFFER_SLOT_FILLED_BG COLOR_CYBER_HIGHLIGHT /* Confirmed slot background */
#define CFG_BUFFER_SLOT_EMPTY_FG  COLOR_CYBER_DIM      /* Empty slot "__" text */
#define CFG_TEXT_BUFFER            "BUFFER"
#define CFG_TEXT_EMPTY_SLOT        "__"

// --- Timer ---

#define CFG_TIMER_X               918    /* Position (-1 = auto) */
#define CFG_TIMER_Y               470
#define CFG_TIMER_W               0      /* Timer box width (0 = auto from text) */
#define CFG_TIMER_H               0      /* Timer box height (0 = auto from font) */
#define CFG_TIMER_PAD_H           16     /* Horizontal padding inside timer outline */
#define CFG_TIMER_INTERVAL_US     10     /* Update interval (microseconds) */
#define CFG_TIMER_MAX_CSEC        9999   /* Max display value (centiseconds) */
#define CFG_TIMER_RED_THRESHOLD   3000   /* Timer turns red below this (csec) */
#define CFG_TIMER_FORMAT          "%02d.%02d"  /* Display format */
#define CFG_TIMER_FG              COLOR_CYBER_YELLOW   /* Timer text (normal) */
#define CFG_TIMER_LOW_FG          COLOR_CYBER_RED      /* Timer text (low time) */
#define CFG_TIMER_OUTLINE         COLOR_CYBER_DIM      /* Timer outline */
#define CFG_TEXT_TIMER_HEADER      "BREACH TIME REMAINING"

// --- Progress Bar ---

#define CFG_BAR_X                 300    /* Position (-1 = auto) */
#define CFG_BAR_Y                 500
#define CFG_BAR_W                 700    /* Bar width (0 = auto: right panel width) */
#define CFG_BAR_H                 16      /* Bar height (auto: th/2, min 8) */
#define CFG_BAR_H_MIN             8      /* Minimum bar height when auto */
#define CFG_PROGRESS_OUTLINE      COLOR_CYBER_DIM      /* Progress bar frame */
#define CFG_PROGRESS_FILL         COLOR_CYBER_GREEN    /* Progress bar fill (normal) */
#define CFG_PROGRESS_FILL_LOW     COLOR_CYBER_RED      /* Progress bar fill (low) */

// --- Sequence Required ---

#define CFG_SEQ_X                 1100   /* Position (-1 = auto) */
#define CFG_SEQ_Y                 630
#define CFG_SEQ_CELL_W            60     /* Hex box width (0 = auto from font) */
#define CFG_SEQ_CELL_H            60     /* Hex box height (0 = auto from font) */
#define CFG_SEQ_PAD_H             16     /* Added to text width for auto seq cell_w */
#define CFG_SEQ_PAD_V             4      /* Added to text height for auto seq cell_h */
#define CFG_SEQ_HEX_GAP           4      /* Gap between hex codes in sequences */
#define CFG_SEQ_NAME_MARGIN       40     /* Gap between label column and last hex box */
#define CFG_SEQ_HEADER_FG         COLOR_CYBER_GREEN    /* "SEQUENCE REQUIRED" header */
#define CFG_SEQ_NAME_FG           COLOR_CYBER_GREEN    /* Target name (incomplete) */
#define CFG_SEQ_NAME_COMPLETE_FG  COLOR_CYBER_COMPLETE /* Target name (complete) */
#define CFG_SEQ_HEX_FG           COLOR_CYBER_YELLOW   /* Target hex (incomplete) */
#define CFG_SEQ_HEX_COMPLETE_FG  COLOR_CYBER_COMPLETE /* Target hex (complete) */
#define CFG_SEQ_HEX_OUTLINE      COLOR_CYBER_DIM      /* Hex code box outline */
#define CFG_SEQ_HEX_OUTLINE_COMPLETE COLOR_CYBER_COMPLETE /* Hex box outline (complete) */
#define CFG_TEXT_SEQ_HEADER        "SEQUENCE REQUIRED TO UPLOAD"

// --- Right Panel (outline around sequence section) ---

#define CFG_RIGHT_PANEL_W         0      /* Width (auto: widest label + padding) */
#define CFG_RIGHT_PANEL_PAD       16     /* Padding for auto right panel width */
#define CFG_RIGHT_PANEL_OUTLINE_THICKNESS 1  /* Outline width (0 = none) */
#define CFG_RIGHT_PANEL_OUTLINE_COLOR COLOR_CYBER_GREEN
#define CFG_RIGHT_PANEL_OUTLINE_PAD 10   /* Padding inside outline around content */

// --- Grid Data ---

#define GRID_SIZE         5
#define BUFFER_SIZE       6
#define NUM_HEX_CODES     4
#define NUM_TARGETS       3

//! The hex code strings displayed in cells.
static const char *HEX_CODES[NUM_HEX_CODES] = {"BD", "1C", "55", "7A"};

//! CODE_MATRIX[row][col] — index into HEX_CODES for each cell.
static const int CODE_MATRIX[GRID_SIZE][GRID_SIZE] = {
    {1, 3, 0, 2, 1},  // row 0: 1C 7A BD 55 1C
    {0, 2, 1, 3, 0},  // row 1: BD 55 1C 7A BD
    {2, 0, 3, 1, 2},  // row 2: 55 BD 7A 1C 55
    {3, 1, 0, 0, 3},  // row 3: 7A 1C BD BD 7A
    {0, 3, 2, 1, 0},  // row 4: BD 7A 55 1C BD
};

//! The hardcoded path through the grid. Each keypress advances one step.
static const GridPos HACK_SEQUENCE[BUFFER_SIZE] = {
    {0, 2},  // BD  (row 0, horizontal)
    {3, 2},  // BD  (col 2, vertical)
    {3, 1},  // 1C  (row 3, horizontal)
    {2, 1},  // BD  (col 1, vertical)
    {2, 3},  // 1C  (row 2, horizontal)
    {4, 3},  // 1C  (col 3, vertical)
};

//! The three target sequences (subsequences of the hack sequence buffer).
static const TargetSequence TARGETS[NUM_TARGETS] = {
    {"DATAMINE_V1", {0, 0}, 2},        // BD BD
    {"DATAMINE_V2", {0, 1, 0}, 3},     // BD 1C BD
    {"DATAMINE_V3", {1, 0, 1, 1}, 4},  // 1C BD 1C 1C
};

/*! ===========================================================
 *  RUNTIME STATE
 *  =========================================================== */

//! Runtime grid state.
typedef struct {
  int current_step;
  int buffer_codes[BUFFER_SIZE];  // indices into HEX_CODES for filled slots
  int buffer_count;
  enum Axis current_axis;
  int active_row;
  int active_col;
  int sequence_complete[NUM_TARGETS];
} GridState;

//! If set, we can start a new login session.
int have_switch_user_command;

//! If set, the prompt will be fixed by <username>@.
int show_username;

//! If set, the prompt will be fixed by <hostname>. If >1, the hostname will be
// shown in full and not cut at the first dot.
int show_hostname;

//! The local hostname.
char hostname[256];

//! The username to authenticate as.
char username[256];

//! The X11 display.
Display *display;

//! The X11 window provided by main. Provided from $XSCREENSAVER_WINDOW.
Window main_window;

//! main_window's parent. Used to create per-monitor siblings.
Window parent_window;

//! The X11 core font for the PAM messages.
XFontStruct *core_font;

#ifdef HAVE_XFT_EXT
//! Xft colors for all draw colors.
XftColor xft_colors[COLOR_COUNT];
XftFont *xft_font;
#endif

//! The background color.
XColor xcolor_background;

//! X11 colors for all draw colors.
XColor xcolors[COLOR_COUNT];

//! The x offset to apply to the entire display (to mitigate burn-in).
static int x_offset = 0;

//! The y offset to apply to the entire display (to mitigate burn-in).
static int y_offset = 0;

//! Maximum offset value when dynamic changes are enabled.
static int burnin_mitigation_max_offset = 0;

//! How much the offsets are allowed to change dynamically, and if so, how high.
static int burnin_mitigation_max_offset_change = 0;

//! Whether to play sounds during authentication.
static int auth_sounds = 0;

//! Whether we only want a single auth window.
static int single_auth_window = 0;

//! If set, we need to re-query monitor data and adjust windows.
int per_monitor_windows_dirty = 1;

#ifdef HAVE_XKB_EXT
//! If set, we show Xkb keyboard layout name.
int show_keyboard_layout = 1;
//! If set, we show Xkb lock/latch status rather than Xkb indicators.
int show_locks_and_latches = 0;
#endif

#define MAIN_WINDOW 0
#define MAX_WINDOWS 16

//! The number of active X11 per-monitor windows.
size_t num_windows = 0;

//! The X11 per-monitor windows to draw on.
Window windows[MAX_WINDOWS];

//! The X11 graphics contexts — one per color per window.
GC gcs_all[COLOR_COUNT][MAX_WINDOWS];

//! Offscreen backbuffers for flicker-free drawing.
Pixmap backbuf[MAX_WINDOWS];
int backbuf_w[MAX_WINDOWS];
int backbuf_h[MAX_WINDOWS];

#ifdef HAVE_XFT_EXT
//! The Xft draw contexts — targeting backbuffers.
XftDraw *xft_draws[MAX_WINDOWS];
#endif

int have_xkb_ext;

enum Sound { SOUND_PROMPT, SOUND_INFO, SOUND_ERROR, SOUND_SUCCESS };

#define NOTE_DS3 156
#define NOTE_A3 220
#define NOTE_DS4 311
#define NOTE_E4 330
#define NOTE_B4 494
#define NOTE_E5 659
int sounds[][2] = {
    /* SOUND_PROMPT=  */ {NOTE_B4, NOTE_E5},   // V|I I
    /* SOUND_INFO=    */ {NOTE_E5, NOTE_E5},   // I 2x
    /* SOUND_ERROR=   */ {NOTE_A3, NOTE_DS3},  // V7 2x
    /* SOUND_SUCCESS= */ {NOTE_DS4, NOTE_E4},  // V I
};
#define SOUND_SLEEP_MS 125
#define SOUND_TONE_MS 100

/* Shared functions: PlaySound, SwitchKeyboardLayout, GetIndicators,
 * TextAscent, TextDescent, XGlyphInfoExpandAmount, TextWidth,
 * StrAppend, BuildTitle, WaitForKeypress, FixedXftFontOpenName,
 * Authenticate.
 */
#include "auth_x11_common.inc.c"

void DestroyPerMonitorWindows(size_t keep_windows) {
  for (size_t i = keep_windows; i < num_windows; ++i) {
#ifdef HAVE_XFT_EXT
    XftDrawDestroy(xft_draws[i]);
#endif
    XFreePixmap(display, backbuf[i]);
    for (int c = 0; c < COLOR_COUNT; ++c) {
      XFreeGC(display, gcs_all[c][i]);
    }
    if (i == MAIN_WINDOW) {
      XUnmapWindow(display, windows[i]);
    } else {
      XDestroyWindow(display, windows[i]);
    }
  }
  if (num_windows > keep_windows) {
    num_windows = keep_windows;
  }
}

void CreateOrUpdatePerMonitorWindow(size_t i, const Monitor *monitor,
                                    int region_w, int region_h, int x_offset,
                                    int y_offset) {
  int w, h, x, y;

  // Check for full-monitor mode (signaled by negative dimensions).
  if (region_w < 0 || region_h < 0) {
    // Full-monitor mode: use entire monitor dimensions.
    w = monitor->width;
    h = monitor->height;
    x = monitor->x + x_offset;
    y = monitor->y + y_offset;
  } else {
    // Normal mode: center the region on the monitor.
    w = region_w;
    h = region_h;
    x = monitor->x + (monitor->width - w) / 2 + x_offset;
    y = monitor->y + (monitor->height - h) / 2 + y_offset;
    // Clip to monitor.
    if (x < 0) {
      w += x;
      x = 0;
    }
    if (y < 0) {
      h += y;
      y = 0;
    }
    if (x + w > monitor->x + monitor->width) {
      w = monitor->x + monitor->width - x;
    }
    if (y + h > monitor->y + monitor->height) {
      h = monitor->y + monitor->height - y;
    }
  }

  if (i < num_windows) {
    // Move the existing window.
    XMoveResizeWindow(display, windows[i], x, y, w, h);
    // Recreate backbuffer if size changed.
    if (w != backbuf_w[i] || h != backbuf_h[i]) {
      if (backbuf[i]) XFreePixmap(display, backbuf[i]);
      backbuf[i] = XCreatePixmap(display, windows[i], w, h,
                                  DefaultDepth(display, DefaultScreen(display)));
      backbuf_w[i] = w;
      backbuf_h[i] = h;
#ifdef HAVE_XFT_EXT
      if (xft_draws[i]) XftDrawChange(xft_draws[i], backbuf[i]);
#endif
    }
    return;
  }

  if (i > num_windows) {
    Log("Unreachable code - can't create monitor sequences with holes");
    abort();
  }

  // Add a new window.
  XSetWindowAttributes attrs = {0};
  attrs.background_pixel = xcolor_background.pixel;
  if (i == MAIN_WINDOW) {
    XMoveResizeWindow(display, main_window, x, y, w, h);
    XChangeWindowAttributes(display, main_window, CWBackPixel, &attrs);
    windows[i] = main_window;
  } else {
    windows[i] =
        XCreateWindow(display, parent_window, x, y, w, h, 0, CopyFromParent,
                      InputOutput, CopyFromParent, CWBackPixel, &attrs);
    SetWMProperties(display, windows[i], "xsecurelock", "auth_x11_grid", argc,
                    argv);
    Window stacking_order[2];
    stacking_order[0] = main_window;
    stacking_order[1] = windows[i];
    XRestackWindows(display, stacking_order, 2);
  }

  // Create offscreen backbuffer.
  backbuf[i] = XCreatePixmap(display, windows[i], w, h,
                              DefaultDepth(display, DefaultScreen(display)));
  backbuf_w[i] = w;
  backbuf_h[i] = h;

  // Create GCs for all colors.
  XGCValues gcattrs;
  gcattrs.function = GXcopy;
  gcattrs.background = xcolor_background.pixel;
  if (core_font != NULL) {
    gcattrs.font = core_font->fid;
  }
  for (int c = 0; c < COLOR_COUNT; ++c) {
    gcattrs.foreground = xcolors[c].pixel;
    gcs_all[c][i] = XCreateGC(display, windows[i],
                               GCFunction | GCForeground | GCBackground |
                                   (core_font != NULL ? GCFont : 0),
                               &gcattrs);
  }

#ifdef HAVE_XFT_EXT
  xft_draws[i] = XftDrawCreate(
      display, backbuf[i], DefaultVisual(display, DefaultScreen(display)),
      DefaultColormap(display, DefaultScreen(display)));
#endif

  // This window is now ready to use.
  XMapWindow(display, windows[i]);
  num_windows = i + 1;
}

void UpdatePerMonitorWindows(int monitors_changed, int region_w, int region_h,
                             int x_offset, int y_offset) {
  static size_t num_monitors = 0;
  static Monitor monitors[MAX_WINDOWS];

  if (monitors_changed) {
    num_monitors = GetMonitors(display, parent_window, monitors, MAX_WINDOWS);
  }

  if (single_auth_window) {
    Window unused_root, unused_child;
    int unused_root_x, unused_root_y, x, y;
    unsigned int unused_mask;
    XQueryPointer(display, parent_window, &unused_root, &unused_child,
                  &unused_root_x, &unused_root_y, &x, &y, &unused_mask);
    for (size_t i = 0; i < num_monitors; ++i) {
      if (x >= monitors[i].x && x < monitors[i].x + monitors[i].width &&
          y >= monitors[i].y && y < monitors[i].y + monitors[i].height) {
        CreateOrUpdatePerMonitorWindow(0, &monitors[i], region_w, region_h,
                                       x_offset, y_offset);
        return;
      }
    }
    if (num_monitors > 0) {
      CreateOrUpdatePerMonitorWindow(0, &monitors[0], region_w, region_h,
                                     x_offset, y_offset);
      DestroyPerMonitorWindows(1);
    } else {
      DestroyPerMonitorWindows(0);
    }
    return;
  }

  size_t new_num_windows = num_monitors;

  for (size_t i = 0; i < new_num_windows; ++i) {
    CreateOrUpdatePerMonitorWindow(i, &monitors[i], region_w, region_h,
                                   x_offset, y_offset);
  }

  DestroyPerMonitorWindows(new_num_windows);

  if (num_windows != new_num_windows) {
    Log("Unreachable code - expected to get %d windows, got %d",
        (int)new_num_windows, (int)num_windows);
  }
}

/*! ===========================================================
 *  TEXT HELPERS (grid-specific)
 *  =========================================================== */

/*! \brief Draw a string with a specific color.
 */
void DrawString(int monitor, int x, int y, enum DrawColor color,
                const char *string, int len) {
#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    XGlyphInfo extents;
    XftTextExtentsUtf8(display, xft_font, (const FcChar8 *)string, len,
                       &extents);
    XftDrawStringUtf8(xft_draws[monitor], &xft_colors[color], xft_font,
                      x + XGlyphInfoExpandAmount(&extents), y,
                      (const FcChar8 *)string, len);
    return;
  }
#endif
  XDrawString(display, backbuf[monitor], gcs_all[color][monitor], x, y, string,
              len);
}

/*! \brief Fill a rectangle with a specific color.
 */
void FillRect(int monitor, int x, int y, int w, int h, enum DrawColor color) {
  XFillRectangle(display, backbuf[monitor], gcs_all[color][monitor], x, y, w,
                 h);
}

/*! \brief Fill a rectangle with the background color.
 */
void FillRectBackground(int monitor, int x, int y, int w, int h) {
  FillRect(monitor, x, y, w, h, COLOR_BACKGROUND);
}

/*! \brief Draw a rectangle outline with a specific color.
 */
void DrawRect(int monitor, int x, int y, int w, int h, enum DrawColor color,
              int thickness) {
  for (int t = 0; t < thickness; ++t) {
    XDrawRectangle(display, backbuf[monitor], gcs_all[color][monitor],
                   x + t, y + t, w - 1 - 2*t, h - 1 - 2*t);
  }
}

/*! \brief Draw a dashed rectangle outline with a specific color.
 */
void DrawRectDashed(int monitor, int x, int y, int w, int h,
                    enum DrawColor color, int thickness,
                    int dash_len, int gap_len) {
  GC gc = gcs_all[color][monitor];
  char dashes[2] = {dash_len, gap_len};
  XSetDashes(display, gc, 0, dashes, 2);
  XSetLineAttributes(display, gc, thickness, LineOnOffDash, CapButt, JoinMiter);
  XDrawRectangle(display, backbuf[monitor], gc, x, y, w - 1, h - 1);
  XSetLineAttributes(display, gc, 0, LineSolid, CapButt, JoinMiter);
}

#define NO_COLOR COLOR_COUNT  /* Sentinel: skip fill/outline/text */

/*! \brief Draw a box with optional fill, outline, and centered text.
 */
void DrawBox(int monitor, int x, int y, int w, int h,
             enum DrawColor bg,
             enum DrawColor outline,
             const char *text, int text_len,
             enum DrawColor text_fg,
             int pad_h) {
  if (bg != NO_COLOR) {
    FillRect(monitor, x, y, w, h, bg);
  }
  if (outline != NO_COLOR) {
    DrawRect(monitor, x, y, w, h, outline, CFG_OUTLINE_THICKNESS);
  }
  if (text_fg != NO_COLOR && text != NULL && text_len > 0) {
    int cell_to = (h + TextAscent() - TextDescent()) / 2;
    int tw = TextWidth(text, text_len);
    int tx = (pad_h > 0) ? x + pad_h : x + (w - tw) / 2;
    DrawString(monitor, tx, y + cell_to, text_fg, text, text_len);
  }
}

/*! \brief Draw a line between two points with CFG_OUTLINE_THICKNESS weight.
 */
void DrawLine(int monitor, int x1, int y1, int x2, int y2,
              enum DrawColor color) {
  for (int t = 0; t < CFG_OUTLINE_THICKNESS; ++t) {
    XDrawLine(display, backbuf[monitor], gcs_all[color][monitor],
              x1 + t, y1, x2 + t, y2);
  }
}

/*! \brief Draw a null-terminated string (convenience wrapper over DrawString).
 */
void DrawText(int monitor, int x, int y, enum DrawColor color,
              const char *text) {
  int len = strlen(text);
  DrawString(monitor, x, y, color, text, len);
}

/*! \brief Draw a filled and/or outlined polygon. Use NO_COLOR to skip.
 */
void DrawPoly(int monitor, XPoint *points, int npoints,
              enum DrawColor fill, enum DrawColor outline) {
  if (fill != NO_COLOR) {
    XFillPolygon(display, backbuf[monitor], gcs_all[fill][monitor],
                 points, npoints, Complex, CoordModeOrigin);
  }
  if (outline != NO_COLOR) {
    XDrawLines(display, backbuf[monitor], gcs_all[outline][monitor],
               points, npoints, CoordModeOrigin);
  }
}

/*! ===========================================================
 *  GRID LOGIC
 *  =========================================================== */

/*! \brief Initialize the grid state for a new prompt.
 */
void InitGridState(GridState *gs) {
  gs->current_step = 0;
  gs->buffer_count = 0;
  gs->current_axis = AXIS_HORIZONTAL;
  gs->active_row = HACK_SEQUENCE[0].row;
  gs->active_col = HACK_SEQUENCE[0].col;
  memset(gs->buffer_codes, 0, sizeof(gs->buffer_codes));
  memset(gs->sequence_complete, 0, sizeof(gs->sequence_complete));
}

/*! \brief Check if a cell has already been used in the hack sequence.
 */
int IsCellUsed(const GridState *gs, int row, int col) {
  for (int i = 0; i < gs->current_step; ++i) {
    if (HACK_SEQUENCE[i].row == row && HACK_SEQUENCE[i].col == col) {
      return 1;
    }
  }
  return 0;
}

/*! \brief Check if each target appears as a contiguous subsequence in the
 * buffer.
 */
void CheckSequenceCompletion(GridState *gs) {
  for (int t = 0; t < NUM_TARGETS; ++t) {
    gs->sequence_complete[t] = 0;
    if (TARGETS[t].length > gs->buffer_count) {
      continue;
    }
    // Check all possible starting positions.
    for (int start = 0; start <= gs->buffer_count - TARGETS[t].length;
         ++start) {
      int match = 1;
      for (int j = 0; j < TARGETS[t].length; ++j) {
        if (gs->buffer_codes[start + j] != TARGETS[t].codes[j]) {
          match = 0;
          break;
        }
      }
      if (match) {
        gs->sequence_complete[t] = 1;
        break;
      }
    }
  }
}

/*! \brief Advance the grid state by one step (on keypress).
 */
void GridAdvanceStep(GridState *gs) {
  if (gs->current_step >= BUFFER_SIZE) {
    return;
  }

  // Record the hex code at the current position.
  int row = HACK_SEQUENCE[gs->current_step].row;
  int col = HACK_SEQUENCE[gs->current_step].col;
  gs->buffer_codes[gs->buffer_count] = CODE_MATRIX[row][col];
  gs->buffer_count++;
  gs->current_step++;

  // Toggle axis and update active row/col from next hack sequence entry.
  if (gs->current_step < BUFFER_SIZE) {
    gs->current_axis =
        (gs->current_axis == AXIS_HORIZONTAL) ? AXIS_VERTICAL : AXIS_HORIZONTAL;
    gs->active_row = HACK_SEQUENCE[gs->current_step].row;
    gs->active_col = HACK_SEQUENCE[gs->current_step].col;
  }

  CheckSequenceCompletion(gs);
}

/*! \brief Rewind the grid state by one step (on backspace).
 */
void GridRewindStep(GridState *gs) {
  if (gs->current_step <= 0) {
    return;
  }

  gs->current_step--;
  gs->buffer_count--;

  // Recompute axis: step 0 = horizontal, step 1 = vertical, etc.
  gs->current_axis =
      (gs->current_step % 2 == 0) ? AXIS_HORIZONTAL : AXIS_VERTICAL;
  gs->active_row = HACK_SEQUENCE[gs->current_step].row;
  gs->active_col = HACK_SEQUENCE[gs->current_step].col;

  CheckSequenceCompletion(gs);
}

/*! ===========================================================
 *  LAYOUT & TIMING HELPERS
 *  =========================================================== */

typedef struct {
  int grid_cw, grid_ch;       /* Grid cell dimensions */
  int buf_cw, buf_ch;         /* Buffer slot dimensions */
  int seq_cw, seq_ch;         /* Sequence hex box dimensions */
  int th, to, bar_h, bar_w, timer_w, timer_h;
  int matrix_w, gap, right_w, buffer_h;
  int region_w, region_h;
  int buffer_x, buffer_y;   /* Buffer section origin */
  int matrix_x, matrix_y;   /* Code matrix section origin */
  int timer_x, timer_y;     /* Timer text origin */
  int bar_x, bar_y;         /* Progress bar origin */
  int seq_x, seq_y;         /* Sequence section origin */
  int rpanel_x, rpanel_y, rpanel_w, rpanel_h;  /* Right panel outline bounds */
} LayoutInfo;

void ComputeLayout(LayoutInfo *L) {
  // 1. Compute font metrics (always from font).
  L->th = TextAscent() + TextDescent() + CFG_LINE_SPACING;
  L->to = TextAscent() + CFG_LINE_SPACING / 2;

  // 2. Compute sizes with auto-override logic.
  int hex_tw = TextWidth("BD", 2);

  if (CFG_GRID_CELL_W > 0) {
    L->grid_cw = CFG_GRID_CELL_W;
  } else {
    L->grid_cw = hex_tw + CFG_GRID_PAD_H;
  }

  if (CFG_GRID_CELL_H > 0) {
    L->grid_ch = CFG_GRID_CELL_H;
  } else {
    L->grid_ch = L->th + CFG_GRID_PAD_V;
  }

  if (CFG_BUF_CELL_W > 0) {
    L->buf_cw = CFG_BUF_CELL_W;
  } else {
    L->buf_cw = hex_tw + CFG_BUF_PAD_H;
  }

  if (CFG_BUF_CELL_H > 0) {
    L->buf_ch = CFG_BUF_CELL_H;
  } else {
    L->buf_ch = L->th + CFG_BUF_PAD_V;
  }

  if (CFG_SEQ_CELL_W > 0) {
    L->seq_cw = CFG_SEQ_CELL_W;
  } else {
    L->seq_cw = hex_tw + CFG_SEQ_PAD_H;
  }

  if (CFG_SEQ_CELL_H > 0) {
    L->seq_ch = CFG_SEQ_CELL_H;
  } else {
    L->seq_ch = L->th + CFG_SEQ_PAD_V;
  }

  if (CFG_BAR_H > 0) {
    L->bar_h = CFG_BAR_H;
  } else {
    L->bar_h = L->th / 2;
    if (L->bar_h < CFG_BAR_H_MIN) L->bar_h = CFG_BAR_H_MIN;
  }

  if (CFG_PANEL_GAP > 0) {
    L->gap = CFG_PANEL_GAP;
  } else {
    L->gap = L->grid_cw;
  }

  if (CFG_RIGHT_PANEL_W > 0) {
    L->right_w = CFG_RIGHT_PANEL_W;
  } else {
    int widest = TextWidth(CFG_TEXT_SEQ_HEADER, strlen(CFG_TEXT_SEQ_HEADER));
    int buf_slots_w = BUFFER_SIZE * (L->buf_cw + CFG_SLOT_GAP);
    if (buf_slots_w > widest) widest = buf_slots_w;
    L->right_w = widest + CFG_RIGHT_PANEL_PAD;
  }

  if (CFG_BAR_W > 0) {
    L->bar_w = CFG_BAR_W;
  } else {
    L->bar_w = L->right_w;
  }

  // Timer box dimensions.
  if (CFG_TIMER_W > 0) {
    L->timer_w = CFG_TIMER_W;
  } else {
    // "99.99" is always 5 chars in the fixed format.
    L->timer_w = TextWidth("99.99", 5) + CFG_TIMER_PAD_H * 2;
  }

  if (CFG_TIMER_H > 0) {
    L->timer_h = CFG_TIMER_H;
  } else {
    L->timer_h = L->th;
  }

  int grid_inset_x = CFG_GRID_OUTLINE_THICKNESS + CFG_GRID_OUTLINE_PAD_LEFT
                   + CFG_GRID_OUTLINE_THICKNESS + CFG_GRID_OUTLINE_PAD_RIGHT;
  int grid_inset_y = CFG_GRID_OUTLINE_THICKNESS + CFG_GRID_OUTLINE_PAD_TOP
                   + CFG_GRID_OUTLINE_THICKNESS + CFG_GRID_OUTLINE_PAD_BOTTOM;
  L->matrix_w = grid_inset_x + GRID_SIZE * L->grid_cw;
  L->buffer_h = L->th * 2 + L->buf_ch;

  // 3. Compute auto positions (centered layout).
  int matrix_h = L->th + grid_inset_y + GRID_SIZE * L->grid_ch;
  int timer_h = L->th;
  int seq_h = L->th + CFG_LINE_SPACING + NUM_TARGETS * (L->seq_ch + CFG_LINE_SPACING);

  int right_panel_h = timer_h + L->bar_h + seq_h;
  int left_panel_h = L->buffer_h + matrix_h;
  int total_h = (left_panel_h > right_panel_h) ? left_panel_h : right_panel_h;
  int total_w = L->matrix_w + L->gap + L->right_w;

  // Centered auto positions within the logical region.
  int auto_buffer_x = 0;
  int auto_buffer_y = 0;
  int auto_matrix_x = 0;
  int auto_matrix_y = L->buffer_h;
  int auto_timer_x = L->matrix_w + L->gap;
  int auto_timer_y = 0;
  int auto_bar_x = auto_timer_x;
  int auto_bar_y = auto_timer_y + timer_h;
  int auto_seq_x = L->matrix_w + L->gap;
  int auto_seq_y = timer_h + L->bar_h;

  // 4. Apply overrides from CFG_* constants.
  L->buffer_x = (CFG_BUFFER_X != SECTION_AUTO) ? CFG_BUFFER_X : auto_buffer_x;
  L->buffer_y = (CFG_BUFFER_Y != SECTION_AUTO) ? CFG_BUFFER_Y : auto_buffer_y;
  L->matrix_x = (CFG_MATRIX_X != SECTION_AUTO) ? CFG_MATRIX_X : auto_matrix_x;
  L->matrix_y = (CFG_MATRIX_Y != SECTION_AUTO) ? CFG_MATRIX_Y : auto_matrix_y;
  L->timer_x = (CFG_TIMER_X != SECTION_AUTO) ? CFG_TIMER_X : auto_timer_x;
  L->timer_y = (CFG_TIMER_Y != SECTION_AUTO) ? CFG_TIMER_Y : auto_timer_y;
  L->bar_x = (CFG_BAR_X != SECTION_AUTO) ? CFG_BAR_X : auto_bar_x;
  L->bar_y = (CFG_BAR_Y != SECTION_AUTO) ? CFG_BAR_Y : auto_bar_y;
  L->seq_x = (CFG_SEQ_X != SECTION_AUTO) ? CFG_SEQ_X : auto_seq_x;
  L->seq_y = (CFG_SEQ_Y != SECTION_AUTO) ? CFG_SEQ_Y : auto_seq_y;

  // 5. Compute final region_w, region_h as bounding box.
  L->region_w = (CFG_REGION_W > 0) ? CFG_REGION_W : total_w;
  L->region_h = (CFG_REGION_H > 0) ? CFG_REGION_H : total_h;

  // 6. Compute right panel outline bounds.
  int rp_pad = CFG_RIGHT_PANEL_OUTLINE_PAD;
  // Top-left: sequence section origin, minus padding.
  int rp_top_x = L->seq_x;
  int rp_top_y = L->seq_y;
  // Bottom: sequence section bottom edge.
  int rp_bot_y = L->seq_y + NUM_TARGETS * (L->seq_ch + CFG_LINE_SPACING);
  // Right edge: use right_w from the leftmost right-panel x.
  L->rpanel_x = rp_top_x - rp_pad;
  L->rpanel_y = rp_top_y - rp_pad;
  L->rpanel_w = L->right_w + 2 * rp_pad;
  L->rpanel_h = (rp_bot_y - rp_top_y) + 2 * rp_pad;
}

/*! \brief Compute centiseconds remaining from deadline to now.
 */
static int ComputeCentisecondsRemaining(const struct timeval *deadline,
                                        const struct timeval *now) {
  long sec_diff = deadline->tv_sec - now->tv_sec;
  long usec_diff = deadline->tv_usec - now->tv_usec;
  int csec = (int)(sec_diff * 100 + usec_diff / 10000);
  if (csec < 0) csec = 0;
  return csec;
}

/*! ===========================================================
 *  DRAWING FUNCTIONS
 *  =========================================================== */

/*! \brief Draw the 5x5 CODE MATRIX section.
 *
 * \param monitor The window index.
 * \param ox X origin of the matrix area.
 * \param oy Y origin of the matrix area.
 * \param cell_w Width of each cell.
 * \param cell_h Height of each cell.
 * \param gs The current grid state.
 */
void DrawCodeMatrix(int monitor, int ox, int oy, int cell_w, int cell_h,
                    const GridState *gs) {
  int inset_l = CFG_GRID_OUTLINE_THICKNESS + CFG_GRID_OUTLINE_PAD_LEFT;
  int inset_t = CFG_GRID_OUTLINE_THICKNESS + CFG_GRID_OUTLINE_PAD_TOP;
  int cells_w = GRID_SIZE * cell_w;
  int cells_h = GRID_SIZE * cell_h;
  int outline_w = CFG_GRID_OUTLINE_PAD_LEFT + cells_w + CFG_GRID_OUTLINE_PAD_RIGHT
                  + 2 * CFG_GRID_OUTLINE_THICKNESS;
  int outline_h = CFG_GRID_OUTLINE_PAD_TOP + cells_h + CFG_GRID_OUTLINE_PAD_BOTTOM
                  + 2 * CFG_GRID_OUTLINE_THICKNESS;

  // Draw grid outline.
  if (CFG_GRID_OUTLINE_THICKNESS > 0) {
    DrawRect(monitor, ox, oy, outline_w, outline_h,
             CFG_GRID_OUTLINE_COLOR, CFG_GRID_OUTLINE_THICKNESS);
    // Pentagon sitting on top of grid outline (adjust points as needed).
    XPoint pentagon[5] = {
      { ox + outline_w / 30,  oy - outline_h / 10  },  // top apex
      { ox + outline_w,      oy - outline_h / 10  },  // upper-right
      { ox + outline_w,      oy                  },  // bottom-right (= top-right of outline)
      { ox,                  oy                  },  // bottom-left  (= top-left of outline)
      { ox,                  oy - outline_h / 20  },  // upper-left
    };
    XFillPolygon(display, backbuf[monitor], gcs_all[CFG_GRID_OUTLINE_COLOR][monitor],
                 pentagon, 5, Convex, CoordModeOrigin);
  }

  // Cell origin (inset from outline).
  int gx = ox + inset_l;
  int gy = oy + inset_t;
  int inner_x = ox + CFG_GRID_OUTLINE_THICKNESS;
  int inner_y = oy + CFG_GRID_OUTLINE_THICKNESS;
  int inner_w = outline_w - 2 * CFG_GRID_OUTLINE_THICKNESS;
  int inner_h = outline_h - 2 * CFG_GRID_OUTLINE_THICKNESS;

  // Draw full-width row / full-height column highlights (spans padding).
  if (gs->current_axis == AXIS_HORIZONTAL) {
    int ry = gy + gs->active_row * cell_h;
    FillRect(monitor, inner_x, ry, inner_w, cell_h, CFG_GRID_HIGHLIGHT_BG);
  } else {
    int colx = gx + gs->active_col * cell_w;
    FillRect(monitor, colx, inner_y, cell_w, inner_h, CFG_GRID_HIGHLIGHT_BG);
  }

  for (int row = 0; row < GRID_SIZE; ++row) {
    for (int col = 0; col < GRID_SIZE; ++col) {
      int cx = gx + col * cell_w;
      int cy = gy + row * cell_h;

      int is_current = (gs->current_step < BUFFER_SIZE &&
                        row == HACK_SEQUENCE[gs->current_step].row &&
                        col == HACK_SEQUENCE[gs->current_step].col);
      int used = IsCellUsed(gs, row, col);

      enum DrawColor bg = is_current ? CFG_GRID_CELL_ACTIVE_BG : NO_COLOR;
      enum DrawColor ol = used ? CFG_GRID_CELL_USED_OUTLINE : NO_COLOR;
      enum DrawColor text_color = used ? CFG_GRID_CELL_USED_FG
                                  : is_current ? CFG_GRID_CELL_ACTIVE_FG
                                  : CFG_GRID_CELL_FG;

      const char *hex = HEX_CODES[CODE_MATRIX[row][col]];
      int hxlen = strlen(hex);
      DrawBox(monitor, cx, cy, cell_w, cell_h, bg, ol, hex, hxlen,
              text_color, 0);
    }
  }
}

/*! \brief Draw the BUFFER section (filled and empty slots).
 *
 * \param monitor The window index.
 * \param ox X origin.
 * \param oy Y origin (baseline of the header text).
 * \param cell_w Width of each buffer slot.
 * \param cell_h Height of each buffer slot.
 * \param gs The current grid state.
 */
void DrawBufferSection(int monitor, int ox, int oy, int cell_w, int cell_h,
                       const GridState *gs) {
  int buffer_w = BUFFER_SIZE * (cell_w + CFG_SLOT_GAP) - CFG_SLOT_GAP;

  for (int i = 0; i < BUFFER_SIZE; ++i) {
    int sx = ox + i * (cell_w + CFG_SLOT_GAP);
    int sy = oy;
    int filled = (i < gs->buffer_count);

    enum DrawColor bg = (filled && i < gs->buffer_count - 1)
                        ? CFG_BUFFER_SLOT_FILLED_BG : NO_COLOR;
    const char *txt = filled
                      ? HEX_CODES[gs->buffer_codes[i]] : CFG_TEXT_EMPTY_SLOT;
    int tlen = strlen(txt);
    enum DrawColor fg = filled
                        ? CFG_BUFFER_SLOT_FILLED_FG : CFG_BUFFER_SLOT_EMPTY_FG;

#if CFG_BUFFER_SLOT_DASHED
    if (!filled) {
      // Empty slot: dashed outline, text only (no solid outline via DrawBox).
      DrawRectDashed(monitor, sx, sy, cell_w, cell_h,
                     CFG_BUFFER_SLOT_OUTLINE, CFG_OUTLINE_THICKNESS,
                     CFG_BUFFER_DASH_LEN, CFG_BUFFER_DASH_GAP);
      DrawBox(monitor, sx, sy, cell_w, cell_h, bg, NO_COLOR,
              txt, tlen, fg, 0);
    } else {
      DrawBox(monitor, sx, sy, cell_w, cell_h, bg, CFG_BUFFER_SLOT_OUTLINE,
              txt, tlen, fg, 0);
    }
#else
    DrawBox(monitor, sx, sy, cell_w, cell_h, bg, CFG_BUFFER_SLOT_OUTLINE,
            txt, tlen, fg, 0);
#endif
  }
}

/*! \brief Draw the SS.CC timer text.
 *
 * Draws to the backbuffer. Caller is responsible for blitting to screen.
 */
void DrawTimerText(int monitor, int ox, int oy, int box_w, int box_h,
                   int csec_remaining) {
  char timebuf[8];
  int display_csec = csec_remaining;
  if (display_csec < 0) display_csec = 0;
  if (display_csec > CFG_TIMER_MAX_CSEC) display_csec = CFG_TIMER_MAX_CSEC;
  snprintf(timebuf, sizeof(timebuf), CFG_TIMER_FORMAT,
           display_csec / 100, display_csec % 100);
  enum DrawColor timer_color =
      (csec_remaining < CFG_TIMER_RED_THRESHOLD) ? CFG_TIMER_LOW_FG : CFG_TIMER_FG;
  DrawBox(monitor, ox, oy, box_w, box_h, NO_COLOR, CFG_TIMER_OUTLINE,
          timebuf, 5, timer_color, CFG_TIMER_PAD_H);
}

/*! \brief Draw the progress bar.
 *
 * Draws to the backbuffer. Caller is responsible for blitting to screen.
 */
void DrawProgressBar(int monitor, int ox, int oy, int bar_w, int bar_h,
                     int csec_remaining, int csec_total) {
  DrawBox(monitor, ox, oy, bar_w, bar_h, NO_COLOR, CFG_PROGRESS_OUTLINE,
          NULL, 0, NO_COLOR, 0);
  int fill_w = 0;
  if (csec_total > 0 && csec_remaining > 0) {
    fill_w = (bar_w - 2) * csec_remaining / csec_total;
    if (fill_w > bar_w - 2) fill_w = bar_w - 2;
    if (fill_w < 1) fill_w = 1;
  }
  enum DrawColor bar_color =
      (csec_remaining < CFG_TIMER_RED_THRESHOLD) ? CFG_PROGRESS_FILL_LOW : CFG_PROGRESS_FILL;
  if (fill_w > 0) {
    FillRect(monitor, ox + 1 + (bar_w - 2 - fill_w), oy + 1, fill_w, bar_h - 2, bar_color);
  }
}

/*! \brief Draw the SEQUENCE REQUIRED TO UPLOAD section.
 *
 * \param monitor The window index.
 * \param ox X origin.
 * \param oy Y origin.
 * \param cell_w Width of each hex code cell.
 * \param gs The current grid state.
 */
void DrawSequenceSection(int monitor, int ox, int oy, int cell_w, int cell_h,
                         const GridState *gs) {
  // Find the longest sequence to align all DATAMINE labels at the same x.
  int max_len = 0;
  for (int t = 0; t < NUM_TARGETS; ++t) {
    if (TARGETS[t].length > max_len) max_len = TARGETS[t].length;
  }
  int name_x = ox + max_len * (cell_w + CFG_SEQ_HEX_GAP)
               - CFG_SEQ_HEX_GAP + CFG_SEQ_NAME_MARGIN;

  for (int t = 0; t < NUM_TARGETS; ++t) {
    int complete = gs->sequence_complete[t];

    // Draw outlined hex code boxes (left-aligned).
    for (int j = 0; j < TARGETS[t].length; ++j) {
      int sx = ox + j * (cell_w + CFG_SEQ_HEX_GAP);
      const char *hex = HEX_CODES[TARGETS[t].codes[j]];
      int hxlen = strlen(hex);

      enum DrawColor code_color =
          complete ? CFG_SEQ_HEX_COMPLETE_FG : CFG_SEQ_HEX_FG;
      enum DrawColor outline_color =
          complete ? CFG_SEQ_HEX_OUTLINE_COMPLETE : CFG_SEQ_HEX_OUTLINE;
      DrawBox(monitor, sx, oy, cell_w, cell_h, NO_COLOR, outline_color,
              hex, hxlen, code_color, 0);
    }

    // Draw DATAMINE label at aligned column.
    int name_y = oy + (cell_h + TextAscent() - TextDescent()) / 2;
    enum DrawColor name_color =
        complete ? CFG_SEQ_NAME_COMPLETE_FG : CFG_SEQ_NAME_FG;
    const char *name = TARGETS[t].name;
    DrawString(monitor, name_x, name_y, name_color, name, strlen(name));

    oy += cell_h + CFG_LINE_SPACING;
  }
}

/*! \brief Display the full Breach Protocol UI (all sections).
 *
 * Draws everything to offscreen backbuffers, then blits atomically.
 * Called on first render, after input, and after monitor changes.
 */
void DisplayBreachProtocolFull(const GridState *gs, int csec_remaining,
                               int csec_total) {
  LayoutInfo L;
  ComputeLayout(&L);

  // Compute burn-in mitigation offset for content (not window position).
  int content_x_offset = 0;
  int content_y_offset = 0;
  if (burnin_mitigation_max_offset_change > 0) {
    x_offset += rand() % (2 * burnin_mitigation_max_offset_change + 1) -
                burnin_mitigation_max_offset_change;
    if (x_offset < -burnin_mitigation_max_offset) {
      x_offset = -burnin_mitigation_max_offset;
    }
    if (x_offset > burnin_mitigation_max_offset) {
      x_offset = burnin_mitigation_max_offset;
    }
    y_offset += rand() % (2 * burnin_mitigation_max_offset_change + 1) -
                burnin_mitigation_max_offset_change;
    if (y_offset < -burnin_mitigation_max_offset) {
      y_offset = -burnin_mitigation_max_offset;
    }
    if (y_offset > burnin_mitigation_max_offset) {
      y_offset = burnin_mitigation_max_offset;
    }
    content_x_offset = x_offset;
    content_y_offset = y_offset;
  }

  // Use negative values to signal full-monitor mode.
  UpdatePerMonitorWindows(per_monitor_windows_dirty, -1, -1, 0, 0);
  per_monitor_windows_dirty = 0;

  for (size_t i = 0; i < num_windows; ++i) {
    // Clear backbuffer.
    FillRect(i, 0, 0, backbuf_w[i], backbuf_h[i], COLOR_CONTENT_BG);

    // Center content on the monitor with burn-in offset.
    int cx = (backbuf_w[i] - L.region_w) / 2 + content_x_offset;
    int cy = (backbuf_h[i] - L.region_h) / 2 + content_y_offset;

    // Draw panel outline.
#if CFG_SHOW_PANEL
    DrawRect(i, cx + CFG_PANEL_X, cy + CFG_PANEL_Y,
             CFG_PANEL_W, CFG_PANEL_H, COLOR_PANEL_BG, CFG_OUTLINE_THICKNESS);
#endif

    // Draw right panel outline.
#if CFG_SHOW_RIGHT_PANEL
    if (CFG_RIGHT_PANEL_OUTLINE_THICKNESS > 0) {
      int rpx = cx + L.rpanel_x;
      int rpy = cy + L.rpanel_y;
      DrawRect(i, rpx, rpy,
               L.rpanel_w, L.rpanel_h,
               CFG_RIGHT_PANEL_OUTLINE_COLOR, CFG_RIGHT_PANEL_OUTLINE_THICKNESS);
      // Pentagon outline sitting on top of right panel outline.
      XPoint rp_pent[6] = {
        { rpx + L.rpanel_w / 30,  rpy - L.rpanel_h / 5 },
        { rpx + L.rpanel_w,       rpy - L.rpanel_h / 5 },
        { rpx + L.rpanel_w,       rpy                   },
        { rpx,                    rpy                   },
        { rpx,                    rpy - L.rpanel_h / 10 },
        { rpx + L.rpanel_w / 30,  rpy - L.rpanel_h / 5 },
      };
      XDrawLines(display, backbuf[i],
                 gcs_all[COLOR_CYBER_DIM][i],
                 rp_pent, 6, CoordModeOrigin);
    }
#endif

    // Draw all sections to backbuffer with adjusted positions.
    DrawBufferSection(i, cx + L.buffer_x, cy + L.buffer_y + L.th, L.buf_cw, L.buf_ch, gs);
#if CFG_SHOW_MATRIX
    DrawCodeMatrix(i, cx + L.matrix_x, cy + L.matrix_y + L.th, L.grid_cw, L.grid_ch, gs);
#endif
#if CFG_SHOW_TIMER
    DrawTimerText(i, cx + L.timer_x, cy + L.timer_y, L.timer_w, L.timer_h, csec_remaining);
#endif
#if CFG_SHOW_BAR
    DrawProgressBar(i, cx + L.bar_x, cy + L.bar_y, L.bar_w, L.bar_h,
                    csec_remaining, csec_total);
#endif
#if CFG_SHOW_SEQUENCES
    DrawSequenceSection(i, cx + L.seq_x, cy + L.seq_y, L.seq_cw, L.seq_ch, gs);
#endif
    // Blit backbuffer to window atomically.
    XCopyArea(display, backbuf[i], windows[i],
              gcs_all[COLOR_FOREGROUND][i],
              0, 0, backbuf_w[i], backbuf_h[i], 0, 0);
  }

  XFlush(display);
}

/*! \brief Redraw only the timer section (called every 10ms tick).
 *
 * Redraws just the timer area on the backbuffer, then blits that rect.
 */
void RedrawTimerOnly(int csec_remaining, int csec_total) {
  LayoutInfo L;
  ComputeLayout(&L);

  for (size_t i = 0; i < num_windows; ++i) {
    // Center content on the monitor (same as DisplayBreachProtocolFull).
    int cx = (backbuf_w[i] - L.region_w) / 2;
    int cy = (backbuf_h[i] - L.region_h) / 2;

#if CFG_SHOW_TIMER
    int timer_x = cx + L.timer_x;
    int timer_y = cy + L.timer_y;
#endif
#if CFG_SHOW_BAR
    int bar_x = cx + L.bar_x;
    int bar_y = cy + L.bar_y;
#endif

#if CFG_SHOW_TIMER
    // Clear and redraw timer text (use timer_w, not bar_w, to avoid
    // wiping other elements that share the same x range).
    FillRect(i, timer_x, timer_y, L.timer_w, L.timer_h, COLOR_CONTENT_BG);
    DrawTimerText(i, timer_x, timer_y, L.timer_w, L.timer_h, csec_remaining);
    XCopyArea(display, backbuf[i], windows[i],
              gcs_all[COLOR_FOREGROUND][i],
              timer_x, timer_y, L.timer_w, L.timer_h,
              timer_x, timer_y);
#endif

#if CFG_SHOW_BAR
    // Clear and redraw progress bar.
    FillRect(i, bar_x, bar_y, L.bar_w, L.bar_h, COLOR_CONTENT_BG);
    DrawProgressBar(i, bar_x, bar_y, L.bar_w, L.bar_h,
                    csec_remaining, csec_total);
    XCopyArea(display, backbuf[i], windows[i],
              gcs_all[COLOR_FOREGROUND][i],
              bar_x, bar_y, L.bar_w, L.bar_h,
              bar_x, bar_y);
#endif
  }

  XFlush(display);
}

/*! \brief Display a simple text message (fallback for non-grid states).
 */
void DisplayMessage(const char *title, const char *str, int is_warning) {
  char full_title[256];
  BuildTitle(full_title, sizeof(full_title), title);

  int th = TextAscent() + TextDescent() + CFG_LINE_SPACING;
  int to = TextAscent() + CFG_LINE_SPACING / 2;

  int len_full_title = strlen(full_title);
  int tw_full_title = TextWidth(full_title, len_full_title);

  int len_str = strlen(str);
  int tw_str = TextWidth(str, len_str);

  int box_w = tw_full_title;
  if (box_w < tw_str) {
    box_w = tw_str;
  }
  int box_h = 4 * th;
  int region_w = box_w;
  int region_h = box_h;

  UpdatePerMonitorWindows(per_monitor_windows_dirty, region_w, region_h,
                          x_offset, y_offset);
  per_monitor_windows_dirty = 0;

  enum DrawColor color = is_warning ? COLOR_WARNING : COLOR_FOREGROUND;

  for (size_t i = 0; i < num_windows; ++i) {
    int cx = region_w / 2;
    int cy = region_h / 2;
    int y = cy + to - box_h / 2;

    // Clear backbuffer.
    FillRect(i, 0, 0, backbuf_w[i], backbuf_h[i], COLOR_BACKGROUND);

    DrawString(i, cx - tw_full_title / 2, y, color, full_title,
               len_full_title);
    y += th * 2;

    DrawString(i, cx - tw_str / 2, y, color, str, len_str);

    // Blit backbuffer to window.
    XCopyArea(display, backbuf[i], windows[i],
              gcs_all[COLOR_FOREGROUND][i],
              0, 0, backbuf_w[i], backbuf_h[i], 0, 0);
  }

  XFlush(display);
}

//! The size of the buffer to store the password in. Not NUL terminated.
#define PWBUF_SIZE 256

//! The size of the buffer to use for display, with space for cursor and NUL.
#define DISPLAYBUF_SIZE (PWBUF_SIZE + 2)

/*! \brief Ask a question to the user.
 *
 * \param msg The message.
 * \param response The response will be stored in a newly allocated buffer here.
 * \param echo If true, the input will be shown; otherwise it will be hidden.
 * \return 1 if successful, anything else otherwise.
 */
int Prompt(const char *msg, char **response, int echo) {
  struct {
    // The received X11 event.
    XEvent ev;

    // Input buffer. Not NUL-terminated.
    char pwbuf[PWBUF_SIZE];
    // Current input length.
    size_t pwlen;

    // Display buffer (used for echo mode).
    char displaybuf[DISPLAYBUF_SIZE];
    // Display buffer length.
    size_t displaylen;

    // Character read buffer.
    char inputbuf;

    // Grid state for breach protocol visualization.
    GridState grid;

    // Temporary position variables.
    size_t prevpos;
    size_t pos;
    int len;
  } priv;

  if (!echo && MLOCK_PAGE(&priv, sizeof(priv)) < 0) {
    LogErrno("mlock");
    DisplayMessage(CFG_TEXT_ERROR, CFG_TEXT_MLOCK_WARN, 1);
    WaitForKeypress(1);
  }

  priv.pwlen = 0;
  InitGridState(&priv.grid);

  struct timeval deadline_tv;
  gettimeofday(&deadline_tv, NULL);
  deadline_tv.tv_sec += prompt_timeout;

  int csec_total = prompt_timeout * 100;
  if (csec_total > CFG_TIMER_MAX_CSEC) csec_total = CFG_TIMER_MAX_CSEC;

  int status = 0;
  int done = 0;
  int played_sound = 0;
  int need_full_redraw = 1;

  while (!done) {
    struct timeval now_tv;
    gettimeofday(&now_tv, NULL);
    int csec_remaining = ComputeCentisecondsRemaining(&deadline_tv, &now_tv);

    if (echo) {
      // Echo mode: only redraw on input (no timer to update).
      if (need_full_redraw) {
        if (priv.pwlen != 0) {
          memcpy(priv.displaybuf, priv.pwbuf, priv.pwlen);
        }
        priv.displaylen = priv.pwlen;
        priv.displaybuf[priv.displaylen] = '_';
        priv.displaybuf[priv.displaylen + 1] = '\0';
        DisplayMessage(msg, priv.displaybuf, 0);
        need_full_redraw = 0;
      }
    } else {
      // Password mode: full redraw on input, timer-only on tick.
      if (need_full_redraw) {
        DisplayBreachProtocolFull(&priv.grid, csec_remaining, csec_total);
        need_full_redraw = 0;
      } else {
        RedrawTimerOnly(csec_remaining, csec_total);
      }
    }

    if (!played_sound) {
      PlaySound(SOUND_PROMPT);
      played_sound = 1;
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = CFG_TIMER_INTERVAL_US;

    while (!done) {
      fd_set set;
      memset(&set, 0, sizeof(set));
      FD_ZERO(&set);
      FD_SET(0, &set);
      int nfds = select(1, &set, NULL, NULL, &timeout);
      if (nfds < 0) {
        LogErrno("select");
        done = 1;
        break;
      }

      gettimeofday(&now_tv, NULL);
      if (now_tv.tv_sec > deadline_tv.tv_sec ||
          (now_tv.tv_sec == deadline_tv.tv_sec &&
           now_tv.tv_usec >= deadline_tv.tv_usec)) {
        Log("AUTH_TIMEOUT hit");
        done = 1;
        break;
      }
      if (nfds == 0) {
        // Timer tick — break to outer loop for timer redraw.
        break;
      }

      // Input available — nonblocking drain from now on.
      timeout.tv_usec = 0;

      // Reset the prompt timeout on input.
      gettimeofday(&deadline_tv, NULL);
      deadline_tv.tv_sec += prompt_timeout;

      ssize_t nread = read(0, &priv.inputbuf, 1);
      if (nread <= 0) {
        Log("EOF on password input - bailing out");
        done = 1;
        break;
      }
      switch (priv.inputbuf) {
        case '\b':      // Backspace.
        case '\177': {  // Delete.
          // Backwards skip with multibyte support.
          mblen(NULL, 0);
          priv.pos = priv.prevpos = 0;
          while (priv.pos < priv.pwlen) {
            priv.prevpos = priv.pos;
            priv.len = mblen(priv.pwbuf + priv.pos, priv.pwlen - priv.pos);
            if (priv.len <= 0) {
              break;
            }
            priv.pos += priv.len;
          }
          priv.pwlen = priv.prevpos;
          if (!echo) {
            GridRewindStep(&priv.grid);
          }
          need_full_redraw = 1;
          break;
        }
        case '\001':  // Ctrl-A.
          priv.pwlen = 0;
          if (!echo) {
            InitGridState(&priv.grid);
          }
          need_full_redraw = 1;
          break;
        case '\023':  // Ctrl-S.
          SwitchKeyboardLayout();
          need_full_redraw = 1;
          break;
        case '\025':  // Ctrl-U.
          priv.pwlen = 0;
          if (!echo) {
            InitGridState(&priv.grid);
          }
          need_full_redraw = 1;
          break;
        case 0:       // Shouldn't happen.
        case '\033':  // Escape.
          done = 1;
          break;
        case '\r':  // Return.
        case '\n':  // Return.
          *response = malloc(priv.pwlen + 1);
          if (!echo && MLOCK_PAGE(*response, priv.pwlen + 1) < 0) {
            LogErrno("mlock");
            DisplayMessage(CFG_TEXT_ERROR, CFG_TEXT_MLOCK_ERR, 1);
            WaitForKeypress(1);
          }
          if (priv.pwlen != 0) {
            memcpy(*response, priv.pwbuf, priv.pwlen);
          }
          (*response)[priv.pwlen] = 0;
          status = 1;
          done = 1;
          break;
        default:
          if (priv.inputbuf >= '\000' && priv.inputbuf <= '\037') {
            break;
          }
          if (priv.pwlen < sizeof(priv.pwbuf)) {
            priv.pwbuf[priv.pwlen] = priv.inputbuf;
            ++priv.pwlen;
            if (!echo) {
              GridAdvanceStep(&priv.grid);
            }
            need_full_redraw = 1;
          } else {
            Log("Password entered is too long - bailing out");
            done = 1;
            break;
          }
          break;
      }
    }

    // Handle X11 events that queued up.
    while (!done && XPending(display) && (XNextEvent(display, &priv.ev), 1)) {
      if (IsMonitorChangeEvent(display, priv.ev.type)) {
        per_monitor_windows_dirty = 1;
        need_full_redraw = 1;
      }
    }
  }

  // priv contains password related data, so better clear it.
  memset(&priv, 0, sizeof(priv));

  if (!done) {
    Log("Unreachable code - the loop above must set done");
  }
  return status;
}

/*! \brief The main program.
 *
 * Usage: XSCREENSAVER_WINDOW=window_id ./auth_x11_grid; status=$?
 *
 * \return 0 if authentication successful, anything else otherwise.
 */
int main(int argc_local, char **argv_local) {
  argc = argc_local;
  argv = argv_local;

  setlocale(LC_CTYPE, "");
  setlocale(LC_TIME, "");

  struct timeval tv;
  gettimeofday(&tv, NULL);
  srand(tv.tv_sec ^ tv.tv_usec ^ getpid());

  authproto_executable = GetExecutablePathSetting("XSECURELOCK_AUTHPROTO",
                                                  AUTHPROTO_EXECUTABLE, 0);

  burnin_mitigation_max_offset =
      GetIntSetting("XSECURELOCK_BURNIN_MITIGATION", 16);
  if (burnin_mitigation_max_offset > 0) {
    x_offset = rand() % (2 * burnin_mitigation_max_offset + 1) -
               burnin_mitigation_max_offset;
    y_offset = rand() % (2 * burnin_mitigation_max_offset + 1) -
               burnin_mitigation_max_offset;
  }

  burnin_mitigation_max_offset_change =
      GetIntSetting("XSECURELOCK_BURNIN_MITIGATION_DYNAMIC", 0);

  prompt_timeout = GetIntSetting("XSECURELOCK_AUTH_TIMEOUT", CFG_DEFAULT_TIMEOUT);
  show_username = GetIntSetting("XSECURELOCK_SHOW_USERNAME", 1);
  show_hostname = GetIntSetting("XSECURELOCK_SHOW_HOSTNAME", 1);
  have_switch_user_command =
      !!*GetStringSetting("XSECURELOCK_SWITCH_USER_COMMAND", "");
  auth_sounds = GetIntSetting("XSECURELOCK_AUTH_SOUNDS", 0);
  single_auth_window = GetIntSetting("XSECURELOCK_SINGLE_AUTH_WINDOW", 0);
#ifdef HAVE_XKB_EXT
  show_keyboard_layout =
      GetIntSetting("XSECURELOCK_SHOW_KEYBOARD_LAYOUT", 1);
  show_locks_and_latches =
      GetIntSetting("XSECURELOCK_SHOW_LOCKS_AND_LATCHES", 0);
#endif

  if ((display = XOpenDisplay(NULL)) == NULL) {
    Log("Could not connect to $DISPLAY");
    return 1;
  }

#ifdef HAVE_XKB_EXT
  int xkb_opcode, xkb_event_base, xkb_error_base;
  int xkb_major_version = XkbMajorVersion, xkb_minor_version = XkbMinorVersion;
  have_xkb_ext =
      XkbQueryExtension(display, &xkb_opcode, &xkb_event_base, &xkb_error_base,
                        &xkb_major_version, &xkb_minor_version);
#endif

  if (!GetHostName(hostname, sizeof(hostname))) {
    return 1;
  }
  if (!GetUserName(username, sizeof(username))) {
    return 1;
  }

  main_window = ReadWindowID();
  if (main_window == None) {
    Log("Invalid/no window ID in XSCREENSAVER_WINDOW");
    return 1;
  }
  Window unused_root;
  Window *unused_children = NULL;
  unsigned int unused_nchildren;
  XQueryTree(display, main_window, &unused_root, &parent_window,
             &unused_children, &unused_nchildren);
  XFree(unused_children);

  Colormap colormap = DefaultColormap(display, DefaultScreen(display));

  // Allocate colors.
  XColor dummy;

  XAllocNamedColor(display, colormap,
                   GetStringSetting("XSECURELOCK_AUTH_BACKGROUND_COLOR",
                                   CFG_COLOR_BACKGROUND),
                   &xcolor_background, &dummy);

  // COLOR_FOREGROUND — white (used for text on highlighted backgrounds).
  XAllocNamedColor(display, colormap,
                   GetStringSetting("XSECURELOCK_AUTH_FOREGROUND_COLOR",
                                   CFG_COLOR_FOREGROUND),
                   &xcolors[COLOR_FOREGROUND], &dummy);

  // COLOR_WARNING — red for warnings.
  XAllocNamedColor(display, colormap,
                   GetStringSetting("XSECURELOCK_AUTH_WARNING_COLOR",
                                   CFG_COLOR_WARNING),
                   &xcolors[COLOR_WARNING], &dummy);

  // Cyber colors — from configuration constants.
  XAllocNamedColor(display, colormap, CFG_COLOR_CYBER_GREEN,
                   &xcolors[COLOR_CYBER_GREEN], &dummy);
  XAllocNamedColor(display, colormap, CFG_COLOR_CYBER_DIM,
                   &xcolors[COLOR_CYBER_DIM], &dummy);
  XAllocNamedColor(display, colormap, CFG_COLOR_CYBER_YELLOW,
                   &xcolors[COLOR_CYBER_YELLOW], &dummy);
  XAllocNamedColor(display, colormap, CFG_COLOR_CYBER_HIGHLIGHT,
                   &xcolors[COLOR_CYBER_HIGHLIGHT], &dummy);
  XAllocNamedColor(display, colormap, CFG_COLOR_CYBER_RED,
                   &xcolors[COLOR_CYBER_RED], &dummy);
  XAllocNamedColor(display, colormap, CFG_COLOR_CYBER_COMPLETE,
                   &xcolors[COLOR_CYBER_COMPLETE], &dummy);

  // COLOR_BACKGROUND — same as xcolor_background, used for backbuffer fills.
  xcolors[COLOR_BACKGROUND] = xcolor_background;

  XAllocNamedColor(display, colormap, CFG_COLOR_CONTENT_BG,
                   &xcolors[COLOR_CONTENT_BG], &dummy);

  XAllocNamedColor(display, colormap, CFG_COLOR_PANEL_BG,
                   &xcolors[COLOR_PANEL_BG], &dummy);

  core_font = NULL;
#ifdef HAVE_XFT_EXT
  xft_font = NULL;
#endif

  const char *font_name = GetStringSetting("XSECURELOCK_FONT", "");

  int have_font = 0;
  if (font_name[0] != 0) {
    core_font = XLoadQueryFont(display, font_name);
    have_font = (core_font != NULL);
#ifdef HAVE_XFT_EXT
    if (!have_font) {
      xft_font =
          FixedXftFontOpenName(display, DefaultScreen(display), font_name);
      have_font = (xft_font != NULL);
    }
#endif
  }
  if (!have_font) {
    if (font_name[0] != 0) {
      Log("Could not load the specified font %s - trying a default font",
          font_name);
    }
#ifdef HAVE_XFT_EXT
    xft_font =
        FixedXftFontOpenName(display, DefaultScreen(display), CFG_FONT_NAME);
    have_font = (xft_font != NULL);
#endif
  }
  if (!have_font) {
    core_font = XLoadQueryFont(display, CFG_FONT_CORE);
    have_font = (core_font != NULL);
  }
  if (!have_font) {
    Log("Could not load a mind-bogglingly stupid font");
    return 1;
  }

#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    XRenderColor xrcolor;
    xrcolor.alpha = 65535;

    for (int c = 0; c < COLOR_COUNT; ++c) {
      xrcolor.red = xcolors[c].red;
      xrcolor.green = xcolors[c].green;
      xrcolor.blue = xcolors[c].blue;
      XftColorAllocValue(display,
                         DefaultVisual(display, DefaultScreen(display)),
                         colormap, &xrcolor, &xft_colors[c]);
    }
  }
#endif

  SelectMonitorChangeEvents(display, main_window);

  InitWaitPgrp();

  int status = Authenticate();

  // Clear any possible processing message by closing our windows.
  DestroyPerMonitorWindows(0);

#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    for (int c = 0; c < COLOR_COUNT; ++c) {
      XftColorFree(display, DefaultVisual(display, DefaultScreen(display)),
                   colormap, &xft_colors[c]);
    }
    XftFontClose(display, xft_font);
  }
#endif

  for (int c = 0; c < COLOR_COUNT; ++c) {
    XFreeColors(display, colormap, &xcolors[c].pixel, 1, 0);
  }
  XFreeColors(display, colormap, &xcolor_background.pixel, 1, 0);

  return status;
}
