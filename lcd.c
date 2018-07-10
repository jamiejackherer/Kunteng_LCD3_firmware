/*
 * LCD3 firmware
 *
 * Copyright (C) Casainho, 2018.
 *
 * Released under the GPL License, Version 3
 */

#include <string.h>
#include "stm8s.h"
#include "stm8s_gpio.h"
#include "gpio.h"
#include "timers.h"
#include "ht162.h"
#include "lcd.h"
#include "adc.h"
#include "main.h"
#include "config.h"
#include "button.h"
#include "eeprom.h"
#include "pins.h"
#include "uart.h"

#define LCD_MENU_CONFIG_SUBMENU_MAX_NUMBER 1

uint8_t ui8_lcd_frame_buffer[LCD_FRAME_BUFFER_SIZE];

uint8_t ui8_lcd_field_offset[] = {
    ASSIST_LEVEL_DIGIT_OFFSET,
    ODOMETER_DIGIT_OFFSET,
    TEMPERATURE_DIGIT_OFFSET,
    WHEEL_SPEED_OFFSET,
    BATTERY_POWER_DIGIT_OFFSET,
    0
};

uint8_t ui8_lcd_digit_mask[] = {
    NUMBER_0_MASK,
    NUMBER_1_MASK,
    NUMBER_2_MASK,
    NUMBER_3_MASK,
    NUMBER_4_MASK,
    NUMBER_5_MASK,
    NUMBER_6_MASK,
    NUMBER_7_MASK,
    NUMBER_8_MASK,
    NUMBER_9_MASK
};

uint8_t ui8_lcd_digit_mask_inverted[] = {
    NUMBER_0_MASK_INVERTED,
    NUMBER_1_MASK_INVERTED,
    NUMBER_2_MASK_INVERTED,
    NUMBER_3_MASK_INVERTED,
    NUMBER_4_MASK_INVERTED,
    NUMBER_5_MASK_INVERTED,
    NUMBER_6_MASK_INVERTED,
    NUMBER_7_MASK_INVERTED,
    NUMBER_8_MASK_INVERTED,
    NUMBER_9_MASK_INVERTED
};

static uint32_t ui32_battery_voltage_accumulated = 0;
static uint16_t ui16_battery_voltage_filtered_x10;

static uint16_t ui16_battery_current_accumulated = 0;
static uint16_t ui16_battery_current_filtered_x5;

static uint16_t ui16_battery_power_accumulated = 0;
static uint16_t ui16_battery_power_filtered_x50;
static uint16_t ui16_battery_power_filtered;

static uint32_t ui32_wh_sum_x5 = 0;
static uint32_t ui32_wh_sum_counter = 0;
static uint32_t ui32_wh_x10 = 0;
static uint8_t ui8_config_wh_x10_offset;

static uint32_t ui32_torque_sensor_force_x1000;
static uint32_t ui32_torque_accumulated = 0;
static uint16_t ui32_torque_accumulated_filtered_x10;

static uint8_t ui8_motor_controller_init = 1;

static uint8_t ui8_lights_state = 0;
static uint8_t lcd_lights_symbol = 0;
static uint8_t lcd_backlight_intensity = 0;

static uint8_t ui8_lcd_menu = 0;
static uint8_t ui8_lcd_menu_config_submenu_0_state = 0;
static uint8_t ui8_lcd_menu_config_submenu_1_state = 0;
static uint8_t ui8_lcd_menu_flash_counter = 0;
static uint8_t ui8_lcd_menu_flash_state;
static uint8_t ui8_lcd_menu_config_submenu_number = 0;
static uint8_t ui8_lcd_menu_config_submenu_active = 0;

static struct_motor_controller_data motor_controller_data;
static struct_configuration_variables configuration_variables;

void low_pass_filter_battery_voltage_current_power (void);
void lcd_enable_motor_symbol (uint8_t ui8_state);
void lcd_enable_lights_symbol (uint8_t ui8_state);
void lcd_enable_walk_symbol (uint8_t ui8_state);
void lcd_enable_km_symbol (uint8_t ui8_state);
void lcd_enable_wheel_speed_point_symbol (uint8_t ui8_state);
void lcd_enable_kmh_symbol (uint8_t ui8_state);
void lcd_enable_mph_symbol (uint8_t ui8_state);
void calc_wh (void);
void assist_level_state (void);
void brake (void);
void odometer (void);
void wheel_speed (void);
void power (void);
void power_off_management (void);
uint8_t first_time_management (void);
void temperature (void);
void battery_soc (void);
void low_pass_filter_pedal_torque (void);
void lights_state (void);
void lcd_set_backlight_intensity (uint8_t ui8_intensity);
void walk_assist_state (void);
void lcd_execute_main_screen (void);
void lcd_execute_menu_config (void);
void lcd_execute_menu_config_power (void);
void lcd_execute_menu_config_submenu_0 (void);
void lcd_execute_menu_config_submenu_1 (void);
void update_menu_flashing_state (void);

