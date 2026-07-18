/****************************************************************************
 * D13x home panel Mijia family model
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <netutils/cJSON.h>

#include "home_panel_mijia_model.h"

static void model_copy(char *destination, size_t capacity,
                       const char *source)
{
  snprintf(destination, capacity, "%s", source == NULL ? "" : source);
}

static const char *model_string(cJSON *object, const char *name)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

  return cJSON_IsString(item) && item->valuestring != NULL ?
         item->valuestring : NULL;
}

static cJSON *model_property(cJSON *properties, const char *name)
{
  cJSON *property;

  cJSON_ArrayForEach(property, properties)
    {
      const char *property_name = model_string(property, "name");
      if (property_name != NULL && strcmp(property_name, name) == 0)
        {
          return property;
        }
    }

  return NULL;
}

static bool model_number(cJSON *property, int *value)
{
  cJSON *item;

  if (property == NULL)
    {
      return false;
    }

  item = cJSON_GetObjectItemCaseSensitive(property, "current_value");
  if (!cJSON_IsNumber(item))
    {
      return false;
    }

  *value = item->valueint;
  return true;
}

static bool model_boolean(cJSON *property, bool *value)
{
  cJSON *item;

  if (property == NULL)
    {
      return false;
    }

  item = cJSON_GetObjectItemCaseSensitive(property, "current_value");
  if (!cJSON_IsBool(item))
    {
      return false;
    }

  *value = cJSON_IsTrue(item);
  return true;
}

static bool model_writable(cJSON *property)
{
  const char *rw = property == NULL ? NULL : model_string(property, "rw");

  return rw != NULL && strchr(rw, 'w') != NULL;
}

static void model_property_ids(cJSON *property, uint16_t *siid,
                               uint16_t *piid)
{
  cJSON *item;

  if (property == NULL)
    {
      return;
    }

  item = cJSON_GetObjectItemCaseSensitive(property, "siid");
  if (cJSON_IsNumber(item) && item->valueint > 0 && item->valueint <= 65535)
    {
      *siid = (uint16_t)item->valueint;
    }

  item = cJSON_GetObjectItemCaseSensitive(property, "piid");
  if (cJSON_IsNumber(item) && item->valueint > 0 && item->valueint <= 65535)
    {
      *piid = (uint16_t)item->valueint;
    }
}

static void model_brightness_range(cJSON *property,
                                   struct home_panel_device_s *device)
{
  cJSON *range;
  cJSON *minimum;
  cJSON *maximum;

  range = cJSON_GetObjectItemCaseSensitive(property, "range");
  minimum = cJSON_IsArray(range) ? cJSON_GetArrayItem(range, 0) : NULL;
  maximum = cJSON_IsArray(range) ? cJSON_GetArrayItem(range, 1) : NULL;
  device->brightness_min = cJSON_IsNumber(minimum) ? minimum->valueint : 1;
  device->brightness_max = cJSON_IsNumber(maximum) ? maximum->valueint : 100;
}

static void model_parse_device(cJSON *object,
                               struct home_panel_device_s *device)
{
  cJSON *properties;
  cJSON *item;
  int value;

  model_copy(device->did, sizeof(device->did), model_string(object, "did"));
  model_copy(device->name, sizeof(device->name),
             model_string(object, "name"));
  model_copy(device->room, sizeof(device->room),
             model_string(object, "room_name"));
  model_copy(device->model, sizeof(device->model),
             model_string(object, "model"));
  model_copy(device->type, sizeof(device->type),
             model_string(object, "device_type"));
  item = cJSON_GetObjectItemCaseSensitive(object, "isOnline");
  device->online = cJSON_IsTrue(item);

  properties = cJSON_GetObjectItemCaseSensitive(object, "properties");
  if (!cJSON_IsArray(properties))
    {
      return;
    }

  item = model_property(properties, "on");
  device->has_power = model_boolean(item, &device->power);
  model_property_ids(item, &device->power_siid, &device->power_piid);
  device->power_writable = model_writable(item);

  item = model_property(properties, "brightness");
  model_property_ids(item, &device->brightness_siid,
                     &device->brightness_piid);
  if (model_number(item, &value))
    {
      device->has_brightness = true;
      device->brightness = (int)value;
      model_brightness_range(item, device);
    }

  item = model_property(properties, "temperature");
  model_property_ids(item, &device->temperature_siid,
                     &device->temperature_piid);
  if (model_number(item, &value))
    {
      device->has_temperature = true;
      device->temperature = value;
    }

  item = model_property(properties, "relative-humidity");
  model_property_ids(item, &device->humidity_siid,
                     &device->humidity_piid);
  if (model_number(item, &value))
    {
      device->has_humidity = true;
      device->humidity = value;
    }

  item = model_property(properties, "battery-level");
  model_property_ids(item, &device->battery_siid,
                     &device->battery_piid);
  if (model_number(item, &value))
    {
      device->has_battery = true;
      device->battery = (int)value;
    }
}

static struct home_panel_room_s *model_find_room(
  struct home_panel_family_model_s *model, const char *name)
{
  unsigned int index;

  for (index = 0; index < model->room_count; index++)
    {
      if (strcmp(model->rooms[index].name, name) == 0)
        {
          return &model->rooms[index];
        }
    }

  return NULL;
}

void home_panel_mijia_model_refresh_rooms(
  struct home_panel_family_model_s *model)
{
  unsigned int index;

  for (index = 0; index < model->room_count; index++)
    {
      model->rooms[index].has_temperature = false;
      model->rooms[index].temperature = 0;
      model->rooms[index].has_humidity = false;
      model->rooms[index].humidity = 0;
    }

  for (index = 0; index < model->device_count; index++)
    {
      struct home_panel_device_s *device = &model->devices[index];
      struct home_panel_room_s *room;

      if (strcmp(device->type, "environment-sensor") != 0)
        {
          continue;
        }

      room = model_find_room(model, device->room);
      if (room == NULL)
        {
          continue;
        }

      room->has_temperature = device->has_temperature;
      room->temperature = device->temperature;
      room->has_humidity = device->has_humidity;
      room->humidity = device->humidity;
    }
}

int home_panel_mijia_model_apply_online(
  struct home_panel_family_model_s *model, const char *did, bool online)
{
  unsigned int index;

  for (index = 0; index < model->device_count; index++)
    {
      struct home_panel_device_s *device = &model->devices[index];

      if (strcmp(device->did, did) != 0)
        {
          continue;
        }

      if (device->online == online)
        {
          return 0;
        }

      device->online = online;
      if (online)
        {
          model->online_count++;
        }
      else if (model->online_count > 0)
        {
          model->online_count--;
        }
      return 1;
    }

  return -ENOENT;
}

int home_panel_mijia_model_apply_property(
  struct home_panel_family_model_s *model, const char *did,
  uint16_t siid, uint16_t piid, bool is_boolean, bool boolean_value,
  bool is_number, int number_value)
{
  struct home_panel_device_s *device = NULL;
  int *number_target = NULL;
  bool *has_target = NULL;
  unsigned int index;

  for (index = 0; index < model->device_count; index++)
    {
      if (strcmp(model->devices[index].did, did) == 0)
        {
          device = &model->devices[index];
          break;
        }
    }

  if (device == NULL)
    {
      return -ENOENT;
    }

  if (device->power_siid == siid && device->power_piid == piid)
    {
      if (!is_boolean)
        {
          return -EBADMSG;
        }

      if (device->has_power && device->power == boolean_value)
        {
          return 0;
        }

      device->has_power = true;
      device->power = boolean_value;
      return 1;
    }

  if (device->brightness_siid == siid && device->brightness_piid == piid)
    {
      has_target = &device->has_brightness;
      number_target = &device->brightness;
    }
  else if (device->temperature_siid == siid &&
           device->temperature_piid == piid)
    {
      has_target = &device->has_temperature;
      number_target = &device->temperature;
    }
  else if (device->humidity_siid == siid && device->humidity_piid == piid)
    {
      has_target = &device->has_humidity;
      number_target = &device->humidity;
    }
  else if (device->battery_siid == siid && device->battery_piid == piid)
    {
      has_target = &device->has_battery;
      number_target = &device->battery;
    }
  else
    {
      return -ENOENT;
    }

  if (!is_number)
    {
      return -EBADMSG;
    }

  if (*has_target && *number_target == number_value)
    {
      return 0;
    }

  *has_target = true;
  *number_target = number_value;
  return 1;
}

int home_panel_mijia_model_parse(const char *json, uint32_t revision,
                                 struct home_panel_family_model_s *model)
{
  cJSON *devices;
  cJSON *homes;
  cJSON *object;
  cJSON *protocol;
  cJSON *root;
  cJSON *rooms;
  cJSON *scenes;

  if (json == NULL || model == NULL)
    {
      return -EINVAL;
    }

  root = cJSON_Parse(json);
  if (root == NULL)
    {
      return -EBADMSG;
    }

  memset(model, 0, sizeof(*model));
  model->revision = revision;
  protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol");
  model_copy(model->event_source, sizeof(model->event_source),
             model_string(protocol, "event_source"));
  object = cJSON_GetObjectItemCaseSensitive(protocol, "mqtt_connected");
  model->mqtt_connected = cJSON_IsTrue(object);

  devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
  homes = cJSON_GetObjectItemCaseSensitive(root, "homes");
  scenes = cJSON_GetObjectItemCaseSensitive(root, "scenes");
  if (!cJSON_IsArray(devices) || !cJSON_IsArray(homes) ||
      !cJSON_IsArray(scenes))
    {
      cJSON_Delete(root);
      return -EBADMSG;
    }

  cJSON_ArrayForEach(object, devices)
    {
      struct home_panel_device_s *device;

      if (model->device_count >= HOME_PANEL_MAX_DEVICES)
        {
          break;
        }

      device = &model->devices[model->device_count++];
      model_parse_device(object, device);
      model->online_count += device->online ? 1 : 0;
    }

  cJSON_ArrayForEach(object, homes)
    {
      cJSON *room;

      rooms = cJSON_GetObjectItemCaseSensitive(object, "rooms");
      if (!cJSON_IsArray(rooms))
        {
          continue;
        }

      cJSON_ArrayForEach(room, rooms)
        {
          struct home_panel_room_s *target;
          cJSON *dids;

          if (model->room_count >= HOME_PANEL_MAX_ROOMS)
            {
              break;
            }

          target = &model->rooms[model->room_count++];
          model_copy(target->name, sizeof(target->name),
                     model_string(room, "name"));
          dids = cJSON_GetObjectItemCaseSensitive(room, "dids");
          target->device_count = cJSON_IsArray(dids) ?
                                 cJSON_GetArraySize(dids) : 0;
        }
    }

  home_panel_mijia_model_refresh_rooms(model);

  cJSON_ArrayForEach(object, scenes)
    {
      struct home_panel_scene_s *scene;

      if (model->scene_count >= HOME_PANEL_MAX_SCENES)
        {
          break;
        }

      scene = &model->scenes[model->scene_count++];
      model_copy(scene->id, sizeof(scene->id),
                 model_string(object, "scene_id"));
      model_copy(scene->name, sizeof(scene->name),
                 model_string(object, "name"));
    }

  cJSON_Delete(root);
  return 0;
}
