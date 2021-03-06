#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "user_config.h"
#include "user_interface.h"
#include "v7.h"
#include "mem.h"
#include "espconn.h"
#include <math.h>
#include <stdlib.h>

#include "v7_esp.h"
#include "dht11.h"
#include "util.h"
#include "v7_esp_features.h"
#include "v7_uart.h"

struct v7 *v7;
os_timer_t js_timeout_timer;

ICACHE_FLASH_ATTR static v7_val_t usleep(struct v7 *v7, v7_val_t this_obj,
                                         v7_val_t args) {
  v7_val_t usecsv = v7_array_get(v7, args, 0);
  int usecs;
  if (!v7_is_double(usecsv)) {
    printf("usecs is not a double\n\r");
    return v7_create_undefined();
  }
  usecs = v7_to_double(usecsv);
  os_delay_us(usecs);
  return v7_create_undefined();
}

ICACHE_FLASH_ATTR static void js_timeout() {
  v7_val_t cb = v7_get(v7, v7_get_global_object(v7), "_js_timeout_handler", 19);
  v7_val_t undef = v7_create_undefined();
  v7_apply(v7, cb, undef, undef);
}

/* Currently can only handle one timer */
ICACHE_FLASH_ATTR static v7_val_t set_timeout(struct v7 *v7, v7_val_t this_obj,
                                              v7_val_t args) {
  v7_val_t cb = v7_array_get(v7, args, 0);
  v7_val_t msecsv = v7_array_get(v7, args, 1);
  int msecs;

  if (!v7_is_function(cb)) {
    printf("cb is not a function\n");
    return v7_create_undefined();
  }
  if (!v7_is_double(msecsv)) {
    printf("msecs is not a double\n");
    return v7_create_undefined();
  }
  msecs = v7_to_double(msecsv);

  /*
   * used to convey the callback to the timer handler _and_ to root
   * the function so that the GC doesn't deallocate it.
   */
  v7_set(v7, v7_get_global_object(v7), "_js_timeout_handler", 19, 0, cb);
  os_timer_disarm(&js_timeout_timer);
  os_timer_setfn(&js_timeout_timer, js_timeout, NULL);
  os_timer_arm(&js_timeout_timer, msecs, 0);

  return v7_create_undefined();
}

ICACHE_FLASH_ATTR static v7_val_t GPIO_out(struct v7 *v7, v7_val_t this_obj,
                                           v7_val_t args) {
  v7_val_t pinv = v7_array_get(v7, args, 0);
  v7_val_t valv = v7_array_get(v7, args, 1);
  int pin, val;

  if (!v7_is_double(pinv)) {
    printf("non-numeric pin\n");
    return v7_create_undefined();
  }
  pin = v7_to_double(pinv);
  val = v7_is_true(v7, valv) ? 1 : 0;

  set_gpio(pin, val);

  return v7_create_undefined();
}

ICACHE_FLASH_ATTR static v7_val_t GPIO_in(struct v7 *v7, v7_val_t this_obj,
                                          v7_val_t args) {
  v7_val_t pinv = v7_array_get(v7, args, 0);
  int pin;

  if (!v7_is_double(pinv)) {
    printf("non-numeric pin\n");
    return v7_create_undefined();
  }
  pin = v7_to_double(pinv);
  return v7_create_boolean(read_gpio_pin(pin));
}

ICACHE_FLASH_ATTR static v7_val_t Wifi_connect(struct v7 *v7, v7_val_t this_obj,
                                               v7_val_t args) {
  (void) v7;
  (void) this_obj;
  (void) args;
  return v7_create_boolean(wifi_station_connect());
}

ICACHE_FLASH_ATTR static v7_val_t Wifi_disconnect(struct v7 *v7,
                                                  v7_val_t this_obj,
                                                  v7_val_t args) {
  (void) v7;
  (void) this_obj;
  (void) args;
  return v7_create_boolean(wifi_station_disconnect());
}

