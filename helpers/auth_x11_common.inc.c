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
 * \file auth_x11_common.inc.c
 * \brief Shared code between auth_x11.c and auth_x11_grid.c.
 *
 * This file is textually #include'd into each auth module's .c file.
 * It is NOT compiled independently.
 *
 * The functions here reference file-scope globals (display, core_font,
 * xft_font, sounds[], etc.) that must be defined in the including
 * translation unit before the #include point.
 *
 * The including file must also define:
 *   void DisplayMessage(const char *, const char *, int);
 *   int Prompt(const char *, char **, int);
 * since Authenticate() calls them, and they differ between auth modules.
 */

#ifndef AUTH_X11_COMMON_INC_C_
#define AUTH_X11_COMMON_INC_C_

/* Forward declarations for functions defined differently in each auth module. */
void DisplayMessage(const char *title, const char *str, int is_warning);
int Prompt(const char *msg, char **response, int echo);

/*! \brief Play a sound sequence.
 */
void PlaySound(enum Sound snd) {
  XKeyboardState state;
  XKeyboardControl control;
  struct timespec sleeptime;

  if (!auth_sounds) {
    return;
  }

  XGetKeyboardControl(display, &state);

  control.bell_percent = 50;
  control.bell_duration = SOUND_TONE_MS;
  control.bell_pitch = sounds[snd][0];
  XChangeKeyboardControl(display, KBBellPercent | KBBellDuration | KBBellPitch,
                         &control);
  XBell(display, 0);

  XFlush(display);

  sleeptime.tv_sec = SOUND_SLEEP_MS / 1000;
  sleeptime.tv_nsec = 1000000L * (SOUND_SLEEP_MS % 1000);
  nanosleep(&sleeptime, NULL);

  control.bell_pitch = sounds[snd][1];
  XChangeKeyboardControl(display, KBBellPitch, &control);
  XBell(display, 0);

  control.bell_percent = state.bell_percent;
  control.bell_duration = state.bell_duration;
  control.bell_pitch = state.bell_pitch;
  XChangeKeyboardControl(display, KBBellPercent | KBBellDuration | KBBellPitch,
                         &control);

  XFlush(display);

  nanosleep(&sleeptime, NULL);
}

/*! \brief Switch to the next keyboard layout.
 */
void SwitchKeyboardLayout(void) {
#ifdef HAVE_XKB_EXT
  if (!have_xkb_ext) {
    return;
  }

  XkbDescPtr xkb;
  xkb = XkbGetMap(display, 0, XkbUseCoreKbd);
  if (XkbGetControls(display, XkbGroupsWrapMask, xkb) != Success) {
    Log("XkbGetControls failed");
    XkbFreeKeyboard(xkb, 0, True);
    return;
  }
  if (xkb->ctrls->num_groups < 1) {
    Log("XkbGetControls returned less than 1 group");
    XkbFreeKeyboard(xkb, 0, True);
    return;
  }
  XkbStateRec state;
  if (XkbGetState(display, XkbUseCoreKbd, &state) != Success) {
    Log("XkbGetState failed");
    XkbFreeKeyboard(xkb, 0, True);
    return;
  }

  XkbLockGroup(display, XkbUseCoreKbd,
               (state.group + 1) % xkb->ctrls->num_groups);

  XkbFreeKeyboard(xkb, 0, True);
#endif
}

/*! \brief Check which modifiers are active.
 */
