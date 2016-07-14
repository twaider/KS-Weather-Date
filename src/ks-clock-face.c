#include <pebble.h>

#define COLORS       PBL_IF_COLOR_ELSE(true, false)
#define ANTIALIASING true

#define HAND_MARGIN  10
#define FINAL_RADIUS 60

#define ANIMATION_DURATION 500
#define ANIMATION_DELAY    600

#define SAFEMODE_ON  0
#define SAFEMODE_OFF 6

typedef struct {
  int hours;
  int minutes;
} Time;

static Window *s_main_window;
static Layer *s_canvas_layer;
static TextLayer *s_weather_layer;
static TextLayer *s_weathertext_layer;
static TextLayer *s_date_layer;

static GFont s_weather_font;
static GFont s_icon_font;

static GPoint s_center;
static Time s_last_time, s_anim_time;
static char s_last_date[16];
static int s_radius = 0, s_color_channels[3];
static int background_color;

static bool s_animating = false;
static bool weather_units_conf = false;
static bool weather_safemode_conf = true;
static bool weather_on_conf = false;
static bool background_on_conf = false;

/*************************** appMessage **************************/

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  // Store incoming information
  static char icon_buffer[8];
  static char temperature_buffer[8];
  static int temperature;

  // Read tuples for data
  Tuple *weather_units_tuple = dict_find(iterator, MESSAGE_KEY_UNITS);
  Tuple *weather_on_tuple = dict_find(iterator, MESSAGE_KEY_WEATHER_ON);
  Tuple *weather_safemode_tuple = dict_find(iterator, MESSAGE_KEY_WEATHER_SAFEMODE);
  Tuple *temp_tuple = dict_find(iterator, MESSAGE_KEY_TEMPERATURE);
  Tuple *icon_tuple = dict_find(iterator, MESSAGE_KEY_ICON);  
  Tuple *background_color_tuple = dict_find(iterator, MESSAGE_KEY_BACKGROUND_COLOR);
  Tuple *background_on_tuple = dict_find(iterator, MESSAGE_KEY_BACKGROUND_ON);

  // If we get weather option
  if ( weather_on_tuple ) {
    // Set weather flag
    weather_on_conf = (bool)weather_on_tuple->value->int16;
    persist_write_bool(MESSAGE_KEY_WEATHER_ON, weather_on_conf);
  }
  
  if ( weather_safemode_tuple ) {
      weather_safemode_conf = (bool)weather_safemode_tuple->value->int16;
      persist_write_bool(MESSAGE_KEY_WEATHER_SAFEMODE, weather_safemode_conf);
  }
  
  if ( weather_units_tuple ) {
      weather_units_conf = (bool)weather_units_tuple->value->int16;
      persist_write_bool(MESSAGE_KEY_UNITS, weather_units_conf);
  }

  // If all data is available, use it
  if ( temp_tuple && icon_tuple ) {
    // Assemble strings for temp and icon
    temperature = (float)temp_tuple->value->int32;
    
    if ( weather_units_conf ) {      
      snprintf(temperature_buffer, sizeof(temperature_buffer), "%d F", temperature);
    } else {
      snprintf(temperature_buffer, sizeof(temperature_buffer), "%d C", temperature);
    }
    
    snprintf(icon_buffer, sizeof(icon_buffer), "%s", icon_tuple->value->cstring);

    // Set temp and icon to text layers
    text_layer_set_text(s_weather_layer, icon_buffer);
    text_layer_set_text(s_weathertext_layer, temperature_buffer);
  }

  // If weather disabled, clear weather layers
  if ( !weather_on_conf ) {
    text_layer_set_text(s_weather_layer, "");
    text_layer_set_text(s_weathertext_layer, "");
  }

  // If background color and enabled
  if ( background_color_tuple && background_on_tuple ) {   
    // Set background on/off
    background_on_conf = (bool)background_on_tuple->value->int16;
    persist_write_bool(MESSAGE_KEY_BACKGROUND_ON, background_on_conf);  
    // Set background color if enabled, otherwise we load the default one - red
    background_color = background_on_conf ? (int)background_color_tuple->value->int32 : 0xFF0000;
    persist_write_int(MESSAGE_KEY_BACKGROUND_COLOR, background_color);
    
    // Redraw
    if ( s_canvas_layer ) {
      layer_mark_dirty(s_canvas_layer);
    }
  }

  APP_LOG(APP_LOG_LEVEL_DEBUG, "weather_units_conf %d", weather_units_conf);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "weather_on_conf %d", weather_on_conf);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "background_on_conf %d", background_on_conf);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "background_color %d", background_color);
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped!");
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed!");
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Outbox send success!");
}

/*************************** AnimationImplementation **************************/

static void animation_started(Animation *anim, void *context) {
  s_animating = true;
}

static void animation_stopped(Animation *anim, bool stopped, void *context) {
  s_animating = false;
}

