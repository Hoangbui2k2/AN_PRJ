# Main component CMakeLists.txt for LoRa Irrigation Gateway

idf_component_register(
    SRCS
        main.c
        lora_uart.c
        mqtt.c
        node_manager.c
        command_cache.c
        crc.c
        config.c
        topic.c
        certs.c
    INCLUDE_DIRS
        .
    REQUIRES
        nvs_flash
        esp_timer
        esp_wifi
        esp_event
        esp_netif
        mqtt
        driver
        freertos
        json
        mbedtls
)
