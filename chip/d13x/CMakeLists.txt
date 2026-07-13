# D13x chip-level CMakeLists.txt

list(APPEND SRCS
  artinchip_start.c
  artinchip_irq.c
  artinchip_lowputc.c
  artinchip_uart.c
  artinchip_oneshot.c
  artinchip_allocateheap.c
)

if(CONFIG_D13X_I2C)
  list(APPEND SRCS artinchip_i2c.c)
endif()

if(CONFIG_D13X_DISPLAY)
  list(APPEND SRCS
    artinchip_fb.c
    artinchip_de.c
    artinchip_lvds.c
    artinchip_panel.c
  )
endif()

target_sources(arch PRIVATE ${SRCS})

target_include_directories(arch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