void clock_lcd (void)
{
  lcd_clear (); // start by clear LCD
  if (first_time_management ())
    return;

  update_menu_flashing_state ();

  // enter menu configurations: UP + DOWN click event
  if (get_button_up_down_click_event () &&
      ui8_lcd_menu != 1)
  {
    clear_button_up_down_click_event ();
    ui8_lcd_menu = 1;
  }

  // enter in menu set power: ONOFF + UP click event
  if (get_button_onoff_state () &&
      get_button_up_state ())
  {
    button_clear_events ();
    ui8_lcd_menu = 2;
  }

  switch (ui8_lcd_menu)
  {
    case 0:
      lcd_execute_main_screen ();
    break;

    case 1:
      lcd_execute_menu_config ();
    break;

    case 2:
      lcd_execute_menu_config_power ();
    break;
  }

  low_pass_filter_battery_voltage_current_power ();
  low_pass_filter_pedal_torque ();
  calc_wh ();

  lcd_update ();

  // power off system: ONOFF long click event
  power_off_management ();
}

void lcd_execute_main_screen (void)
{
  assist_level_state ();
  odometer ();
  temperature ();
  wheel_speed ();
  walk_assist_state ();
  power ();
  battery_soc ();
  lights_state ();
  brake ();
}

void lcd_execute_menu_config (void)
{
  // button check when submenu is not active
  if (!ui8_lcd_menu_config_submenu_active)
  {
    // leave config menu with a button_onoff_long_click
    if (get_button_onoff_long_click_event ())
    {
      clear_button_onoff_long_click_event ();
      ui8_lcd_menu_config_submenu_0_state = 0;
      ui8_lcd_menu = 0;

      // save the updated variables on EEPROM
      eeprom_write_variables_values ();

      return;
    }

    // check for change of submenu number
    if (get_button_up_click_event ())
    {
      clear_button_up_click_event ();

      if (ui8_lcd_menu_config_submenu_number < LCD_MENU_CONFIG_SUBMENU_MAX_NUMBER)
        ui8_lcd_menu_config_submenu_number++;
    }

    if (get_button_down_click_event ())
    {
      clear_button_down_click_event ();

      if (ui8_lcd_menu_config_submenu_number > 0)
        ui8_lcd_menu_config_submenu_number--;
    }

    // check if we should enter a submenu
    if (get_button_onoff_click_event ())
    {
      clear_button_onoff_click_event ();

      ui8_lcd_menu_config_submenu_active = 1;
      ui8_config_wh_x10_offset = 1;
    }

    // print submenu number only half of the time
    if (ui8_lcd_menu_flash_state)
    {
      lcd_print (ui8_lcd_menu_config_submenu_number * 10, WHEEL_SPEED_FIELD, 1);
    }
  }
  // ui8_lcd_menu_config_submenu_active == 1
  else
  {
    switch (ui8_lcd_menu_config_submenu_number)
    {
      case 0:
        lcd_execute_menu_config_submenu_0 ();
      break;

      case 1:
        lcd_execute_menu_config_submenu_1 ();
      break;

      default:
        ui8_lcd_menu_config_submenu_number = 0;
      break;
    }

    // leave config menu with a button_onoff_long_click
    if (get_button_onoff_long_click_event ())
    {
      clear_button_onoff_long_click_event ();

      ui8_lcd_menu_config_submenu_active = 0;
    }
  }
}

void lcd_execute_menu_config_submenu_0 (void)
{
  switch (ui8_lcd_menu_config_submenu_0_state)
  {
    // menu to choose max wheel speed
    case 0:
      if (get_button_up_click_event ())
      {
        clear_button_up_click_event ();
        if (configuration_variables.ui8_max_speed < 99)
          configuration_variables.ui8_max_speed++;
      }

      if (get_button_down_click_event ())
      {
        clear_button_down_click_event ();
        if (configuration_variables.ui8_max_speed > 2)
          configuration_variables.ui8_max_speed--;
      }

      if (get_button_onoff_click_event ())
      {
        clear_button_onoff_click_event ();
        ui8_lcd_menu_config_submenu_0_state = 1;
      }

      // print wheel speed only half of the time
      if (ui8_lcd_menu_flash_state)
      {
        lcd_print (configuration_variables.ui8_max_speed * 10, WHEEL_SPEED_FIELD, 0);
      }

      lcd_enable_kmh_symbol (1);
    break;

    // menu to choose wheel size
    case 1:
      if (get_button_up_click_event ())
      {
        clear_button_up_click_event ();
        // max value is 30 inchs wheel
        if (configuration_variables.ui8_wheel_size < 30)
          configuration_variables.ui8_wheel_size++;
      }

      if (get_button_down_click_event ())
      {
        clear_button_down_click_event ();
        // min value is 16 inchs wheel
        if (configuration_variables.ui8_wheel_size > 16)
          configuration_variables.ui8_wheel_size--;
      }

      if (get_button_onoff_click_event ())
      {
        clear_button_onoff_click_event ();
        ui8_lcd_menu_config_submenu_0_state = 2;
      }

      if (ui8_lcd_menu_flash_state)
      {
        // if wheel size is 27, print on LCD "700" instead
        if (configuration_variables.ui8_wheel_size != 27)
          lcd_print (configuration_variables.ui8_wheel_size * 10, ODOMETER_FIELD, 1);
        else
          lcd_print (700 * 10, ODOMETER_FIELD, 1);
      }
    break;

    // menu to choose Km/h or MP/h
    case 2:
      if (get_button_up_click_event ())
      {
        clear_button_up_click_event ();
        if (configuration_variables.ui8_units_type < 1)
          configuration_variables.ui8_units_type++;
      }

      if (get_button_down_click_event ())
      {
        clear_button_down_click_event ();
        if (configuration_variables.ui8_units_type > 0)
          configuration_variables.ui8_units_type--;
      }

      if (get_button_onoff_click_event ())
      {
        clear_button_onoff_click_event ();
        ui8_lcd_menu_config_submenu_0_state = 0;
      }

      if (ui8_lcd_menu_flash_state)
      {
        if (configuration_variables.ui8_units_type)
          lcd_enable_mph_symbol (1);
        else
          lcd_enable_kmh_symbol (1);
      }
    break;
  }
}

