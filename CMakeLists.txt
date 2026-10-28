target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_OLED_MODE app PRIVATE src/behavior_oled_mode.c)

if(CONFIG_SOFLE_OLED_SONIC)
    target_sources(app PRIVATE src/oled_sonic.c src/sonic_frames.c)
endif()
