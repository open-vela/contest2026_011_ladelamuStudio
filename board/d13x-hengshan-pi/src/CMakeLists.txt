# D13x Hengshan Pi 板级源文件 CMakeLists.txt

list(APPEND SRCS artinchip_boot.c)

target_sources(board PRIVATE ${SRCS})
