idf_component_register(
    SRCS "elrs-netpack.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event driver
)
