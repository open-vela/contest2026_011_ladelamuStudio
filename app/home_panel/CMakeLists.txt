# D13x Hengshan-Pi home control panel

if(CONFIG_D13X_HOME_PANEL)
  nuttx_add_application(
    NAME home_panel
    SRCS home_panel_main.c
    PRIORITY ${CONFIG_D13X_HOME_PANEL_PRIORITY}
    STACKSIZE ${CONFIG_D13X_HOME_PANEL_STACKSIZE})
endif()
