/*
 * Sonic en la OLED de las mitades (periféricos con zmk-nice-oled).
 *
 * - Corre más rápido cuanto más rápido se pulsa en ESA mitad: los periféricos no conocen el WPM
 *   (se calcula en el dongle), así que se cuentan las pulsaciones locales de la matriz.
 * - Modos (se alternan con &oled_mode, ver behavior_oled_mode.c): Sonic -> animación original
 *   del módulo -> sin animación. El modo se guarda en settings.
 *
 * No modifica zmk-nice-oled: se engancha a la pantalla que crea su screen_peripheral.c
 * (pantalla -> widget -> canvas -> animación original), por eso west.yml fija su commit.
 */

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include "oled_sonic.h"

#define SRC(array) (const void **)array, ARRAY_SIZE(array)

LV_IMG_DECLARE(sonic_stand_1);
LV_IMG_DECLARE(sonic_stand_2);
LV_IMG_DECLARE(sonic_stand_3);
LV_IMG_DECLARE(sonic_run_1);
LV_IMG_DECLARE(sonic_run_2);
LV_IMG_DECLARE(sonic_run_3);
LV_IMG_DECLARE(sonic_run_4);
LV_IMG_DECLARE(sonic_run_5);
LV_IMG_DECLARE(sonic_run_6);
LV_IMG_DECLARE(sonic_run_7);
LV_IMG_DECLARE(sonic_run_8);
LV_IMG_DECLARE(sonic_fast_1);
LV_IMG_DECLARE(sonic_fast_2);
LV_IMG_DECLARE(sonic_fast_3);
LV_IMG_DECLARE(sonic_fast_4);
LV_IMG_DECLARE(sonic_roll_1);
LV_IMG_DECLARE(sonic_roll_2);
LV_IMG_DECLARE(sonic_roll_3);
LV_IMG_DECLARE(sonic_roll_4);
LV_IMG_DECLARE(sonic_roll_5);

static const lv_img_dsc_t *stand_imgs[] = {&sonic_stand_1, &sonic_stand_1, &sonic_stand_1,
                                           &sonic_stand_1, &sonic_stand_2, &sonic_stand_3,
                                           &sonic_stand_2, &sonic_stand_3};
static const lv_img_dsc_t *run_imgs[] = {&sonic_run_1, &sonic_run_2, &sonic_run_3, &sonic_run_4,
                                         &sonic_run_5, &sonic_run_6, &sonic_run_7, &sonic_run_8};
static const lv_img_dsc_t *fast_imgs[] = {&sonic_fast_1, &sonic_fast_2, &sonic_fast_3,
                                          &sonic_fast_4};
static const lv_img_dsc_t *roll_imgs[] = {&sonic_roll_1, &sonic_roll_2, &sonic_roll_3,
                                          &sonic_roll_4, &sonic_roll_5};

struct sonic_level {
    const void **imgs;
    uint8_t count;
    uint16_t duration_ms;      // duración de un ciclo completo de la animación
    uint8_t min_presses;       // pulsaciones mínimas dentro de la ventana para este nivel
};

// Ventana de CONFIG_SOFLE_OLED_SONIC_WINDOW_MS (2 s por defecto). Umbrales por mitad:
// escribiendo a ~60 WPM cada mano hace ~2-3 pulsaciones/s.
static const struct sonic_level levels[] = {
    {SRC(stand_imgs), 2400, 0}, // quieto (golpea el pie)
    {SRC(run_imgs), 1400, 1},   // caminando
    {SRC(run_imgs), 800, 3},    // trotando
    {SRC(fast_imgs), 400, 6},   // corriendo
    {SRC(fast_imgs), 220, 9},   // a toda velocidad
    {SRC(roll_imgs), 250, 13},  // rodando (spin dash)
};

#define PRESS_HISTORY 32
static uint32_t press_times[PRESS_HISTORY];
static uint8_t press_head;
static struct k_spinlock press_lock;

static lv_obj_t *sonic_obj;
static lv_obj_t *original_obj;
static int current_level = -1;
static uint8_t mode = SOFLE_OLED_MODE_SONIC;

static void tick_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_work_handler);

static uint8_t presses_in_window(void) {
    uint32_t now = k_uptime_get_32();
    uint8_t n = 0;
    k_spinlock_key_t key = k_spin_lock(&press_lock);
    for (int i = 0; i < PRESS_HISTORY; i++) {
        if (press_times[i] != 0 && now - press_times[i] < CONFIG_SOFLE_OLED_SONIC_WINDOW_MS) {
            n++;
        }
    }
    k_spin_unlock(&press_lock, key);
    return n;
}

static void set_level(int level) {
    if (level == current_level) {
        return;
    }
    current_level = level;
    lv_animimg_set_src(sonic_obj, levels[level].imgs, levels[level].count);
    lv_animimg_set_duration(sonic_obj, levels[level].duration_ms);
    lv_animimg_set_repeat_count(sonic_obj, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(sonic_obj);
}

static void tick_work_handler(struct k_work *work) {
    if (sonic_obj == NULL || mode != SOFLE_OLED_MODE_SONIC) {
        return;
    }
    uint8_t n = presses_in_window();
    int level = 0;
    for (int i = ARRAY_SIZE(levels) - 1; i > 0; i--) {
        if (n >= levels[i].min_presses) {
            level = i;
            break;
        }
    }
    set_level(level);
    if (n > 0) {
        // Mientras quede alguna pulsación en la ventana, revisar para que vaya frenando.
        k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_MSEC(250));
    }
}

