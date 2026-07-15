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

int home_panel_mijia_initialize(void);
int home_panel_mijia_request_login(void);
void home_panel_mijia_get_snapshot(
  struct home_panel_mijia_snapshot_s *snapshot);
int home_panel_mijia_copy_qr(uint32_t revision, void *buffer,
                             size_t capacity, size_t *size);
const char *home_panel_mijia_state_name(
  enum home_panel_mijia_state_e state);

#endif
