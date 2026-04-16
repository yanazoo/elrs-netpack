set(ETH_REQUIRES "")
if(CONFIG_TCP_USE_ETHERNET)
    list(APPEND ETH_REQUIRES esp_eth)
endif()

idf_component_register(
    SRCS "tcp_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_wifi esp_event esp_ringbuf tasks msp mdns ${ETH_REQUIRES}
)