void lcd_execute_menu_config_submenu_1 (void)
{
  uint8_t ui8_temp;

  switch (ui8_lcd_menu_config_submenu_1_state)
  {
    // menu to enable/disable show of numeric watts hour value
    case 0:
      if (get_button_up_click_event ())
      {
        clear_button_up_click_event ();
        configuration_variables.ui8_show_numeric_battery_soc |= 1;
      }

      if (get_button_down_click_event ())
      {
        clear_button_down_click_event ();
        configuration_variables.ui8_show_numeric_battery_soc &= ~1;
      }

      if (get_button_onoff_click_event ())
      {
        clear_button_onoff_click_event ();
        ui8_lcd_menu_config_submenu_1_state = 1;
      }

      ui8_temp = ((configuration_variables.ui8_show_numeric_battery_soc & 1) ? 10 : 0);
      if (ui8_lcd_menu_flash_state)
      {
        lcd_print (ui8_temp, ODOMETER_FIELD, 1);
      }
    break;

    // menu to enable/disable show of numeric watts hour value in incrementing or decementing percentage
    case 1:
      if (get_button_up_click_event ())
      {
        clear_button_up_click_event ();
        configuration_variables.ui8_show_numeric_battery_soc |= 2;
      }

      if (get_button_down_click_event ())
      {
        clear_button_down_click_event ();
        configuration_variables.ui8_show_numeric_battery_soc &= ~2;
      }

      if (get_button_onoff_click_event ())
      {
        clear_button_onoff_click_event ();
        ui8_lcd_menu_config_submenu_1_state = 2;
      }

      ui8_temp = ((configuration_variables.ui8_show_numeric_battery_soc & 2) ? 10 : 0);
      if (ui8_lcd_menu_flash_state)
      {
        lcd_print (ui8_temp, ODOMETER_FIELD, 1);
      }
    break;

    // menu to choose watts hour value to be equal to 100% of battery SOC
    case 2:
      if (get_button_up_click_event ())
      {
        button_clear_events ();

        // increment at 10 units
        if (configuration_variables.ui32_wh_x10_100_percent < 100000)
          configuration_variables.ui32_wh_x10_100_percent += 100;
      }

      if (get_button_down_click_event ())
      {
        button_clear_events ();

        if (configuration_variables.ui32_wh_x10_100_percent >= 100)
          configuration_variables.ui32_wh_x10_100_percent -= 100;
        else if (configuration_variables.ui32_wh_x10_100_percent < 100)
          configuration_variables.ui32_wh_x10_100_percent = 0;
      }

      if (get_button_onoff_click_event ())
      {
        clear_button_onoff_click_event ();
        ui8_lcd_menu_config_submenu_1_state = 3;
      }

      if (ui8_lcd_menu_flash_state)
      {
        lcd_print (configuration_variables.ui32_wh_x10_100_percent, ODOMETER_FIELD, 0);
      }
    break;

    // menu to set current watts hour value
    case 3:
      // on the very first time, use current value of ui32_wh_x10
      if (ui8_config_wh_x10_offset)
      {
        ui8_config_wh_x10_offset = 0;
        configuration_variables.ui32_wh_x10_offset = ui32_wh_x10;
      }
      // keep reseting this values
      ui32_wh_sum_x5 = 0;
      ui32_wh_sum_counter = 0;
      ui32_wh_x10 = 0;

      if (get_button_up_click_event ())
      {
        button_clear_events ();

        // increment at 10 units
        configuration_variables.ui32_wh_x10_offset += 100;
      }

      if (get_button_down_click_event ())
      {
        button_clear_events ();

        if (configuration_variables.ui32_wh_x10_offset >= 100)
          configuration_variables.ui32_wh_x10_offset -= 100;
        else if (configuration_variables.ui32_wh_x10_offset < 100)
          configuration_variables.ui32_wh_x10_offset = 0;
      }

      if (get_button_onoff_click_event ())
      {
        clear_button_onoff_click_event ();
        ui8_lcd_menu_config_submenu_1_state = 0;
      }

      if (ui8_lcd_menu_flash_state)
      {
        lcd_print (configuration_variables.ui32_wh_x10_offset, ODOMETER_FIELD, 0);
      }
    break;
  }

  lcd_print (ui8_lcd_menu_config_submenu_1_state * 10, WHEEL_SPEED_FIELD, 1);
}