ICACHE_FLASH_ATTR static v7_val_t Wifi_setup(struct v7 *v7, v7_val_t this_obj,
                                             v7_val_t args) {
  struct station_config stationConf;
  v7_val_t ssidv = v7_array_get(v7, args, 0);
  v7_val_t passv = v7_array_get(v7, args, 1);
  const char *ssid, *pass;
  size_t ssid_len, pass_len;
  int res;

  if (!v7_is_string(ssidv) || !v7_is_string(passv)) {
    printf("ssid/pass are not strings\n");
    return v7_create_undefined();
  }

  wifi_station_disconnect();

  ssid = v7_to_string(v7, &ssidv, &ssid_len);
  pass = v7_to_string(v7, &passv, &pass_len);

  stationConf.bssid_set = 0;
  strncpy((char *) &stationConf.ssid, ssid, 32);
  strncpy((char *) &stationConf.password, pass, 64);

  res = v7_create_boolean(wifi_station_set_config(&stationConf));
  if (!res) {
    printf("Failed to set station config\n");
    return v7_create_boolean(0);
  }

  return v7_create_boolean(wifi_station_connect());
}

ICACHE_FLASH_ATTR static v7_val_t Wifi_status(struct v7 *v7, v7_val_t this_obj,
                                              v7_val_t args) {
  uint8 st = wifi_station_get_connect_status();
  const char *msg;

  (void) this_obj;
  (void) args;

  switch (st) {
    case STATION_IDLE:
      msg = "idle";
      break;
    case STATION_CONNECTING:
      msg = "connecting";
      break;
    case STATION_WRONG_PASSWORD:
      msg = "bad pass";
      break;
    case STATION_NO_AP_FOUND:
      msg = "no ap";
      break;
    case STATION_CONNECT_FAIL:
      msg = "connect failed";
      break;
    case STATION_GOT_IP:
      msg = "got ip";
      break;
  }
  return v7_create_string(v7, msg, strlen(msg), 1);
}

/*
 * Returns the IP address of an interface.
 *
 * Pass either Wifi.STATION or Wifi.SOFTAP
 * Defaults to Wifi.STATION if called without argument.
 */
ICACHE_FLASH_ATTR static v7_val_t Wifi_ip(struct v7 *v7, v7_val_t this_obj,
                                          v7_val_t args) {
  v7_val_t arg = v7_array_get(v7, args, 0);
  int err;
  struct ip_info info;
  char ip[17];

  (void) this_obj;
  (void) args;

  err = wifi_get_ip_info(v7_is_double(arg) ? v7_to_double(arg) : 0, &info);
  if (err == 0) {
    v7_throw(v7, "cannot get ip info");
  }
  snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
  return v7_create_string(v7, ip, strlen(ip), 1);
}

/*
 * Returns an object describing the free memory.
 */
ICACHE_FLASH_ATTR static v7_val_t GC_stat(struct v7 *v7, v7_val_t this_obj,
                                          v7_val_t args) {
  v7_val_t f = v7_create_object(v7);
  /* prevent the object from being potentially GCed */
  v7_set(v7, args, "_tmp", 4, 0, f);
  v7_set(v7, f, "sysfree", 7, 0, v7_create_number(system_get_free_heap_size()));
  return f;
}

/*
 * Force a pass of the garbage collector.
 */
ICACHE_FLASH_ATTR static v7_val_t GC_collect(struct v7 *v7, v7_val_t this_obj,
                                             v7_val_t args) {
  (void) this_obj;
  (void) args;

  v7_gc(v7);
  return v7_create_undefined();
}

#if V7_ESP_ENABLE__DHT11
ICACHE_FLASH_ATTR
static v7_val_t DHT11_read(struct v7 *v7, v7_val_t this_obj, v7_val_t args) {
  int pin, temp, rh;
  v7_val_t pinv = v7_array_get(v7, args, 0), result;

  if (!v7_is_double(pinv)) {
    printf("non-numeric pin\n");
    return v7_create_undefined();
  }
  pin = v7_to_double(pinv);

  if (!dht11_read(pin, &temp, &rh)) {
    return v7_create_null();
  }

  result = v7_create_object(v7);
  v7_set(v7, result, "temp", 4, 0, v7_create_number(temp));
  v7_set(v7, result, "rh", 2, 0, v7_create_number(rh));
  /* prevent the object from being potentially GCed */
  v7_set(v7, args, "_tmp", 4, 0, result);
  return result;
}
#endif /* V7_ESP_ENABLE__DHT11 */

