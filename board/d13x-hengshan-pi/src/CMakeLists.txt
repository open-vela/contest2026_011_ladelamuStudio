# D13x Hengshan Pi 板级源文件 CMakeLists.txt

list(APPEND SRCS
  artinchip_boot.c
  artinchip_bringup.c
)

if(CONFIG_D13X_TOUCH_GT911)
  list(APPEND SRCS artinchip_touch.c)
endif()

if(CONFIG_BOARDCTL)
  list(APPEND SRCS artinchip_appinit.c)
endif()

target_sources(board PRIVATE ${SRCS})