void lcd_execute_menu_config_power (void)
{
  // because this click envent can happens and will block the detection of button_onoff_long_click_event
  clear_button_onoff_click_event ();

  // leave this menu with a button_onoff_long_click
  if (get_button_onoff_long_click_event ())
  {
    button_clear_events ();
    ui8_lcd_menu = 0;

    // save the updated variables on EEPROM
    eeprom_write_variables_values ();
  }

  if (get_button_up_click_event ())
  {
    button_clear_events ();
    if (configuration_variables.ui8_target_max_battery_power < 195) // the BATTERY_POWER_FIELD can't show higher value
      configuration_variables.ui8_target_max_battery_power += 5;
  }

  if (get_button_down_click_event ())
  {
    button_clear_events ();
    if (configuration_variables.ui8_target_max_battery_power > 5)
      configuration_variables.ui8_target_max_battery_power -= 5;
  }

  if (ui8_lcd_menu_flash_state)
  {
    lcd_print (configuration_variables.ui8_target_max_battery_power * 10, BATTERY_POWER_FIELD, 0);
  }
}

uint8_t first_time_management (void)
{
  uint8_t ui8_status = 0;

  // don't update LCD up to we get first communication package from the motor controller
  if (ui8_motor_controller_init &&
      (uart_received_first_package () == 0))
  {
    ui8_status = 1;
  }
  // this will be executed only 1 time at startup
  else if (ui8_motor_controller_init)
  {
    ui8_motor_controller_init = 0;

    // wait for a first good read value of ADC: voltage can't be 0
    while (ui16_adc_read_battery_voltage_10b () == 0) ;

    // reset Wh value if battery is over 54.4V (when battery is near fully charged)
    if (((uint32_t) ui16_adc_read_battery_voltage_10b () * ADC_BATTERY_VOLTAGE_PER_ADC_STEP_X10000) > 544000)
    {
      configuration_variables.ui32_wh_x10_offset = 0;
    }
  }

  return ui8_status;
}

void power_off_management (void)
{
  // turn off
  if (get_button_onoff_long_click_event ())
  {
    // save values to EEPROM
    configuration_variables.ui32_wh_x10_offset = ui32_wh_x10;
    eeprom_write_variables_values ();

    // clear LCD so it is clear to user what is happening
    lcd_clear ();
    lcd_update ();

    // now disable the power to all the system
    GPIO_WriteLow(LCD3_ONOFF_POWER__PORT, LCD3_ONOFF_POWER__PIN);

    // block here
    while (1) ;
  }
}

void temperature (void)
{
  uint32_t ui32_temp;

  if (configuration_variables.ui8_show_numeric_battery_soc & 1)
  {
    ui32_temp = ui32_wh_x10 * 100;
    if (configuration_variables.ui32_wh_x10_100_percent > 0)
    {
      ui32_temp /= configuration_variables.ui32_wh_x10_100_percent;
    }
    else
    {
      ui32_temp = 0;
    }

    // show 100% - current SOC or just current SOC
    if (configuration_variables.ui8_show_numeric_battery_soc & 2)
    {
      if (ui32_temp > 100)
        ui32_temp = 100;

      lcd_print (100 - ui32_temp, TEMPERATURE_FIELD, 0);
    }
    else
    {
      lcd_print (ui32_temp, TEMPERATURE_FIELD, 0);
    }
  }
}

void battery_soc (void)
{
  static uint8_t ui8_timmer_counter;
  static uint8_t ui8_battery_level;

  // update battery level value only at every 100ms / 10 times per second and this helps to visual filter the fast changing values
  if (ui8_timmer_counter++ >= 10)
  {
    ui8_timmer_counter = 0;
    ui8_battery_level = motor_controller_data.ui8_battery_level;
  }

  // battery SOC
  lcd_enable_battery_symbols (ui8_battery_level);
}
void power (void)
{
  lcd_print (ui16_battery_power_filtered, BATTERY_POWER_FIELD, 0);
  lcd_enable_motor_symbol (1);
  lcd_enable_w_symbol (1);
}

void assist_level_state (void)
{
  if (get_button_up_click_event ())
  {
    clear_button_up_click_event ();

    if (configuration_variables.ui8_assist_level < 5)
      configuration_variables.ui8_assist_level++;
  }

  if (get_button_down_click_event ())
  {
    clear_button_down_click_event ();

    if (configuration_variables.ui8_assist_level > 0)
      configuration_variables.ui8_assist_level--;
  }

  lcd_print (configuration_variables.ui8_assist_level, ASSIST_LEVEL_FIELD, 0);
  lcd_enable_assist_symbol (1);
}

void lights_state (void)
{
  if (get_button_up_long_click_event ())
  {
    clear_button_up_long_click_event ();

    if (ui8_lights_state == 0)
    {
      ui8_lights_state = 1;
      lcd_lights_symbol = 1;
      lcd_backlight_intensity = 5;
      motor_controller_data.ui8_lights = 1;
    }
    else
    {
      ui8_lights_state = 0;
      lcd_lights_symbol = 0;
      lcd_backlight_intensity = 0;
      motor_controller_data.ui8_lights = 0;
    }
  }

  lcd_enable_lights_symbol (lcd_lights_symbol);
  lcd_set_backlight_intensity (lcd_backlight_intensity); // TODO: implement backlight intensity control
}