static void stop_anim(lv_obj_t *obj) {
    lv_anim_del(obj, NULL);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void apply_mode(void) {
    if (sonic_obj == NULL) {
        return;
    }
    stop_anim(sonic_obj);
    if (original_obj != NULL) {
        stop_anim(original_obj);
    }
    current_level = -1;

    switch (mode) {
    case SOFLE_OLED_MODE_SONIC:
        lv_obj_clear_flag(sonic_obj, LV_OBJ_FLAG_HIDDEN);
        k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_NO_WAIT);
        break;
    case SOFLE_OLED_MODE_ORIGINAL:
        if (original_obj != NULL) {
            lv_obj_clear_flag(original_obj, LV_OBJ_FLAG_HIDDEN);
            lv_animimg_start(original_obj);
        }
        break;
    default: // SOFLE_OLED_MODE_OFF
        break;
    }
}

static void apply_mode_work_handler(struct k_work *work) { apply_mode(); }
static K_WORK_DEFINE(apply_mode_work, apply_mode_work_handler);

static void save_work_handler(struct k_work *work) {
#if IS_ENABLED(CONFIG_SETTINGS)
    settings_save_one("sofle_oled/mode", &mode, sizeof(mode));
#endif
}
static K_WORK_DELAYABLE_DEFINE(save_work, save_work_handler);

void sofle_oled_mode_cycle(void) {
    mode = (mode + 1) % SOFLE_OLED_MODE_COUNT;
    LOG_INF("OLED mode -> %d", mode);
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &apply_mode_work);
    }
    // Evita escribir en flash en cada pulsación si se cambia varias veces seguidas.
    k_work_reschedule(&save_work, K_SECONDS(5));
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int mode_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "mode") == 0 && len == sizeof(mode)) {
        uint8_t value;
        if (read_cb(cb_arg, &value, sizeof(value)) == sizeof(value) &&
            value < SOFLE_OLED_MODE_COUNT) {
            mode = value;
            if (zmk_display_is_initialized()) {
                k_work_submit_to_queue(zmk_display_work_q(), &apply_mode_work);
            }
        }
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(sofle_oled, "sofle_oled", NULL, mode_settings_set, NULL, NULL);
#endif

static int position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_spinlock_key_t key = k_spin_lock(&press_lock);
    press_times[press_head] = k_uptime_get_32() | 1; // 0 = hueco vacío
    press_head = (press_head + 1) % PRESS_HISTORY;
    k_spin_unlock(&press_lock, key);

    if (mode == SOFLE_OLED_MODE_SONIC && zmk_display_is_initialized()) {
        k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(sofle_oled_sonic, position_listener);
ZMK_SUBSCRIPTION(sofle_oled_sonic, zmk_position_state_changed);

/*
 * Enganche a la pantalla de zmk-nice-oled (screen_peripheral.c, commit fijado en west.yml):
 *   lv_scr_act() -> hijo 0: widget->obj -> hijo 0: canvas -> hijo 0: animación original.
 */
static void attach_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(attach_work, attach_work_handler);

static void attach_work_handler(struct k_work *work) {
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *widget = screen ? lv_obj_get_child(screen, 0) : NULL;
    lv_obj_t *canvas = widget ? lv_obj_get_child(widget, 0) : NULL;

    if (canvas == NULL) {
        k_work_reschedule_for_queue(zmk_display_work_q(), &attach_work, K_MSEC(200));
        return;
    }

    original_obj = lv_obj_get_child(canvas, 0);
    if (original_obj != NULL && !lv_obj_check_type(original_obj, &lv_animimg_class)) {
        original_obj = NULL; // estructura distinta a la esperada: no tocar nada del módulo
    }

    sonic_obj = lv_animimg_create(canvas);
    lv_obj_align(sonic_obj, LV_ALIGN_TOP_LEFT, CONFIG_SOFLE_OLED_SONIC_X,
                 CONFIG_SOFLE_OLED_SONIC_Y);
    apply_mode();
}

// La cola de la pantalla solo existe una vez que ZMK inicializa el display: esperar en la cola del
// sistema y luego pasar a la del display (todo lo que toca LVGL debe correr ahí).
static void boot_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(boot_work, boot_work_handler);

static void boot_work_handler(struct k_work *work) {
    if (!zmk_display_is_initialized()) {
        k_work_reschedule(&boot_work, K_MSEC(200));
        return;
    }
    k_work_schedule_for_queue(zmk_display_work_q(), &attach_work, K_MSEC(100));
}

static int sofle_oled_sonic_init(void) {
    k_work_schedule(&boot_work, K_MSEC(500));
    return 0;
}

SYS_INIT(sofle_oled_sonic_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
