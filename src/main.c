// Wokwi SH1107 Display Custom Chip - For information and examples see:
// https://docs.wokwi.com/chips-api/getting-started
//
// SPDX-License-Identifier: MIT
// Copyright (C) 2023 Uri Shaked / wokwi.com

// Datasheet: https://www.displayfuture.com/Display/datasheet/controller/SH1107.pdf

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define SH1107_CONTROL_CO 0x80
#define SH1107_CONTROL_DC 0x40

#define CMD_SET_PAGE_ADDR_MODE 0x20
#define CMD_SET_VERTICAL_ADDR_MODE 0x21
#define CMD_SET_CONTRAST 0x81
#define CMD_SEG_REMAP_OFF 0xa0
#define CMD_SEG_REMAP_ON 0xa1
#define CMD_DISPLAY_ALL_ON_RESUME 0xa4
#define CMD_DISPLAY_ALL_ON 0xa5
#define CMD_NORMAL_DISPLAY 0xa6
#define CMD_INVERT_DISPLAY 0xa7
#define CMD_SET_MULTIPLEX 0xa8
#define CMD_DCDC 0xad
#define CMD_DISPLAY_OFF 0xae
#define CMD_DISPLAY_ON 0xaf
#define CMD_COM_SCAN_INC 0xc0
#define CMD_COM_SCAN_DEC 0xc8
#define CMD_SET_DISPLAY_OFFSET 0xd3
#define CMD_SET_DISPLAY_CLOCK_DIV 0xd5
#define CMD_SET_PRECHARGE 0xd9
#define CMD_SET_COM_PINS 0xda
#define CMD_SET_VCOM_DESELECT 0xdb
#define CMD_SET_DISP_START_LINE 0xdc
#define CMD_READ_MODIFY_WRITE 0xe0
#define CMD_END 0xee
#define CMD_NOP 0xe3

// Specifies the number of parameter bytes for each multi-byte command
const uint8_t multi_byte_commands[] = {
    [CMD_SET_CONTRAST] = 1,
    [CMD_SET_MULTIPLEX] = 1,
    [CMD_DCDC] = 1,
    [CMD_SET_DISPLAY_OFFSET] = 1,
    [CMD_SET_COM_PINS] = 1,
    [CMD_SET_DISPLAY_CLOCK_DIV] = 1,
    [CMD_SET_PRECHARGE] = 1,
    [CMD_SET_VCOM_DESELECT] = 1,
    [CMD_SET_DISP_START_LINE] = 1,
};

typedef struct
{
  // Display buffer
  uint32_t width;
  uint32_t height;
  int8_t x_offset;
  uint8_t pixels[128 * 128 / 8];
  buffer_t framebuffer;
  timer_t update_timer;

  // Display settings
  bool display_on;
  bool updated;
  uint8_t contrast;
  bool invert;
  bool reverse_rows;
  bool segment_remap;
  
  // Speed and timing settings
  uint8_t clock_divider;
  uint8_t multiplex_ratio;
  uint8_t phase1;
  uint8_t phase2;

  // Memory and addressing settings
  uint8_t active_column;
  uint8_t active_page;
  uint8_t memory_mode;

  uint8_t start_line;

  // Command parsing state machine
  bool control_byte;
  bool continuous_mode;
  bool command_mode;
  uint8_t current_command_index;
  uint8_t current_command_length;
  uint8_t current_command[8];
} sh1107_state_t;

static void sh1107_reset(sh1107_state_t *state)
{
  state->width = 128;
  state->height = 128;
  state->x_offset = 96; // varias between display models
  state->memory_mode = CMD_SET_PAGE_ADDR_MODE;
  state->contrast = 0x7f;
  state->clock_divider = 1;
  state->multiplex_ratio = 63;
  state->phase1 = 2;
  state->phase2 = 2;
  state->current_command_index = 0;
  state->active_column = 0;
  state->active_page = 0;
  state->start_line = 0;
  state->reverse_rows = false;  
  state->invert = false;
  state->updated = false;
}

void sh1107_update_buffer(void *user_data) {
  sh1107_state_t *state = user_data;
  const uint8_t *pixels = state->pixels;
  const uint8_t invert = state->invert;
  const bool display_on = state->display_on;
  const bool reverse_rows = state->reverse_rows;
  const uint8_t start_line = state->start_line;
  const uint8_t width = state->width;
  const uint8_t height = state->height;
  const int8_t x_offset = state->x_offset;

  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      const uint32_t scroll_y = y + start_line;
      const uint32_t virtual_y = (reverse_rows ? height - 1 - scroll_y : scroll_y) % width;
      const uint32_t pix_index = (virtual_y / 8) * width + (x + x_offset + width) % width;
      const bool pixValue = pixels[pix_index] & (1 << virtual_y % 8) ? !invert : invert;
      const uint32_t data_offset = (y * width + x) * 4;
      uint32_t pixel = pixValue && display_on ? 0xffffffff : 0;
      buffer_write(state->framebuffer, data_offset, &pixel, sizeof(pixel));
    }
  }
  state->updated = false;
}

void sh1107_schedule_update(sh1107_state_t *state) {
  if (!state->updated) {
    state->updated = true;
    timer_start(state->update_timer, 16667, false); // 16.667 millis for ~60 MHz
  }
}