void walk_assist_state (void)
{
  if (get_button_down_long_click_event ())
  {
    // user need to keep pressing the button to have walk assist
    if (get_button_down_state ())
    {
      motor_controller_data.ui8_walk_assist_level = 1;
      lcd_enable_walk_symbol (1);
    }
    else
    {
      motor_controller_data.ui8_walk_assist_level = 0;
      clear_button_down_long_click_event ();
    }
  }
}

void brake (void)
{
  if (motor_controller_data.ui8_motor_controller_state_2 & 1) { lcd_enable_brake_symbol (1); }
  else { lcd_enable_brake_symbol (0); }
}

void odometer (void)
{
  // odometer values
  if (get_button_onoff_click_event ())
  {
    clear_button_onoff_click_event ();
    configuration_variables.ui8_odometer_field_state = (configuration_variables.ui8_odometer_field_state + 1) % 6;
  }

  switch (configuration_variables.ui8_odometer_field_state)
  {
    // Wh value
    case 0:
      lcd_print (ui32_wh_x10, ODOMETER_FIELD, 0);
      lcd_enable_vol_symbol (0);
    break;

    // voltage value
    case 1:
      lcd_print (ui16_battery_voltage_filtered_x10, ODOMETER_FIELD, 0);
      lcd_enable_vol_symbol (1);
    break;

    // current value
    case 2:
      lcd_print (ui16_battery_current_filtered_x5 << 1, ODOMETER_FIELD, 0);
      lcd_enable_vol_symbol (0);
    break;

    // pedal torque in Nm
    case 3:
      lcd_print (ui32_torque_sensor_force_x1000 / 100, ODOMETER_FIELD, 1);
      lcd_enable_vol_symbol (0);
    break;

    // pedal power in watts
    case 4:
      lcd_print (ui32_torque_accumulated_filtered_x10, ODOMETER_FIELD, 1);
      lcd_enable_vol_symbol (0);
    break;

    // pedal cadence value
    case 5:
      lcd_print (motor_controller_data.ui8_pedal_cadence * 10, ODOMETER_FIELD, 1);
      lcd_enable_vol_symbol (0);
    break;

    default:
    configuration_variables.ui8_odometer_field_state = 0;
    break;
  }
}

void wheel_speed (void)
{
  uint16_t ui16_wheel_perimeter;
  uint16_t ui16_wheel_speed_10;
  float f_wheel_speed_x10;

  // wheel perimeter size calculation:
  // P (mm) = rim diameter * inchs_to_mm * pi
  // example: P (mm) = 26 * 25.4 * 3.14 = 2073
  ui16_wheel_perimeter = configuration_variables.ui8_wheel_size * 80;

  // speed value
  // the value sent by the controller is for MPH and not KMH...
  // (1÷((controller_sent_time÷3600)÷wheel_perimeter)÷1.6)
  // Mph
  f_wheel_speed_x10 = 2.25 / (((float) motor_controller_data.ui16_wheel_inverse_rps) / ((float) ui16_wheel_perimeter));

  // km/h
  if (configuration_variables.ui8_units_type == 1)
  {
    f_wheel_speed_x10 /= 1.40625; // 2.25 / 1.6 = 1.40625
  }
  ui16_wheel_speed_10 = (uint16_t) f_wheel_speed_x10;

  // if wheel is stopped, reset speed value
  if (motor_controller_data.ui8_motor_controller_state_2 & 128)  { ui16_wheel_speed_10 = 0; }

  lcd_print (ui16_wheel_speed_10, WHEEL_SPEED_FIELD, 0);
  if (configuration_variables.ui8_units_type)
    lcd_enable_mph_symbol (1);
  else
    lcd_enable_kmh_symbol (1);
}

void lcd_clear (void)
{
  memset(ui8_lcd_frame_buffer, 0, LCD_FRAME_BUFFER_SIZE);
}

void lcd_set_frame_buffer (void)
{
  memset(ui8_lcd_frame_buffer, 255, LCD_FRAME_BUFFER_SIZE);
}

void lcd_update (void)
{
  ht1622_send_frame_buffer (ui8_lcd_frame_buffer);
}

