#include <pebble.h>

#define PERSIST_KEY_APP_STATE 1

// 120 days / 7 days is approx 17 weeks
#define MAX_WEEKS 17 

// 14 day history
#define MAX_DAYS 14  

#define HR_MIN_VALID 35
#define HR_MAX_VALID 110

// Graph spread setting (Zoom level)
#define GRAPH_SPREAD 25

// Baseline resting heart rate for pre-populating the graph
#define NEUTRAL_HR 50 

// Struct to hold all persistent data
typedef struct {
  uint8_t weekly_averages[MAX_WEEKS];
  time_t current_week_start;
  uint16_t current_week_total_hr;
  uint8_t current_week_days_counted;
  time_t last_recorded_day;
  uint8_t daily_history[MAX_DAYS]; // Added the 14-day history array
} AppState;

static AppState s_app_state;
static int s_current_night_hr = 0;
static bool s_showing_14_days = true; // Tracks which graph is currently displayed

static Window *s_main_window;
static TextLayer *s_hr_text_layer;
static TextLayer *s_label_text_layer;
static Layer *s_graph_layer;

// Load stored data
static void load_health_data() {
  // Clear the struct first so any new fields (like daily_history) start at 0
  // if an older, smaller version of the save data is loaded.
  memset(&s_app_state, 0, sizeof(s_app_state));

  if (persist_exists(PERSIST_KEY_APP_STATE)) {
    persist_read_data(PERSIST_KEY_APP_STATE, &s_app_state, sizeof(s_app_state));
    
    // Patch for existing data: fill any empty 0 slots with the neutral HR
    for (int i = 0; i < MAX_WEEKS; i++) {
      if (s_app_state.weekly_averages[i] == 0) {
        s_app_state.weekly_averages[i] = NEUTRAL_HR;
      }
    }
    
    // Patch for existing data migrating to the new 14-day graph
    for (int i = 0; i < MAX_DAYS; i++) {
      if (s_app_state.daily_history[i] == 0) {
        s_app_state.daily_history[i] = NEUTRAL_HR;
      }
    }
  } else {
    // Pre-populate both graph histories with a neutral baseline on first run
    for (int i = 0; i < MAX_WEEKS; i++) {
      s_app_state.weekly_averages[i] = NEUTRAL_HR;
    }
    for (int i = 0; i < MAX_DAYS; i++) {
      s_app_state.daily_history[i] = NEUTRAL_HR;
    }
  }
}

// Save stored data
static void save_health_data() {
  persist_write_data(PERSIST_KEY_APP_STATE, &s_app_state, sizeof(s_app_state));
}

// Calculate the past night's HR
static void calculate_last_night_hr() {
  time_t start_time = time_start_of_today();
  time_t end_time = start_time + (4 * SECONDS_PER_HOUR); 
  time_t now = time(NULL);

  // If we are currently before 4 AM, look at yesterday's midnight to 4 AM
  if (now < end_time) {
    start_time -= SECONDS_PER_DAY;
    end_time -= SECONDS_PER_DAY;
  }

  uint32_t num_records = 4 * 60; // 4 hours in minutes
  HealthMinuteData *minute_data = (HealthMinuteData*)malloc(num_records * sizeof(HealthMinuteData));
  
  if (minute_data) {
    uint32_t num_records_returned = health_service_get_minute_history(minute_data, num_records, &start_time, &end_time);
    
    int total_hr = 0;
    int valid_readings = 0;

    for (uint32_t i = 0; i < num_records_returned; i++) {
      if (!minute_data[i].is_invalid && 
          minute_data[i].heart_rate_bpm >= HR_MIN_VALID && 
          minute_data[i].heart_rate_bpm <= HR_MAX_VALID) {
        
        total_hr += minute_data[i].heart_rate_bpm;
        valid_readings++;
      }
    }

    if (valid_readings > 0) {
      s_current_night_hr = total_hr / valid_readings;
    } else {
      s_current_night_hr = 0;
    }

    free(minute_data);
  }
}

// Process data for both 14-day and 120-day graphs
static void update_history() {
  time_t today = time_start_of_today();

  // If this is the very first time the app is run, set the start of the week
  if (s_app_state.current_week_start == 0) {
    s_app_state.current_week_start = today;
  }

  // Only process if we haven't already recorded today's data
  if (today > s_app_state.last_recorded_day) {
    int days_passed = s_app_state.last_recorded_day == 0 ? 0 : (today - s_app_state.last_recorded_day) / SECONDS_PER_DAY;
    
    // 1. Update the 14-Day History Array 
    if (days_passed > 0) {
      int shift = days_passed;
      if (shift > MAX_DAYS) shift = MAX_DAYS;
      
      // Shift daily data to the right
      for (int i = MAX_DAYS - 1; i >= shift; i--) {
        s_app_state.daily_history[i] = s_app_state.daily_history[i - shift];
      }
      // Fill gap days with neutral HR to keep the graph continuous
      for (int i = 0; i < shift; i++) {
        s_app_state.daily_history[i] = NEUTRAL_HR; 
      }
    }

    // 2. Update the 120-Day (Weekly) History Array
    while (today >= s_app_state.current_week_start + (7 * SECONDS_PER_DAY)) {
      if (s_app_state.current_week_days_counted > 0) {
        uint8_t new_avg = s_app_state.current_week_total_hr / s_app_state.current_week_days_counted;
        
        for (int i = MAX_WEEKS - 1; i > 0; i--) {
          s_app_state.weekly_averages[i] = s_app_state.weekly_averages[i - 1];
        }
        s_app_state.weekly_averages[0] = new_avg;
      }
      
      s_app_state.current_week_start += (7 * SECONDS_PER_DAY);
      s_app_state.current_week_total_hr = 0;
      s_app_state.current_week_days_counted = 0;
    }

    // 3. Record today's valid data into index 0 for today/this week
    if (s_current_night_hr > 0) {
      s_app_state.current_week_total_hr += s_current_night_hr;
      s_app_state.current_week_days_counted++;
      s_app_state.daily_history[0] = s_current_night_hr;
    } else {
      s_app_state.daily_history[0] = NEUTRAL_HR;
    }

    // Mark today as recorded and save to persistent storage
    s_app_state.last_recorded_day = today;
    save_health_data();
  }
}