static void sh1107_process_command(sh1107_state_t *state)
{
  uint8_t command_code = state->current_command[0];
  bool auto_update = false;
  switch (command_code)
  {
  case CMD_SET_CONTRAST:
    state->contrast = state->current_command[1];
    auto_update = true;
    break;

  case CMD_DISPLAY_OFF:
    state->display_on = false;
    sh1107_schedule_update(state);
    break;

  case CMD_DISPLAY_ON:
    state->display_on = true;
    sh1107_schedule_update(state);
    break;

  case CMD_NORMAL_DISPLAY:
    state->invert = false;
    auto_update = true;
    break;

  case CMD_INVERT_DISPLAY:
    state->invert = true;
    auto_update = true;
    break;

  case CMD_NOP:
    break;

  case CMD_SET_PAGE_ADDR_MODE:
  case CMD_SET_VERTICAL_ADDR_MODE:
    state->memory_mode = state->current_command[0];
    break;

  case CMD_SET_DISPLAY_CLOCK_DIV:
    state->clock_divider = 1 + (state->current_command[1] & 0xf);
    break;

  case CMD_SET_PRECHARGE:
    state->phase1 = state->current_command[1] & 0xf;
    state->phase2 = (state->current_command[1] >> 4) & 0xf;
    break;

  case CMD_COM_SCAN_INC:
    state->reverse_rows = false;
    auto_update = true;
    break;

  case CMD_COM_SCAN_DEC:
    state->reverse_rows = true;
    auto_update = true;
    break;

  case CMD_SEG_REMAP_OFF:
    state->segment_remap = false;
    auto_update = true;
    break;

  case CMD_SEG_REMAP_ON:
    state->segment_remap = true;
    auto_update = true;
    break;

  case CMD_SET_DISP_START_LINE:
    state->start_line = state->current_command[1];
    auto_update = true;
    break;

  case CMD_SET_DISPLAY_OFFSET: 
  case CMD_SET_MULTIPLEX:
  case CMD_SET_VCOM_DESELECT:
  case CMD_SET_COM_PINS:
  case CMD_DISPLAY_ALL_ON:
  case CMD_DISPLAY_ALL_ON_RESUME:
    // not implemented
    break;

  default:
    if (command_code <= 0x0f)
    {
      state->active_column = (state->active_column & 0x70) | command_code;
      break;
    }

    if (command_code >= 0x10 && command_code <= 0x17)
    {
      state->active_column = (state->active_column & 0x0f) | ((command_code & 0x07) << 4);
      break;
    }

    if (command_code >= 0xb0 && command_code <= 0xc0)
    {
      state->active_page = command_code & 0x0f;
      auto_update = true;
      break;
    }

    printf("Unknown SH1107 Command %02x\n", command_code);
  }

  if (auto_update && state->display_on)
  {
    sh1107_schedule_update(state);
  }

  // Reset command buffer index, ready to read the next command
  state->current_command_index = 0;
}

static void sh1107_process_data(sh1107_state_t *state, uint8_t value)
{
  uint32_t column = !state->segment_remap ? state->active_column : state->width - 1 - state->active_column;
  uint32_t target = state->active_page * state->width + column;
  state->pixels[target] = value;

  // Memory modes are explained in pages 34-35 of the datasheet,
  // and determine how the order of writing the pixels to the
  // display RAM.
  switch (state->memory_mode)
  {
  case CMD_SET_PAGE_ADDR_MODE:
    state->active_column++;
    if (state->active_column >= state->width) {
      state->active_column = 0;
    }
    break;

  case CMD_SET_VERTICAL_ADDR_MODE:
  default:
    state->active_page++;
    if (state->active_page >= 0x10)
    {
      state->active_page = 0;
      state->active_column++;
      if (state->active_column >= state->width) {
        state->active_column = 0;
      }
    }
    break;
  }
  sh1107_schedule_update(state);
}

static bool sh1107_i2c_connect(void *user_data, uint32_t address, bool connect)
{
  sh1107_state_t *state = user_data;
  state->control_byte = true;
  return true;
}

static uint8_t sh1107_i2c_read(void *user_data)
{
  return 0xff; // TODO
}

static bool sh1107_i2c_write(void *user_data, uint8_t value)
{
  sh1107_state_t *state = user_data;
  if (state->control_byte)
  {
    state->command_mode = !(value & SH1107_CONTROL_DC);
    state->continuous_mode = !(value & SH1107_CONTROL_CO);
    state->control_byte = false;
  }
  else
  {
    if (state->command_mode)
    {
      state->current_command[state->current_command_index] = value;
      if (!state->current_command_index)
      {
        state->current_command_length = 1 + multi_byte_commands[value];
      }
      state->current_command_index++;
      if (state->current_command_index < state->current_command_length)
      {
        // Wait for the next command byte
        return true;
      }
      sh1107_process_command(state);
    }
    else
    {
      sh1107_process_data(state, value);
    }
    if (!state->continuous_mode)
    {
      state->control_byte = true;
    }
  }
  return true;
}

void chip_init(void)
{
  sh1107_state_t *chip = malloc(sizeof(sh1107_state_t));

  sh1107_reset(chip);

  const i2c_config_t i2c = {
    .address = 0x3c,
    .scl = pin_init("SCL", INPUT_PULLUP),
    .sda = pin_init("SDA", INPUT_PULLUP),
    .connect = sh1107_i2c_connect,
    .read = sh1107_i2c_read,
    .write = sh1107_i2c_write,
    .user_data = chip,
  };

  i2c_init(&i2c);

  const timer_config_t update_timer_config = {
    .callback = sh1107_update_buffer,
    .user_data = chip,
  };
  chip->update_timer = timer_init(&update_timer_config);

  chip->framebuffer = framebuffer_init(&chip->width, &chip->height);
}