void lcd_print (uint32_t ui32_number, uint8_t ui8_lcd_field, uint8_t ui8_options)
{
  uint8_t ui8_digit;
  uint8_t ui8_counter;

  // first delete the field
  for (ui8_counter = 0; ui8_counter < 5; ui8_counter++)
  {
    if (ui8_lcd_field == ASSIST_LEVEL_FIELD ||
            ui8_lcd_field == ODOMETER_FIELD ||
            ui8_lcd_field == TEMPERATURE_FIELD)
    {
      ui8_lcd_frame_buffer[ui8_lcd_field_offset[ui8_lcd_field] - ui8_counter] &= NUMBERS_MASK;
    }

    // because the LCD mask/layout is different on some field, like numbers would be inverted
    if (ui8_lcd_field == WHEEL_SPEED_FIELD ||
        ui8_lcd_field == BATTERY_POWER_FIELD)
    {
      ui8_lcd_frame_buffer[ui8_lcd_field_offset[ui8_lcd_field] + ui8_counter] &= NUMBERS_MASK;
    }

    // limit the number of printed digits for each field
    if (ui8_counter == 0 && ui8_lcd_field == ASSIST_LEVEL_FIELD) break;
    if (ui8_counter == 4 && ui8_lcd_field == ODOMETER_FIELD) break;
    if (ui8_counter == 1 && ui8_lcd_field == TEMPERATURE_FIELD) break;
    if (ui8_counter == 2 && ui8_lcd_field == WHEEL_SPEED_FIELD) break;
    if (ui8_counter == 2 && ui8_lcd_field == BATTERY_POWER_FIELD) break;
  }

  // enable only the "1" if power is >= 1000
  if (ui8_lcd_field == BATTERY_POWER_FIELD)
  {
    if (ui32_number >= 1000) { lcd_enable_battery_power_1_symbol (1); }
    else { lcd_enable_battery_power_1_symbol (0); }
  }

  // enable only the "1" if temperature is >= 100
  if (ui8_lcd_field == TEMPERATURE_FIELD)
  {
    if (ui32_number >= 100) { lcd_enable_temperature_1_symbol (1); }
    else { lcd_enable_temperature_1_symbol (0); }
  }

  // do not show the point symbol if number*10 is integer
  if (ui8_options == 1)
  {
    if (ui8_lcd_field == ODOMETER_FIELD) { lcd_enable_odometer_point_symbol (0); }
    else if (ui8_lcd_field == WHEEL_SPEED_FIELD) { lcd_enable_wheel_speed_point_symbol (0); }
  }
  else
  {
    if (ui8_lcd_field == ODOMETER_FIELD) { lcd_enable_odometer_point_symbol (1); }
    else if (ui8_lcd_field == WHEEL_SPEED_FIELD) { lcd_enable_wheel_speed_point_symbol (1); }
  }

  for (ui8_counter = 0; ui8_counter < 5; ui8_counter++)
  {
    ui8_digit = ui32_number % 10;

    if (ui8_lcd_field == ASSIST_LEVEL_FIELD ||
            ui8_lcd_field == ODOMETER_FIELD ||
            ui8_lcd_field == TEMPERATURE_FIELD)
    {

      if ((ui8_options == 1) &&
          (ui8_counter == 0))
      {
        ui8_lcd_frame_buffer[ui8_lcd_field_offset[ui8_lcd_field] - ui8_counter] &= ui8_lcd_digit_mask[NUMBERS_MASK];
      }
      // print empty (NUMBERS_MASK) when ui32_number = 0
      else if ((ui8_counter > 1 && ui32_number == 0) ||
          // TEMPERATURE_FIELD: print 1 zero only when value is less than 10
          (ui8_lcd_field == TEMPERATURE_FIELD && ui8_counter > 0 && ui32_number == 0))
      {
        ui8_lcd_frame_buffer[ui8_lcd_field_offset[ui8_lcd_field] - ui8_counter] &= ui8_lcd_digit_mask[NUMBERS_MASK];
      }
      else
      {
        ui8_lcd_frame_buffer[ui8_lcd_field_offset[ui8_lcd_field] - ui8_counter] |= ui8_lcd_digit_mask[ui8_digit];
      }
    }

    // because the LCD mask/layout is different on some field, like numbers would be inverted
    if (ui8_lcd_field == WHEEL_SPEED_FIELD ||
        ui8_lcd_field == BATTERY_POWER_FIELD)
    {
      if (ui8_lcd_field == WHEEL_SPEED_FIELD)
      {
        if ((ui8_options == 1) &&
            (ui8_counter == 0))
        {
          ui8_lcd_frame_buffer[ui8_lcd_field_offset[ui8_lcd_field] + ui8_counter] &= ui8_lcd_digit_mask[NUMBERS_MASK];
        }
        // print only first 2 zeros
        else if (ui8_counter > 1 && ui32_number == 0)
        {
          ui8_lcd_frame_buffer[ui8_lcd_field_offset[ui8_lcd_field] + ui8_counter] &= ui8_lcd_digit_mask[NUMBERS_MASK];
        }
        else
        {
          ui8_lcd_frame_buffer[ui8_lcd_field_offset[ui8_lcd_field] + ui8_counter] |= ui8_lcd_digit_mask_inverted[ui8_digit];
        }
      }

      if (ui8_lcd_field == BATTERY_POWER_FIELD)
      {
        // print only first zero
        if (ui8_counter > 0 && ui32_number == 0)
        {
          ui8_lcd_frame_buffer[ui8_lcd_field_offset[ui8_lcd_field] + ui8_counter] &= ui8_lcd_digit_mask[NUMBERS_MASK];
        }
        else
        {
          ui8_lcd_frame_buffer[ui8_lcd_field_offset[ui8_lcd_field] + ui8_counter] |= ui8_lcd_digit_mask_inverted[ui8_digit];
        }
      }
    }

    // limit the number of printed digits for each field
    if (ui8_counter == 0 && ui8_lcd_field == ASSIST_LEVEL_FIELD) break;
    if (ui8_counter == 4 && ui8_lcd_field == ODOMETER_FIELD) break;
    if (ui8_counter == 1 && ui8_lcd_field == TEMPERATURE_FIELD) break;
    if (ui8_counter == 2 && ui8_lcd_field == WHEEL_SPEED_FIELD) break;
    if (ui8_counter == 2 && ui8_lcd_field == BATTERY_POWER_FIELD) break;

    ui32_number /= 10;
  }
}