static void animate(int duration, int delay, AnimationImplementation *implementation, bool handlers) {
  Animation *anim = animation_create();
  animation_set_duration(anim, duration);
  animation_set_delay(anim, delay);
  animation_set_curve(anim, AnimationCurveEaseInOut);
  animation_set_implementation(anim, implementation);
  if ( handlers ) {
    animation_set_handlers(anim, (AnimationHandlers) {
      .started = animation_started,
      .stopped = animation_stopped
    }, NULL);
  }
  animation_schedule(anim);
}

/************************************ UI **************************************/

static void setRandomColor() {
  for ( int i = 0; i < 3; i++ ) {
     s_color_channels[i] = rand() % 256;
  }
  
  int toHex ( int r, int g, int b ) {
      return (r<<16) | (g<<8) | b;
  }
  
  background_color = toHex(s_color_channels[0], s_color_channels[1], s_color_channels[2]);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "in_interval %d", background_color);
}

static void tick_handler(struct tm *tick_time, TimeUnits changed) {
  static bool in_interval = true;
  static char date_buffer[16];
  
  APP_LOG(APP_LOG_LEVEL_INFO, "tm_hour %d", tick_time->tm_hour);
  
  // Store time
  s_last_time.hours = tick_time->tm_hour;
  s_last_time.hours -= (s_last_time.hours > 12) ? 12 : 0;
  s_last_time.minutes = tick_time->tm_min;
  
  strftime(s_last_date, sizeof(s_last_date), "%a %d", tick_time);
  
  if ( weather_safemode_conf ) {
    if ( tick_time->tm_hour >= SAFEMODE_ON && tick_time->tm_hour <= SAFEMODE_OFF ) {
      in_interval = false;
      APP_LOG(APP_LOG_LEVEL_INFO, "in_interval");
    }
  }

  // Get weather update every 30 minutes
  if ( tick_time->tm_min % 30 == 0 && weather_on_conf && in_interval ) {
    // Begin dictionary
    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);

    // Add a key-value pair
    dict_write_uint8(iter, 0, 0);

    // Send the message!
    app_message_outbox_send();
  }

  if ( tick_time->tm_min % 10 == 0 && !background_on_conf ) {
    setRandomColor();
  }

  // Redraw
  if ( s_canvas_layer ) {
    layer_mark_dirty(s_canvas_layer);
  }
}

static int hours_to_minutes(int hours_out_of_12) {
  return (int)(float)(((float)hours_out_of_12 / 12.0F) * 60.0F);
}

static void update_proc(Layer *layer, GContext *ctx) {
  // Color background?
  GRect bounds = layer_get_bounds(layer);

  if ( COLORS ) {
    graphics_context_set_fill_color(ctx, GColorFromHEX(background_color));
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  }

  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 4);

  graphics_context_set_antialiased(ctx, ANTIALIASING);
  
  // Set date
  text_layer_set_text(s_date_layer, s_last_date);

  // White clockface
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, s_center, s_radius);

  // Draw outline
  graphics_draw_circle(ctx, s_center, s_radius);

  // Don't use current time while animating
  Time mode_time = (s_animating) ? s_anim_time : s_last_time;  

  // Adjust for minutes through the hour
  float minute_angle = TRIG_MAX_ANGLE * mode_time.minutes / 60;
  float hour_angle;
  if ( s_animating ) {
    // Hours out of 60 for smoothness
    hour_angle = TRIG_MAX_ANGLE * mode_time.hours / 60;
  } else {
    hour_angle = TRIG_MAX_ANGLE * mode_time.hours / 12;
  }
  hour_angle += (minute_angle / TRIG_MAX_ANGLE) * (TRIG_MAX_ANGLE / 12);

  // Plot hands
  GPoint minute_hand = (GPoint) {
    .x = (int16_t)(sin_lookup(TRIG_MAX_ANGLE * mode_time.minutes / 60) * (int32_t)(s_radius - HAND_MARGIN) / TRIG_MAX_RATIO) + s_center.x,
    .y = (int16_t)(-cos_lookup(TRIG_MAX_ANGLE * mode_time.minutes / 60) * (int32_t)(s_radius - HAND_MARGIN) / TRIG_MAX_RATIO) + s_center.y,
  };
  GPoint hour_hand = (GPoint) {
    .x = (int16_t)(sin_lookup(hour_angle) * (int32_t)(s_radius - (2 * HAND_MARGIN)) / TRIG_MAX_RATIO) + s_center.x,
    .y = (int16_t)(-cos_lookup(hour_angle) * (int32_t)(s_radius - (2 * HAND_MARGIN)) / TRIG_MAX_RATIO) + s_center.y,
  };

  // Draw hands with positive length only
  if ( s_radius > 2 * HAND_MARGIN ) {
    graphics_draw_line(ctx, s_center, hour_hand);
  }
  if ( s_radius > HAND_MARGIN ) {
    graphics_draw_line(ctx, s_center, minute_hand);
  }
}

