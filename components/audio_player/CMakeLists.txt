# The coin PCM is embedded only in an audio build (CONFIG_AUDIO_PLAYER_ENABLE).
set(embed)
if(CONFIG_AUDIO_PLAYER_ENABLE)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/sounds/coin.pcm")
        message(FATAL_ERROR "components/audio_player/sounds/coin.pcm is missing: "
                            "run tools/coin_sound.py (assets/README.md)")
    endif()
    list(APPEND embed "sounds/coin.pcm")
endif()

idf_component_register(
    SRCS "audio_player.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2s esp_driver_gpio
    EMBED_FILES ${embed})