void lcd_enable_w_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[9] |= 128;
  else
    ui8_lcd_frame_buffer[9] &= ~128;
}

void lcd_enable_odometer_point_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[6] |= 8;
  else
    ui8_lcd_frame_buffer[6] &= ~8;
}

void lcd_enable_brake_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[23] |= 4;
  else
    ui8_lcd_frame_buffer[23] &= ~4;
}

void lcd_enable_lights_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[23] |= 2;
  else
    ui8_lcd_frame_buffer[23] &= ~2;
}

void lcd_enable_cruise_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[0] |= 16;
  else
    ui8_lcd_frame_buffer[0] &= ~16;
}

void lcd_enable_assist_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[1] |= 8;
  else
    ui8_lcd_frame_buffer[1] &= ~8;
}

void lcd_enable_vol_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[2] |= 8;
  else
    ui8_lcd_frame_buffer[2] &= ~8;
}

void lcd_enable_odo_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[3] |= 8;
  else
    ui8_lcd_frame_buffer[3] &= ~8;
}

void lcd_enable_km_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[4] |= 8;
  else
    ui8_lcd_frame_buffer[4] &= ~8;
}

void lcd_enable_mil_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[5] |= 8;
  else
    ui8_lcd_frame_buffer[5] &= ~8;
}

void lcd_enable_temperature_1_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[7] |= 8;
  else
    ui8_lcd_frame_buffer[7] &= ~8;
}

void lcd_enable_battery_power_1_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[12] |= 8;
  else
    ui8_lcd_frame_buffer[12] &= ~8;
}

void lcd_enable_temperature_minus_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[8] |= 8;
  else
    ui8_lcd_frame_buffer[8] &= ~8;
}

void lcd_enable_temperature_degrees_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[9] |= 16;
  else
    ui8_lcd_frame_buffer[9] &= ~16;
}

void lcd_enable_temperature_farneight_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[9] |= 32;
  else
    ui8_lcd_frame_buffer[9] &= ~32;
}

void lcd_enable_farneight_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[9] |= 1;
  else
    ui8_lcd_frame_buffer[9] &= ~1;
}

void lcd_enable_motor_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[9] |= 2;
  else
    ui8_lcd_frame_buffer[9] &= ~2;
}

void lcd_enable_degrees_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[9] |= 64;
  else
    ui8_lcd_frame_buffer[9] &= ~64;
}

void lcd_enable_kmh_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[13] |= 1;
  else
    ui8_lcd_frame_buffer[13] &= ~1;
}

void lcd_enable_wheel_speed_point_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[13] |= 8;
  else
    ui8_lcd_frame_buffer[13] &= ~8;
}

void lcd_enable_avs_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[13] |= 16;
  else
    ui8_lcd_frame_buffer[13] &= ~16;
}

void lcd_enable_mxs_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[13] |= 32;
  else
    ui8_lcd_frame_buffer[13] &= ~32;
}

void lcd_enable_walk_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[13] |= 64;
  else
    ui8_lcd_frame_buffer[13] &= ~64;
}

void lcd_enable_mph_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[13] |= 128;
  else
    ui8_lcd_frame_buffer[13] &= ~128;
}

void lcd_enable_dst_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[16] |= 8;
  else
    ui8_lcd_frame_buffer[16] &= ~8;
}

void lcd_enable_tm_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[17] |= 16;
  else
    ui8_lcd_frame_buffer[17] &= ~16;
}

void lcd_enable_ttm_symbol (uint8_t ui8_state)
{
  if (ui8_state)
    ui8_lcd_frame_buffer[17] |= 32;
  else
    ui8_lcd_frame_buffer[17] &= ~32;
}

void lcd_enable_battery_symbols (uint8_t ui8_state)
{
/*
  ui8_lcd_frame_buffer[23] |= 16;  // empty
  ui8_lcd_frame_buffer[23] |= 128; // bar number 1
  ui8_lcd_frame_buffer[23] |= 1;   // bar number 2
  ui8_lcd_frame_buffer[23] |= 64;  // bar number 3
  ui8_lcd_frame_buffer[23] |= 32;  // bar number 4
  */

  // first clean battery symbols
  ui8_lcd_frame_buffer[23] &= ~241;

  if (ui8_state <= 5)
    ui8_lcd_frame_buffer[23] |= 16;
  else if ((ui8_state > 5) && (ui8_state <= 10))
    ui8_lcd_frame_buffer[23] |= 144;
  else if ((ui8_state > 10) && (ui8_state <= 15))
    ui8_lcd_frame_buffer[23] |= 145;
  else if ((ui8_state > 15) && (ui8_state <= 20))
    ui8_lcd_frame_buffer[23] |= 209;
  else
    ui8_lcd_frame_buffer[23] |= 241;
}