static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect window_bounds = layer_get_bounds(window_layer);

  s_center = grect_center_point(&window_bounds);

  s_canvas_layer = layer_create(window_bounds);

  layer_set_update_proc(s_canvas_layer, update_proc);
  layer_add_child(window_layer, s_canvas_layer);
  
  // Create date Layer
  s_date_layer = text_layer_create(
      GRect(0, PBL_IF_ROUND_ELSE(50, 50), window_bounds.size.w, 25));

  // Create weather icon Layer
  s_weather_layer = text_layer_create(
      GRect(0, PBL_IF_ROUND_ELSE(90, 90), window_bounds.size.w, 25));

  // Create temperature Layer
  s_weathertext_layer = text_layer_create(
      GRect(0, PBL_IF_ROUND_ELSE(112, 112), window_bounds.size.w, 25));
  
  // Style the date text
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorBlack);
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);

  // Style the text
  text_layer_set_background_color(s_weather_layer, GColorClear);
  text_layer_set_text_color(s_weather_layer, GColorBlack);
  text_layer_set_text_alignment(s_weather_layer, GTextAlignmentCenter);

  // Style the text
  text_layer_set_background_color(s_weathertext_layer, GColorClear);
  text_layer_set_text_color(s_weathertext_layer, GColorBlack);
  text_layer_set_text_alignment(s_weathertext_layer, GTextAlignmentCenter);

  // Set fonts
  s_weather_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_icon_font = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_NUPE_23));
  text_layer_set_font(s_date_layer, s_weather_font);
  text_layer_set_font(s_weathertext_layer, s_weather_font);
  text_layer_set_font(s_weather_layer, s_icon_font);

  // Add layers
  layer_add_child(window_get_root_layer(window), text_layer_get_layer(s_date_layer));
  layer_add_child(window_get_root_layer(window), text_layer_get_layer(s_weathertext_layer));
  layer_add_child(window_get_root_layer(window), text_layer_get_layer(s_weather_layer));
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
   // Destroy weather elements
  text_layer_destroy(s_date_layer);
  text_layer_destroy(s_weather_layer);
  text_layer_destroy(s_weathertext_layer);
  fonts_unload_custom_font(s_icon_font);
}

/*********************************** App **************************************/

static int anim_percentage(AnimationProgress dist_normalized, int max) {
  return (int)(float)(((float)dist_normalized / (float)ANIMATION_NORMALIZED_MAX) * (float)max);
}

static void radius_update(Animation *anim, AnimationProgress dist_normalized) {
  s_radius = anim_percentage(dist_normalized, FINAL_RADIUS);

  layer_mark_dirty(s_canvas_layer);
}

static void hands_update(Animation *anim, AnimationProgress dist_normalized) {
  s_anim_time.hours = anim_percentage(dist_normalized, hours_to_minutes(s_last_time.hours));
  s_anim_time.minutes = anim_percentage(dist_normalized, s_last_time.minutes);

  layer_mark_dirty(s_canvas_layer);
}

static void init() {
  srand(time(NULL));

  time_t t = time(NULL);
  struct tm *time_now = localtime(&t);
  tick_handler(time_now, MINUTE_UNIT);

  s_main_window = window_create();

  weather_on_conf = persist_exists(MESSAGE_KEY_WEATHER_ON) ? persist_read_bool(MESSAGE_KEY_WEATHER_ON) : false;
  weather_safemode_conf = persist_exists(MESSAGE_KEY_WEATHER_SAFEMODE) ? persist_read_bool(MESSAGE_KEY_WEATHER_SAFEMODE) : true;
  weather_units_conf = persist_exists(MESSAGE_KEY_UNITS) ? persist_read_bool(MESSAGE_KEY_UNITS) : false;
  background_on_conf = persist_exists(MESSAGE_KEY_BACKGROUND_ON) ? persist_read_bool(MESSAGE_KEY_BACKGROUND_ON) : false;
  background_color = persist_exists(MESSAGE_KEY_BACKGROUND_COLOR) ? persist_read_int(MESSAGE_KEY_BACKGROUND_COLOR) : 0xFF0000;

  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_main_window, true);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  // Prepare animations
  AnimationImplementation radius_impl = {
    .update = radius_update
  };
  animate(ANIMATION_DURATION, ANIMATION_DELAY, &radius_impl, false);

  AnimationImplementation hands_impl = {
    .update = hands_update
  };
  animate(2 * ANIMATION_DURATION, ANIMATION_DELAY, &hands_impl, true);

   // Register callbacks
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);

  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());
}

static void deinit() {
  window_destroy(s_main_window);
}

int main() {
  init();
  app_event_loop();
  deinit();
}
