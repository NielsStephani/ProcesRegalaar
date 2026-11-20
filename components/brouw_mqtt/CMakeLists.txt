idf_component_register(
    SRCS "brouw_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES brouw_netwerk mqtt esp_event esp_hw_support esp_app_format
)
