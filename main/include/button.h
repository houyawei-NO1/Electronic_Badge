/**
 * @file button.h
 * @brief Button handling module
 */
#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include "driver/gpio.h"

// Button event types
typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_PRESS,       // Short press
    BUTTON_EVENT_LONG_PRESS,  // Long press (> defined threshold)
    BUTTON_EVENT_RELEASE,
} button_event_t;

// Button IDs
typedef enum {
    BUTTON_WAKE = 0,
    BUTTON_REFRESH,
    BUTTON_MAX
} button_id_t;

// Button configuration
typedef struct {
    gpio_num_t gpio;
    uint32_t long_press_ms;   // Time to trigger long press
    uint32_t debounce_ms;     // Debounce time
} button_config_t;

/**
 * @brief Initialize button module
 */
void button_init(void);

/**
 * @brief Get button event (non-blocking)
 * @param id Button ID
 * @return button_event_t Event type
 */
button_event_t button_get_event(button_id_t id);

/**
 * @brief Check if button is currently pressed
 * @param id Button ID
 * @return true if pressed, false otherwise
 */
bool button_is_pressed(button_id_t id);

/**
 * @brief Reset button state (clear pending events)
 */
void button_reset(void);

/**
 * @brief Task to monitor button states
 * @param arg Task arguments
 */
void button_task(void* arg);

#endif // BUTTON_H
