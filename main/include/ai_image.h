/**
 * @file ai_image.h
 * @brief AI image generation module (Pollinations.AI)
 */
#ifndef AI_IMAGE_H
#define AI_IMAGE_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize AI image module
 */
void ai_image_init(void);

/**
 * @brief Generate weather-based AI image
 * @param weather_text Weather description
 * @param temperature Temperature in Celsius
 * @param festival Optional festival name (can be NULL)
 * @return true if generation was successful, image saved to SPIFFS
 */
bool ai_image_generate(const char* weather_text, int temperature, const char* festival);

/**
 * @brief Check if AI image exists in storage
 * @return true if image file exists
 */
bool ai_image_exists(void);

/**
 * @brief Get AI image file path
 * @return Path to AI image in SPIFFS
 */
const char* ai_image_get_path(void);

/**
 * @brief Delete AI image
 */
void ai_image_delete(void);

/**
 * @brief Build prompt from weather and festival
 * @param weather_text Weather description
 * @param temperature Temperature
 * @param festival Festival name (can be NULL)
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 */
void ai_build_prompt(const char* weather_text, int temperature, 
                    const char* festival, char* buffer, size_t buffer_size);

/**
 * @brief Get current festival or season name
 * @return Festival name if today is a holiday, season name otherwise, NULL if no match
 */
const char* ai_get_current_festival(void);

#endif // AI_IMAGE_H
