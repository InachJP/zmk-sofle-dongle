#pragma once

enum sofle_oled_mode {
    SOFLE_OLED_MODE_SONIC,
    SOFLE_OLED_MODE_ORIGINAL,
    SOFLE_OLED_MODE_OFF,
    SOFLE_OLED_MODE_COUNT,
};

// Pasa al siguiente modo de la animación de la OLED (solo en las mitades).
void sofle_oled_mode_cycle(void);
