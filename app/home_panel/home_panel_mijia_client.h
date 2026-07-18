/****************************************************************************
 * D13x home panel Mijia API client
 ****************************************************************************/

#ifndef __HOME_PANEL_MIJIA_CLIENT_H
#define __HOME_PANEL_MIJIA_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum home_panel_mijia_state_e
{
  HOME_PANEL_MIJIA_IDLE = 0,
  HOME_PANEL_MIJIA_STARTING,
  HOME_PANEL_MIJIA_WAITING,
  HOME_PANEL_MIJIA_AUTHENTICATED,
  HOME_PANEL_MIJIA_EXPIRED,
  HOME_PANEL_MIJIA_ERROR
};

enum home_panel_mijia_command_state_e
{
  HOME_PANEL_MIJIA_COMMAND_IDLE = 0,
  HOME_PANEL_MIJIA_COMMAND_PENDING,
  HOME_PANEL_MIJIA_COMMAND_ACCEPTED,
  HOME_PANEL_MIJIA_COMMAND_CONFIRMED,
  HOME_PANEL_MIJIA_COMMAND_ERROR
};

struct home_panel_mijia_snapshot_s
{
  enum home_panel_mijia_state_e state;
  uint32_t revision;
  uint32_t qr_revision;
  size_t qr_size;
  unsigned int device_count;
  unsigned int online_count;
  char home_name[64];
  char message[96];
};

struct home_panel_mijia_family_snapshot_s
{
  uint32_t revision;
  uint32_t server_revision;
  uint32_t generated_at;
  size_t json_size;
  unsigned int home_count;
  unsigned int room_count;
  unsigned int device_count;
  unsigned int online_count;
  unsigned int scene_count;
  unsigned int detail_error_count;
  unsigned int consecutive_failures;
  bool stale;
};

struct home_panel_mijia_command_snapshot_s
{
  enum home_panel_mijia_command_state_e state;
  uint32_t revision;
  int code;
  bool target;
  char device_name[48];
  char message[96];
};

struct home_panel_family_model_s;

int home_panel_mijia_initialize(void);
int home_panel_mijia_request_login(void);
void home_panel_mijia_get_snapshot(
  struct home_panel_mijia_snapshot_s *snapshot);
void home_panel_mijia_get_family_snapshot(
  struct home_panel_mijia_family_snapshot_s *snapshot);
int home_panel_mijia_get_family_model(
  uint32_t revision, struct home_panel_family_model_s *model);
int home_panel_mijia_request_bool_property(const char *did,
                                            const char *device_name,
                                            const char *property_name,
                                            uint16_t siid,
                                            uint16_t piid,
                                            bool value);
int home_panel_mijia_request_scene(const char *scene_id,
                                   const char *scene_name);
void home_panel_mijia_get_command_snapshot(
  struct home_panel_mijia_command_snapshot_s *snapshot);
int home_panel_mijia_copy_qr(uint32_t revision, void *buffer,
                             size_t capacity, size_t *size);
const char *home_panel_mijia_state_name(
  enum home_panel_mijia_state_e state);

#endif
