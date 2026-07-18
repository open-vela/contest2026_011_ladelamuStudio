# D13x Hengshan-Pi home control panel

if(CONFIG_D13X_HOME_PANEL)
  nuttx_add_application(
    NAME home_panel
    SRCS home_panel_main.c home_panel_mijia_client.c
         home_panel_mijia_model.c home_panel_font.c home_panel_misans_18.c
         home_panel_digits_28.c
    PRIORITY ${CONFIG_D13X_HOME_PANEL_PRIORITY}
    STACKSIZE ${CONFIG_D13X_HOME_PANEL_STACKSIZE})
endif()