/*
 * Sets output for debug messages.
 * Available modes are:
 * 0 - no debug output
 * 1 - print debug output to UART0 (V7's console)
 * 2 - print debug output to UART1
 */
ICACHE_FLASH_ATTR static v7_val_t Debug_set_output(struct v7 *v7,
                                                   v7_val_t this_obj,
                                                   v7_val_t args) {
  int mode, res;
  v7_val_t output_val = v7_array_get(v7, args, 0);

  if (!v7_is_double(output_val)) {
    printf("Output is not a number\n");
    return v7_create_undefined();
  }

  mode = v7_to_double(output_val);

  uart_debug_init(0, 0);
  res = uart_redirect_debug(mode);

  return v7_create_number(res < 0 ? res : mode);
}

/*
 * Prints message to current debug output
 */
ICACHE_FLASH_ATTR v7_val_t
Debug_print(struct v7 *v7, v7_val_t this_obj, v7_val_t args) {
  char *p, buf[512];
  int i, num_args = v7_array_length(v7, args);

  (void) this_obj;
  for (i = 0; i < num_args; i++) {
    v7_val_t arg = v7_array_get(v7, args, i);
    if (v7_is_string(arg)) {
      size_t n;
      const char *s = v7_to_string(v7, &arg, &n);
      os_printf("%s", s);
    } else {
      p = v7_to_json(v7, arg, buf, sizeof(buf));
      os_printf("%s", p);
      if (p != buf) {
        free(p);
      }
    }
  }
  os_printf("\n");

  return v7_create_null();
}

ICACHE_FLASH_ATTR void init_v7() {
  struct v7_create_opts opts;
  v7_val_t wifi, gpio, dht11, gc, debug;

  opts.object_arena_size = 94;
  opts.function_arena_size = 17;
  opts.property_arena_size = 340;

  v7 = v7_create_opt(opts);

  v7_set_method(v7, v7_get_global_object(v7), "usleep", usleep);
  v7_set_method(v7, v7_get_global_object(v7), "setTimeout", set_timeout);

  gpio = v7_create_object(v7);
  v7_set(v7, v7_get_global_object(v7), "GPIO", 4, 0, gpio);
  v7_set_method(v7, gpio, "in", GPIO_in);
  v7_set_method(v7, gpio, "out", GPIO_out);

  wifi = v7_create_object(v7);
  v7_set(v7, v7_get_global_object(v7), "Wifi", 4, 0, wifi);
  v7_set_method(v7, wifi, "setup", Wifi_setup);
  v7_set_method(v7, wifi, "disconnect", Wifi_disconnect);
  v7_set_method(v7, wifi, "connect", Wifi_connect);
  v7_set_method(v7, wifi, "status", Wifi_status);
  v7_set_method(v7, wifi, "ip", Wifi_ip);
  v7_set(v7, wifi, "STATION", 7, 0, v7_create_number(0));
  v7_set(v7, wifi, "SOFTAP", 6, 0, v7_create_number(1));

#if V7_ESP_ENABLE__DHT11
  dht11 = v7_create_object(v7);
  v7_set(v7, v7_get_global_object(v7), "DHT11", 5, 0, dht11);
  v7_set_method(v7, dht11, "read", DHT11_read);
#endif /* V7_ESP_ENABLE__DHT11 */

  gc = v7_create_object(v7);
  v7_set(v7, v7_get_global_object(v7), "GC", 2, 0, gc);
  v7_set_method(v7, gc, "stat", GC_stat);
  v7_set_method(v7, gc, "collect", GC_collect);

  debug = v7_create_object(v7);
  v7_set(v7, v7_get_global_object(v7), "Debug", 5, 0, debug);
  v7_set_method(v7, debug, "setOutput", Debug_set_output);
  v7_set_method(v7, debug, "print", Debug_print);

  v7_init_http_client(v7);
}
