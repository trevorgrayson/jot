set(src_dirs        
    .
    ./app
    ./bsp
    ./jot
)

set(include_dirs      
    .
    ./app
    ./bsp
    ./jot
    ../../../lib/JotCore/src
    ../../../include
) 

idf_component_register(
    SRC_DIRS 
        ${src_dirs} 
    INCLUDE_DIRS 
        ${include_dirs}
    REQUIRES 
        esp_hid
        esp_driver_ledc
        esp_driver_i2c
        esp_driver_uart
        esp_wifi
        esp_netif
        bt
        esp_http_client
        esp_timer
        esp_event
        json
        littlefs
    PRIV_REQUIRES 
        nvs_flash
)
target_sources(${COMPONENT_LIB} PRIVATE "../../../lib/JotCore/src/JotCore.cpp")
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