// Click handler for toggling the graph via the middle right button
static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_showing_14_days = !s_showing_14_days; // Toggle boolean
  
  if (s_showing_14_days) {
    text_layer_set_text(s_label_text_layer, "14 Day History");
  } else {
    text_layer_set_text(s_label_text_layer, "120 Day History");
  }
  
  // Redraw the graph with the new data set
  layer_mark_dirty(s_graph_layer);
}

// Click config provider
static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
}

// Custom drawing routine for the graph
static void graph_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  
  // Draw Background
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Draw axes
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_line(ctx, GPoint(10, bounds.size.h - 10), GPoint(bounds.size.w - 10, bounds.size.h - 10)); // X axis
  graphics_draw_line(ctx, GPoint(10, 10), GPoint(10, bounds.size.h - 10)); // Y axis

  // Calculate dynamic bounds centered around the latest recorded daily RHR
  uint8_t latest_hr = s_app_state.daily_history[0];
  if (latest_hr == 0) {
    latest_hr = NEUTRAL_HR;
  }
  
  int graph_min_hr = latest_hr - (GRAPH_SPREAD / 2);
  int graph_max_hr = latest_hr + (GRAPH_SPREAD / 2) + (GRAPH_SPREAD % 2);
  
  // Prevent minimum from dipping below zero
  if (graph_min_hr < 0) {
    graph_min_hr = 0;
    graph_max_hr = GRAPH_SPREAD;
  }

  // Draw the bars
  int max_graph_height = bounds.size.h - 20;
  
  // Dynamically set properties depending on which graph is showing
  int num_bars = s_showing_14_days ? MAX_DAYS : MAX_WEEKS;
  int bar_width = (bounds.size.w - 20) / num_bars;

  for (int i = 0; i < num_bars; i++) {
    int display_index = (num_bars - 1) - i; 
    
    // Pull from the correct array
    uint8_t val = s_showing_14_days ? s_app_state.daily_history[display_index] : s_app_state.weekly_averages[display_index];

    if (val > 0) {
      int bar_height;
      
      // Apply zoom: Scale the heart rate to our dynamically calculated spread
      if (val <= graph_min_hr) {
        bar_height = 1; // Minimum 1 pixel to show data exists
      } else if (val >= graph_max_hr) {
        bar_height = max_graph_height; // Cap at the top of the graph
      } else {
        bar_height = ((val - graph_min_hr) * max_graph_height) / GRAPH_SPREAD;
      }

      int x_pos = 12 + (i * bar_width);
      int y_pos = bounds.size.h - 10 - bar_height;

      // Make the newest entry (the rightmost column) a darker shade
      if (i == num_bars - 1) {
        graphics_context_set_fill_color(ctx, GColorDukeBlue); 
      } else {
        graphics_context_set_fill_color(ctx, GColorPictonBlue); 
      }

      graphics_fill_rect(ctx, GRect(x_pos, y_pos, bar_width - 2, bar_height), 0, GCornerNone);
    }
  }
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // Set up TextLayer for Current HR
  s_hr_text_layer = text_layer_create(GRect(0, 5, bounds.size.w, 35));
  text_layer_set_text_alignment(s_hr_text_layer, GTextAlignmentCenter);
  text_layer_set_font(s_hr_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  
  static char s_hr_buffer[32]; 
  if (s_current_night_hr > 0) {
    snprintf(s_hr_buffer, sizeof(s_hr_buffer), "CURRENT RHR: %d", s_current_night_hr);
  } else {
    snprintf(s_hr_buffer, sizeof(s_hr_buffer), "No Data Yet");
  }
  text_layer_set_text(s_hr_text_layer, s_hr_buffer);
  layer_add_child(window_layer, text_layer_get_layer(s_hr_text_layer));

  // Set up Graph Layer (Starts below HR text, leaves 20px at bottom for the label)
  s_graph_layer = layer_create(GRect(0, 45, bounds.size.w, bounds.size.h - 65));
  layer_set_update_proc(s_graph_layer, graph_update_proc);
  layer_add_child(window_layer, s_graph_layer);
  
  // Set up TextLayer for the Graph Label
  s_label_text_layer = text_layer_create(GRect(0, bounds.size.h - 20, bounds.size.w, 20));
  text_layer_set_text_alignment(s_label_text_layer, GTextAlignmentCenter);
  text_layer_set_font(s_label_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text(s_label_text_layer, "14 Day History"); // Default
  text_layer_set_background_color(s_label_text_layer, GColorClear);
  text_layer_set_text_color(s_label_text_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(s_label_text_layer));
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_hr_text_layer);
  text_layer_destroy(s_label_text_layer);
  layer_destroy(s_graph_layer);
}

static void init() {
  load_health_data();
  calculate_last_night_hr();
  update_history();

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });
  
  // Attach the click handling for toggling the graph
  window_set_click_config_provider(s_main_window, click_config_provider);
  
  window_set_background_color(s_main_window, GColorBlack);
  window_stack_push(s_main_window, true);
}

static void deinit() {
  save_health_data();
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}