void low_pass_filter_battery_voltage_current_power (void)
{
  // low pass filter battery voltage
  ui32_battery_voltage_accumulated -= ui32_battery_voltage_accumulated >> READ_BATTERY_VOLTAGE_FILTER_COEFFICIENT;
  ui32_battery_voltage_accumulated += (uint32_t) ui16_adc_read_battery_voltage_10b () * ADC_BATTERY_VOLTAGE_PER_ADC_STEP_X10000;
  ui16_battery_voltage_filtered_x10 = ((uint32_t) (ui32_battery_voltage_accumulated >> READ_BATTERY_VOLTAGE_FILTER_COEFFICIENT)) / 1000;

  // low pass filter batery current
  ui16_battery_current_accumulated -= ui16_battery_current_accumulated >> READ_BATTERY_CURRENT_FILTER_COEFFICIENT;
  ui16_battery_current_accumulated += (uint16_t) motor_controller_data.ui8_battery_current;
  ui16_battery_current_filtered_x5 = ui16_battery_current_accumulated >> READ_BATTERY_CURRENT_FILTER_COEFFICIENT;

  // battery power
  ui16_battery_power_filtered_x50 = ui16_battery_current_filtered_x5 * ui16_battery_voltage_filtered_x10;
  ui16_battery_power_filtered = ui16_battery_power_filtered_x50 / 50;

  // loose resolution under 10W
  if (ui16_battery_power_filtered < 200)
  {
    ui16_battery_power_filtered /= 10;
    ui16_battery_power_filtered *= 10;
  }
  // loose resolution under 20W
  else if (ui16_battery_power_filtered < 400)
  {
    ui16_battery_power_filtered /= 20;
    ui16_battery_power_filtered *= 20;
  }
  // loose resolution under 25W
  else
  {
    ui16_battery_power_filtered /= 25;
    ui16_battery_power_filtered *= 25;
  }
}

void low_pass_filter_pedal_torque (void)
{
  uint32_t ui32_torque_x10;
  uint32_t ui32_torque_filtered_x10;

  ui32_torque_sensor_force_x1000 = motor_controller_data.ui8_pedal_torque_sensor - motor_controller_data.ui8_pedal_torque_sensor_offset;
  if (ui32_torque_sensor_force_x1000 > 200) { ui32_torque_sensor_force_x1000 = 0; }
  ui32_torque_sensor_force_x1000 *= TORQUE_SENSOR_FORCE_SCALE_X1000;

  // calc now torque
  // P = force x rotations_seconds x 2 x pi
  ui32_torque_x10 = (ui32_torque_sensor_force_x1000 * motor_controller_data.ui8_pedal_cadence) / 955;

  // low pass filter
  ui32_torque_accumulated -= ui32_torque_accumulated >> TORQUE_FILTER_COEFFICIENT;
  ui32_torque_accumulated += ui32_torque_x10;
  ui32_torque_filtered_x10 = ((uint32_t) (ui32_torque_accumulated >> TORQUE_FILTER_COEFFICIENT));

  // loose resolution under 10W
  if (ui32_torque_filtered_x10 < 1000)
  {
    ui32_torque_accumulated_filtered_x10 = ui32_torque_filtered_x10 / 50;
    ui32_torque_accumulated_filtered_x10 *= 50;
  }
  // loose resolution under 20W
  else
  {
    ui32_torque_accumulated_filtered_x10 = ui32_torque_filtered_x10 / 100;
    ui32_torque_accumulated_filtered_x10 *= 100;
  }
}

void calc_wh (void)
{
  static uint8_t ui8_100ms_timmer_counter;
  static uint8_t ui8_1s_timmer_counter;
  uint32_t ui32_temp = 0;

  // calc wh every 100ms
  if (ui8_100ms_timmer_counter++ >= 10)
  {
    ui8_100ms_timmer_counter = 0;

    if (ui16_battery_power_filtered_x50 > 0)
    {
      ui32_wh_sum_x5 += ui16_battery_power_filtered_x50 / 10;
      ui32_wh_sum_counter++;
    }

    // calc at 1s rate
    if (ui8_1s_timmer_counter++ >= 10)
    {
      ui8_1s_timmer_counter = 0;

      // avoid  zero divisison
      if (ui32_wh_sum_counter != 0)
      {
        ui32_temp = ui32_wh_sum_counter / 36;
        ui32_temp = (ui32_temp * (ui32_wh_sum_x5 / ui32_wh_sum_counter)) / 500;
      }

      ui32_wh_x10 = configuration_variables.ui32_wh_x10_offset + ui32_temp;
    }
  }
}

struct_configuration_variables* get_configuration_variables (void)
{
  return &configuration_variables;
}

struct_motor_controller_data* lcd_get_motor_controller_data (void)
{
  return &motor_controller_data;
}

void lcd_init (void)
{
  ht1622_init ();
  lcd_set_frame_buffer ();
  lcd_update();

  // init variables with the stored value on EEPROM
  eeprom_read_values_to_variables ();
}

void lcd_set_backlight_intensity (uint8_t ui8_intensity)
{
  if ((ui8_intensity >= 0) && (ui8_intensity <= 9))
  {
    TIM1_SetCompare4 (ui8_intensity); // set background light
  }
}

void update_menu_flashing_state (void)
{
  if (ui8_lcd_menu_flash_counter++ > 50)
  {
    ui8_lcd_menu_flash_counter = 0;

    if (ui8_lcd_menu_flash_state)
      ui8_lcd_menu_flash_state = 0;
    else
      ui8_lcd_menu_flash_state = 1;
  }
}