const char *GetIndicators(int *warning, int *have_multiple_layouts) {
#ifdef HAVE_XKB_EXT
  static char buf[128];
  char *p;

  if (!have_xkb_ext) {
    return "";
  }

  XkbDescPtr xkb;
  xkb = XkbGetMap(display, 0, XkbUseCoreKbd);
  if (XkbGetControls(display, XkbGroupsWrapMask, xkb) != Success) {
    Log("XkbGetControls failed");
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  if (XkbGetNames(
          display,
          XkbIndicatorNamesMask | XkbGroupNamesMask | XkbSymbolsNameMask,
          xkb) != Success) {
    Log("XkbGetNames failed");
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  XkbStateRec state;
  if (XkbGetState(display, XkbUseCoreKbd, &state) != Success) {
    Log("XkbGetState failed");
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  unsigned int istate = 0;
  if (!show_locks_and_latches) {
    if (XkbGetIndicatorState(display, XkbUseCoreKbd, &istate) != Success) {
      Log("XkbGetIndicatorState failed");
      XkbFreeKeyboard(xkb, 0, True);
      return "";
    }
  }

  unsigned int implicit_mods = state.latched_mods | state.locked_mods;
  if (implicit_mods & LockMask) {
    *warning = 1;
  }

  if (xkb->ctrls->num_groups > 1) {
    *have_multiple_layouts = 1;
  }

  p = buf;

  const char *word = "Keyboard: ";
  size_t n = strlen(word);
  if (n >= sizeof(buf) - (p - buf)) {
    Log("Not enough space to store intro '%s'", word);
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  memcpy(p, word, n);
  p += n;

  int have_output = 0;
  if (show_keyboard_layout) {
    Atom layouta = xkb->names->groups[state.group];
    if (layouta == None) {
      layouta = xkb->names->symbols;
    }
    if (layouta != None) {
      char *layout = XGetAtomName(display, layouta);
      n = strlen(layout);
      if (n >= sizeof(buf) - (p - buf)) {
        Log("Not enough space to store layout name '%s'", layout);
        XFree(layout);
        XkbFreeKeyboard(xkb, 0, True);
        return "";
      }
      memcpy(p, layout, n);
      XFree(layout);
      p += n;
      have_output = 1;
    }
  }

  if (show_locks_and_latches) {
#define ADD_INDICATOR(mask, name)                                \
  do {                                                           \
    if (!(implicit_mods & (mask))) {                             \
      continue;                                                  \
    }                                                            \
    if (have_output) {                                           \
      if (2 >= sizeof(buf) - (p - buf)) {                        \
        Log("Not enough space to store another modifier name");  \
        break;                                                   \
      }                                                          \
      memcpy(p, ", ", 2);                                        \
      p += 2;                                                    \
    }                                                            \
    size_t n = strlen(name);                                     \
    if (n >= sizeof(buf) - (p - buf)) {                          \
      Log("Not enough space to store modifier name '%s'", name); \
      XFree(name);                                               \
      break;                                                     \
    }                                                            \
    memcpy(p, (name), n);                                        \
    p += n;                                                      \
    have_output = 1;                                             \
  } while (0)
    ADD_INDICATOR(ShiftMask, "Shift");
    ADD_INDICATOR(LockMask, "Lock");
    ADD_INDICATOR(ControlMask, "Control");
    ADD_INDICATOR(Mod1Mask, "Mod1");
    ADD_INDICATOR(Mod2Mask, "Mod2");
    ADD_INDICATOR(Mod3Mask, "Mod3");
    ADD_INDICATOR(Mod4Mask, "Mod4");
    ADD_INDICATOR(Mod5Mask, "Mod5");
  } else {
    for (int i = 0; i < XkbNumIndicators; i++) {
      if (!(istate & (1U << i))) {
        continue;
      }
      Atom namea = xkb->names->indicators[i];
      if (namea == None) {
        continue;
      }
      if (have_output) {
        if (2 >= sizeof(buf) - (p - buf)) {
          Log("Not enough space to store another modifier name");
          break;
        }
        memcpy(p, ", ", 2);
        p += 2;
      }
      char *name = XGetAtomName(display, namea);
      size_t n = strlen(name);
      if (n >= sizeof(buf) - (p - buf)) {
        Log("Not enough space to store modifier name '%s'", name);
        XFree(name);
        break;
      }
      memcpy(p, name, n);
      XFree(name);
      p += n;
      have_output = 1;
    }
  }
  *p = 0;
  XkbFreeKeyboard(xkb, 0, True);
  return have_output ? buf : "";
#else
  *warning = *warning;                              // Shut up clang-analyzer.
  *have_multiple_layouts = *have_multiple_layouts;  // Shut up clang-analyzer.
  return "";
#endif
}

int TextAscent(void) {
#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    return xft_font->ascent;
  }
#endif
  return core_font->max_bounds.ascent;
}

int TextDescent(void) {
#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    return xft_font->descent;
  }
#endif
  return core_font->max_bounds.descent;
}

#ifdef HAVE_XFT_EXT
int XGlyphInfoExpandAmount(XGlyphInfo *extents) {
  int expand_left = extents->x;
  int expand_right = -extents->x + extents->width - extents->xOff;
  int expand_max = expand_left > expand_right ? expand_left : expand_right;
  int expand_positive = expand_max > 0 ? expand_max : 0;
  return expand_positive;
}
#endif

int TextWidth(const char *string, int len) {
#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    XGlyphInfo extents;
    XftTextExtentsUtf8(display, xft_font, (const FcChar8 *)string, len,
                       &extents);
    return extents.xOff + 2 * XGlyphInfoExpandAmount(&extents);
  }
#endif
  return XTextWidth(core_font, string, len);
}

void StrAppend(char **output, size_t *output_size, const char *input,
               size_t input_size) {
  if (*output_size <= input_size) {
    input_size = *output_size - 1;
  }
  memcpy(*output, input, input_size);
  *output += input_size;
  *output_size -= input_size;
}

void BuildTitle(char *output, size_t output_size, const char *input) {
  if (show_username) {
    size_t username_len = strlen(username);
    StrAppend(&output, &output_size, username, username_len);
  }

  if (show_username && show_hostname) {
    StrAppend(&output, &output_size, "@", 1);
  }

  if (show_hostname) {
    size_t hostname_len =
        show_hostname > 1 ? strlen(hostname) : strcspn(hostname, ".");
    StrAppend(&output, &output_size, hostname, hostname_len);
  }

  if (*input == 0) {
    *output = 0;
    return;
  }

  if (show_username || show_hostname) {
    StrAppend(&output, &output_size, " - ", 3);
  }
  strncpy(output, input, output_size - 1);
  output[output_size - 1] = 0;
}

void WaitForKeypress(int seconds) {
  struct timeval timeout;
  timeout.tv_sec = seconds;
  timeout.tv_usec = 0;
  fd_set set;
  memset(&set, 0, sizeof(set));
  FD_ZERO(&set);
  FD_SET(0, &set);
  select(1, &set, NULL, NULL, &timeout);
}

#ifdef HAVE_XFT_EXT
XftFont *FixedXftFontOpenName(Display *display, int screen,
                              const char *font_name) {
  XftFont *xft_font = XftFontOpenName(display, screen, font_name);
#ifdef HAVE_FONTCONFIG
  FcBool iscol;
  if (xft_font != NULL &&
      FcPatternGetBool(xft_font->pattern, FC_COLOR, 0, &iscol) && iscol) {
    Log("Colored font %s is not supported by Xft", font_name);
    XftFontClose(display, xft_font);
    return NULL;
  }
#else
#warning "Xft enabled without fontconfig. May crash trying to use emoji fonts."
  Log("Xft enabled without fontconfig. May crash trying to use emoji fonts.");
#endif
  return xft_font;
}
#endif

/*! \brief Perform authentication using a helper proxy.
 *
 * \return The authentication status (0 for OK, 1 otherwise).
 */
int Authenticate() {
  int requestfd[2], responsefd[2];
  if (pipe(requestfd)) {
    LogErrno("pipe");
    return 1;
  }
  if (pipe(responsefd)) {
    LogErrno("pipe");
    return 1;
  }

  pid_t childpid = ForkWithoutSigHandlers();
  if (childpid == -1) {
    LogErrno("fork");
    return 1;
  }

  if (childpid == 0) {
    close(requestfd[0]);
    close(responsefd[1]);

    if (requestfd[1] == 0) {
      int requestfd1 = dup(requestfd[1]);
      if (requestfd1 == -1) {
        LogErrno("dup");
        _exit(EXIT_FAILURE);
      }
      close(requestfd[1]);
      if (dup2(responsefd[0], 0) == -1) {
        LogErrno("dup2");
        _exit(EXIT_FAILURE);
      }
      close(responsefd[0]);
      if (requestfd1 != 1) {
        if (dup2(requestfd1, 1) == -1) {
          LogErrno("dup2");
          _exit(EXIT_FAILURE);
        }
        close(requestfd1);
      }
    } else {
      if (responsefd[0] != 0) {
        if (dup2(responsefd[0], 0) == -1) {
          LogErrno("dup2");
          _exit(EXIT_FAILURE);
        }
        close(responsefd[0]);
      }
      if (requestfd[1] != 1) {
        if (dup2(requestfd[1], 1) == -1) {
          LogErrno("dup2");
          _exit(EXIT_FAILURE);
        }
        close(requestfd[1]);
      }
    }
    {
      const char *args[2] = {authproto_executable, NULL};
      ExecvHelper(authproto_executable, args);
      sleep(2);
      _exit(EXIT_FAILURE);
    }
  }

  // Parent process.
  close(requestfd[1]);
  close(responsefd[0]);
  for (;;) {
    char *message;
    char *response;
    char type = ReadPacket(requestfd[0], &message, 1);
    switch (type) {
      case PTYPE_INFO_MESSAGE:
        DisplayMessage(CFG_TEXT_PAM_SAYS, message, 0);
        explicit_bzero(message, strlen(message));
        free(message);
        PlaySound(SOUND_INFO);
        WaitForKeypress(1);
        break;
      case PTYPE_ERROR_MESSAGE:
        DisplayMessage(CFG_TEXT_ERROR, message, 1);
        explicit_bzero(message, strlen(message));
        free(message);
        PlaySound(SOUND_ERROR);
        WaitForKeypress(1);
        break;
      case PTYPE_PROMPT_LIKE_USERNAME:
        if (Prompt(message, &response, 1)) {
          WritePacket(responsefd[1], PTYPE_RESPONSE_LIKE_USERNAME, response);
          explicit_bzero(response, strlen(response));
          free(response);
        } else {
          WritePacket(responsefd[1], PTYPE_RESPONSE_CANCELLED, "");
        }
        explicit_bzero(message, strlen(message));
        free(message);
        DisplayMessage(CFG_TEXT_PROCESSING, "", 0);
        break;
      case PTYPE_PROMPT_LIKE_PASSWORD:
        if (Prompt(message, &response, 0)) {
          WritePacket(responsefd[1], PTYPE_RESPONSE_LIKE_PASSWORD, response);
          explicit_bzero(response, strlen(response));
          free(response);
        } else {
          WritePacket(responsefd[1], PTYPE_RESPONSE_CANCELLED, "");
        }
        explicit_bzero(message, strlen(message));
        free(message);
        DisplayMessage(CFG_TEXT_PROCESSING, "", 0);
        break;
      case 0:
        goto done;
      default:
        Log("Unknown message type %02x", (int)type);
        explicit_bzero(message, strlen(message));
        free(message);
        goto done;
    }
  }
done:
  close(requestfd[0]);
  close(responsefd[1]);
  int status;
  if (!WaitProc("authproto", &childpid, 1, 0, &status)) {
    Log("WaitPgrp returned false but we were blocking");
    abort();
  }
  if (status == 0) {
    PlaySound(SOUND_SUCCESS);
  }
  return status != 0;
}

#endif /* AUTH_X11_COMMON_INC_C_ */
