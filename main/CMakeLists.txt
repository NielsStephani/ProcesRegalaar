idf_component_register(SRCS "main.c" "recept.c"
                    INCLUDE_DIRS "."
                    REQUIRES max31865 scherm_uart json brouw_netwerk brouw_mqtt)
