/**
 * @file font_chars.h
 * @brief Character set of lv_font_simsun_16_cjk for runtime fallback checking
 *
 * This file is auto-generated from lv_font_simsun_16_cjk.c.
 * Total: 1097 Chinese chars + 77 Hiragana + 73 Katakana
 *
 * Usage: font_supports_chinese("多云") checks if ALL chars in the string
 *        are present in the font. If not, use English fallback.
 */

#ifndef FONT_CHARS_H
#define FONT_CHARS_H

#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if a UTF-8 string contains only characters supported by
 *        lv_font_simsun_16_cjk.
 * @param str UTF-8 encoded string
 * @return true if all characters are supported, false otherwise
 */
bool font_supports_chinese(const char *str);

#ifdef __cplusplus
}
#endif

#endif // FONT_CHARS_H
