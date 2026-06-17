/**
 * @file button.c
 * @brief Button handling implementation with debounce and long press detection
 */
#include <string.h>
#include "button.h"
#include "config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char* TAG = "按键";

static const button_config_t button_configs[BUTTON_MAX] = {
    [BUTTON_WAKE] = {
        .gpio = GPIO_BTN_WAKE,
        .long_press_ms = 2000,    // 2 seconds for sleep
        .debounce_ms = 50
    },
    [BUTTON_REFRESH] = {
        .gpio = GPIO_BTN_REFRESH,
        .long_press_ms = 3000,    // 3 seconds for config mode
        .debounce_ms = 50
    }
};

// Button state structure
typedef struct {
    bool is_pressed;
    uint32_t press_start_time;
    bool long_press_triggered;
    button_event_t pending_event;
    uint32_t last_event_time;  // For debounce
} button_state_t;

static button_state_t button_states[BUTTON_MAX];
static QueueHandle_t button_event_queue = NULL;

// Interrupt handler for button press
static void IRAM_ATTR button_isr_handler(void* arg)
{
    button_id_t id = (button_id_t)(uint32_t)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    // Notify button task
    if (button_event_queue) {
        uint8_t msg = id;
        xQueueSendFromISR(button_event_queue, &msg, &xHigherPriorityTaskWoken);
    }
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

void button_init(void)
{
    // Create event queue
    button_event_queue = xQueueCreate(10, sizeof(uint8_t));
    
    // Install GPIO ISR service (must be called before gpio_isr_handler_add)
    gpio_install_isr_service(0);
    
    // Initialize button GPIOs
    for (int i = 0; i < BUTTON_MAX; i++) {
        const button_config_t* cfg = &button_configs[i];
        
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << cfg->gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE  // Detect both press and release
        };
        gpio_config(&io_conf);
        
        // Add ISR handler for this button
        gpio_isr_handler_add(cfg->gpio, button_isr_handler, (void*)(uint32_t)i);
        
        // Initialize state
        memset(&button_states[i], 0, sizeof(button_state_t));
        button_states[i].pending_event = BUTTON_EVENT_NONE;
        button_states[i].last_event_time = 0;
        
        // Debug: log initial GPIO state
        int level = gpio_get_level(cfg->gpio);
        DEBUG_LOG_GPIO(TAG, cfg->gpio, level);
    }
    
    // Create button monitoring task
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
}

button_event_t button_get_event(button_id_t id)
{
    if (id >= BUTTON_MAX) return BUTTON_EVENT_NONE;
    
    button_event_t event = button_states[id].pending_event;
    button_states[id].pending_event = BUTTON_EVENT_NONE;
    return event;
}

bool button_is_pressed(button_id_t id)
{
    if (id >= BUTTON_MAX) return false;
    return gpio_get_level(button_configs[id].gpio) == 0;  // Active low
}

void button_reset(void)
{
    for (int i = 0; i < BUTTON_MAX; i++) {
        button_states[i].pending_event = BUTTON_EVENT_NONE;
        button_states[i].long_press_triggered = false;
    }
}

void button_set_sleep_interrupt_type(void)
{
    for (int i = 0; i < BUTTON_MAX; i++) {
        gpio_set_intr_type(button_configs[i].gpio, GPIO_INTR_LOW_LEVEL);
    }
}

void button_restore_interrupt_type(void)
{
    for (int i = 0; i < BUTTON_MAX; i++) {
        gpio_set_intr_type(button_configs[i].gpio, GPIO_INTR_ANYEDGE);
    }
}

void button_task(void* arg)
{
    (void)arg;
    uint8_t button_id;
    uint32_t last_update_tick = 0;
    
    while (1) {
        // Check queue for button events (with debounce timeout)
        if (xQueueReceive(button_event_queue, &button_id, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (button_id >= BUTTON_MAX) continue;
            
            button_state_t* state = &button_states[button_id];
            const button_config_t* cfg = &button_configs[button_id];
            
            bool current_level = gpio_get_level(cfg->gpio) == 0;
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            
            // Debounce: ignore events too close together
            if (state->last_event_time > 0 && 
                (now_ms - state->last_event_time) < cfg->debounce_ms) {
                continue;
            }
            state->last_event_time = now_ms;
            
            // Only log if state actually changed
            if (current_level != state->is_pressed) {
                DEBUG_LOG_GPIO(TAG, cfg->gpio, current_level ? 0 : 1);
                
                if (current_level) {
                    // Button just pressed
                    state->is_pressed = true;
                    state->press_start_time = now_ms;
                    state->long_press_triggered = false;
                    DEBUG_LOG(TAG, "Button %d (GPIO%d) pressed", button_id, cfg->gpio);
                } else {
                    // Button just released
                    state->is_pressed = false;
                    uint32_t press_duration = now_ms - state->press_start_time;
                    
                    if (!state->long_press_triggered) {
                        // Short press
                        state->pending_event = BUTTON_EVENT_PRESS;
                        DEBUG_LOG(TAG, "Button %d (GPIO%d) short press (duration: %d ms)", 
                                  button_id, cfg->gpio, press_duration);
                    } else {
                        DEBUG_LOG(TAG, "Button %d (GPIO%d) released after long press", 
                                  button_id, cfg->gpio);
                    }
                }
            }
            
            last_update_tick = xTaskGetTickCount();
        }
        
        // Check for long press while button is held
        uint32_t now_tick = xTaskGetTickCount();
        if (now_tick - last_update_tick >= pdMS_TO_TICKS(50)) {
            last_update_tick = now_tick;
            
            for (int i = 0; i < BUTTON_MAX; i++) {
                button_state_t* state = &button_states[i];
                const button_config_t* cfg = &button_configs[i];
                
                if (state->is_pressed && !state->long_press_triggered) {
                    uint32_t now_ms = now_tick * portTICK_PERIOD_MS;
                    uint32_t press_duration = now_ms - state->press_start_time;
                    
                    if (press_duration >= cfg->long_press_ms) {
                        state->long_press_triggered = true;
                        state->pending_event = BUTTON_EVENT_LONG_PRESS;
                        DEBUG_LOG(TAG, "Button %d (GPIO%d) long press detected (duration: %d ms)", 
                                  i, cfg->gpio, press_duration);
                    }
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
