# Board-level CMakeLists.txt for D13x Hengshan Pi

add_subdirectory(src)

set_property(GLOBAL PROPERTY LD_SCRIPT ${NUTTX_BOARD_ABS_DIR}/scripts/ld.script)
