/****************************************************************************
 * D13x home panel Mijia family model
 ****************************************************************************/

#ifndef __HOME_PANEL_MIJIA_MODEL_H
#define __HOME_PANEL_MIJIA_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define HOME_PANEL_MAX_DEVICES  20
#define HOME_PANEL_MAX_ROOMS    16
#define HOME_PANEL_MAX_SCENES   8

struct home_panel_device_s
{
  char did[80];
  char name[48];
  char room[32];
  char model[64];
  char type[32];
  bool online;
  bool has_power;
  uint16_t power_siid;
  uint16_t power_piid;
  bool power_writable;
  bool power;
  bool has_brightness;
  uint16_t brightness_siid;
  uint16_t brightness_piid;
  int brightness;
  int brightness_min;
  int brightness_max;
  bool has_temperature;
  uint16_t temperature_siid;
  uint16_t temperature_piid;
  int temperature;
  bool has_humidity;
  uint16_t humidity_siid;
  uint16_t humidity_piid;
  int humidity;
  bool has_battery;
  uint16_t battery_siid;
  uint16_t battery_piid;
  int battery;
};

struct home_panel_room_s
{
  char name[32];
  unsigned int device_count;
  bool has_temperature;
  int temperature;
  bool has_humidity;
  int humidity;
};

struct home_panel_scene_s
{
  char id[64];
  char name[48];
};

struct home_panel_family_model_s
{
  uint32_t revision;
  unsigned int device_count;
  unsigned int online_count;
  unsigned int room_count;
  unsigned int scene_count;
  bool mqtt_connected;
  char event_source[32];
  struct home_panel_device_s devices[HOME_PANEL_MAX_DEVICES];
  struct home_panel_room_s rooms[HOME_PANEL_MAX_ROOMS];
  struct home_panel_scene_s scenes[HOME_PANEL_MAX_SCENES];
};

int home_panel_mijia_model_parse(const char *json, uint32_t revision,
                                 struct home_panel_family_model_s *model);
int home_panel_mijia_model_apply_online(
  struct home_panel_family_model_s *model, const char *did, bool online);
int home_panel_mijia_model_apply_property(
  struct home_panel_family_model_s *model, const char *did,
  uint16_t siid, uint16_t piid, bool is_boolean, bool boolean_value,
  bool is_number, int number_value);
void home_panel_mijia_model_refresh_rooms(
  struct home_panel_family_model_s *model);

#endif
