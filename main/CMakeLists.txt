set(EXTRA_REQUIRES "")
if(CONFIG_XIAO_EXTERNAL_ANTENNA)
    list(APPEND EXTRA_REQUIRES driver)
endif()

idf_component_register(
    SRCS "elrs-netpack.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_netif esp_event esp_ringbuf espnow_server tcp_server tasks msp ${EXTRA_REQUIRES}
)
