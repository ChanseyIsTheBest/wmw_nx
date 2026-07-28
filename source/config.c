/* config.c -- runtime screen size and locale
 *
 * Where's My Water? (com.disney.WMW) Switch port.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <switch.h>
#include <string.h>

#include <stdio.h>
#include <ctype.h>

#include "config.h"
#include "util.h"
#include "wmw_tate.h"
#include "wmw_paths.h"

// The landscape swapchain / touch panel space. Locked in both handheld and
// docked so the presentation never changes underneath the engine.
int screen_width  = 1280;
int screen_height = 720;

// The portrait space the engine is told it has. Rotated 90 degrees this lands
// exactly on the panel above, 1:1.
int render_width  = 720;
int render_height = 1280;

// ---------------------------------------------------------------------------
// locale
// ---------------------------------------------------------------------------
//
// The engine asks BaseActivity for getLanguageCode() and getCountryCode() and
// uses them to pick its localized string table and localized art. libwmw's
// .rodata shows it special-cases "zh-Hant" / "zh-Hans" against country "HK",
// so Traditional/Simplified Chinese are selected by the pair, not the language
// alone. Everything else is a plain two-letter code.

static char s_lang[8]    = "en";
static char s_country[8] = "US";
static int  s_resolved   = 0;

static void resolve_locale(void) {
  if (s_resolved) return;
  s_resolved = 1;

  u64 lang_code = 0;
  SetLanguage lang = SetLanguage_ENUS;
  if (R_FAILED(setGetSystemLanguage(&lang_code)) ||
      R_FAILED(setMakeLanguage(lang_code, &lang)))
    return; // keep the en/US default

  switch (lang) {
    case SetLanguage_JA:     strcpy(s_lang, "ja"); strcpy(s_country, "JP"); break;
    case SetLanguage_FR:
    case SetLanguage_FRCA:   strcpy(s_lang, "fr"); strcpy(s_country, "FR"); break;
    case SetLanguage_DE:     strcpy(s_lang, "de"); strcpy(s_country, "DE"); break;
    case SetLanguage_IT:     strcpy(s_lang, "it"); strcpy(s_country, "IT"); break;
    case SetLanguage_ES:
    case SetLanguage_ES419:  strcpy(s_lang, "es"); strcpy(s_country, "ES"); break;
    case SetLanguage_PT:
    case SetLanguage_PTBR:   strcpy(s_lang, "pt"); strcpy(s_country, "BR"); break;
    case SetLanguage_RU:     strcpy(s_lang, "ru"); strcpy(s_country, "RU"); break;
    case SetLanguage_KO:     strcpy(s_lang, "ko"); strcpy(s_country, "KR"); break;
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS: strcpy(s_lang, "zh-Hans"); strcpy(s_country, "CN"); break;
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANT: strcpy(s_lang, "zh-Hant"); strcpy(s_country, "HK"); break;
    case SetLanguage_NL:     strcpy(s_lang, "nl"); strcpy(s_country, "NL"); break;
    case SetLanguage_ENGB:   strcpy(s_lang, "en"); strcpy(s_country, "GB"); break;
    default:                 strcpy(s_lang, "en"); strcpy(s_country, "US"); break;
  }
  debugPrintf("locale: %s / %s\n", s_lang, s_country);
}

const char *wmw_language_code(void) { resolve_locale(); return s_lang; }
const char *wmw_country_code(void)  { resolve_locale(); return s_country; }

// ---------------------------------------------------------------------------
// config.txt
// ---------------------------------------------------------------------------
//
// Optional, sits next to the .nro. Currently one key:
//
//     rotation = 1     (default) 90 CW  -- right Joy-Con up
//     rotation = 2               90 CCW -- left Joy-Con up
//
// Which way feels right depends on how you like to hold the console; there is
// no correct answer, so it is a setting rather than a guess.

static int s_rotation = -1;

static void trim(char *s) {
  char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static const char *const k_default_config =
  "# Where's My Water? -- Switch port configuration\n"
  "#\n"
  "# This file was written automatically on first launch. Edit it and relaunch.\n"
  "\n"
  "# rotation -- which way the portrait image is turned onto the panel.\n"
  "#\n"
  "# The game is portrait, so you hold the console sideways like a phone. Which\n"
  "# way you turn it is a matter of preference and of which Joy-Con you want\n"
  "# under your thumb.\n"
  "#\n"
  "#   1   90 degrees clockwise -- RIGHT Joy-Con up   (default)\n"
  "#   2   90 degrees counter-clockwise -- LEFT Joy-Con up\n"
  "#\n"
  "# Touch, the on-screen cursor, the stick, a USB mouse and gyro pointing all\n"
  "# follow this setting together, so they cannot disagree with the picture.\n"
  "\n"
  "rotation = 1\n";

static void write_default_config(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) return;
  fputs(k_default_config, f);
  fclose(f);
  debugPrintf("config: wrote a default %s\n", path);
}

int wmw_rotation_mode(void) {
  if (s_rotation >= 0)
    return s_rotation;

  s_rotation = WMW_TATE_CW; // default: rotated, fullscreen

  char path[512];
  snprintf(path, sizeof(path), "%s/config.txt", wmw_game_dir());
  FILE *f = fopen(path, "r");
  if (!f) {
    // Write a documented one so the setting is discoverable without the README.
    write_default_config(path);
    debugPrintf("config: rotation = 1 (90 CW, right Joy-Con up) [default]\n");
    return s_rotation;
  }

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq || line[0] == '#') continue;
    *eq = '\0';
    char key[64], val[64];
    snprintf(key, sizeof(key), "%s", line);
    snprintf(val, sizeof(val), "%s", eq + 1);
    trim(key); trim(val);
    if (strcmp(key, "rotation") != 0) continue;

    // Numeric, and deliberately the same values as the WMW_TATE_* enum and as
    // NxpConfig.rotation -- so the setting, the display transform and the input
    // transform are all literally the same number and cannot be mismatched in
    // translation.
    //
    // Only 1 and 2 are offered. The game is portrait and the point of the port
    // is to play it that way; an unrotated pillarboxed mode wastes most of the
    // panel. Anything else, including the old cw/ccw spellings and any stray
    // value, resolves to 1.
    s_rotation = (val[0] == '2' || !strcasecmp(val, "ccw")) ? WMW_TATE_CCW
                                                           : WMW_TATE_CW;

    debugPrintf("config: rotation = %d (%s)\n", s_rotation,
                s_rotation == WMW_TATE_CCW ? "90 CCW, left Joy-Con up"
                                           : "90 CW, right Joy-Con up");
  }
  fclose(f);
  return s_rotation;
}
