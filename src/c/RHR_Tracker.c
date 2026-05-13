#include <pebble.h>

#define PERSIST_KEY_APP_STATE 1
#define MAX_WEEKS 17
#define MAX_DAYS 14
#define HR_MIN_VALID 35
#define HR_MAX_VALID 110
#define GRAPH_SPREAD 35
#define NEUTRAL_HR 55
#define NUM_RECORDS (4 * 60) // 4 hours of minute data

typedef struct {
  uint8_t weekly_averages[MAX_WEEKS];
  time_t current_week_start;
  uint32_t current_week_total_hr;
  uint8_t current_week_days_counted;
  time_t last_recorded_day;
  uint8_t daily_history[MAX_DAYS];
} AppState;

static AppState s_app_state;
static int s_current_night_hr = 0;
static bool s_showing_14_days = true;

static Window *s_main_window;
static TextLayer *s_hr_text_layer;
static TextLayer *s_label_text_layer;
static Layer *s_graph_layer;

// Best of Both Worlds: Static global buffer instead of stack allocation to prevent stack overflow
static HealthMinuteData s_minute_data[NUM_RECORDS];

static uint8_t get_rhr_for_date(time_t day_start) {
  time_t start = day_start;
  time_t end = day_start + (4 * SECONDS_PER_HOUR);

  uint32_t returned = health_service_get_minute_history(
      s_minute_data,
      NUM_RECORDS,
      &start,
      &end);

  uint8_t result = NEUTRAL_HR;
  int total = 0;
  int count = 0;

  for (uint32_t i = 0; i < returned; i++) {
    if (!s_minute_data[i].is_invalid &&
        s_minute_data[i].heart_rate_bpm >= HR_MIN_VALID &&
        s_minute_data[i].heart_rate_bpm <= HR_MAX_VALID) {

      total += s_minute_data[i].heart_rate_bpm;
      count++;
    }
  }

  if (count > 0) {
    result = total / count;
  }

  return result;
}

static void backfill_historical_data() {
  time_t today = time_start_of_today();

  for (int i = 0; i < 7; i++) {
    time_t day_to_check = today - (i * SECONDS_PER_DAY);
    s_app_state.daily_history[i] = get_rhr_for_date(day_to_check);
  }

  for (int i = 7; i < MAX_DAYS; i++) {
    s_app_state.daily_history[i] = NEUTRAL_HR;
  }

  for (int w = 0; w < MAX_WEEKS; w++) {
    s_app_state.weekly_averages[w] = NEUTRAL_HR;
  }
}

static void load_health_data() {
  memset(&s_app_state, 0, sizeof(s_app_state));

  if (persist_exists(PERSIST_KEY_APP_STATE)) {
    persist_read_data(PERSIST_KEY_APP_STATE,
                      &s_app_state,
                      sizeof(s_app_state));
  } else {
    backfill_historical_data();
    s_app_state.current_week_start = time_start_of_today();
  }
}

static void save_health_data() {
  persist_write_data(PERSIST_KEY_APP_STATE,
                     &s_app_state,
                     sizeof(s_app_state));
}

static void calculate_last_night_hr() {
  time_t today = time_start_of_today();
  s_current_night_hr = get_rhr_for_date(today);
}

static void update_history() {
  time_t today = time_start_of_today();

  if (s_app_state.current_week_start == 0) {
    s_app_state.current_week_start = today;
  }

  if (today > s_app_state.last_recorded_day) {

    int days_passed =
        s_app_state.last_recorded_day == 0
            ? 0
            : (today - s_app_state.last_recorded_day) / SECONDS_PER_DAY;

    if (days_passed > 0) {

      int shift =
          (days_passed > MAX_DAYS)
              ? MAX_DAYS
              : days_passed;

      for (int i = MAX_DAYS - 1; i >= shift; i--) {
        s_app_state.daily_history[i] =
            s_app_state.daily_history[i - shift];
      }

      for (int i = 0; i < shift; i++) {
        s_app_state.daily_history[i] = NEUTRAL_HR;
      }
    }

    while (today >=
           s_app_state.current_week_start +
               (7 * SECONDS_PER_DAY)) {

      if (s_app_state.current_week_days_counted > 0) {

        uint8_t new_avg =
            s_app_state.current_week_total_hr /
            s_app_state.current_week_days_counted;

        for (int i = MAX_WEEKS - 1; i > 0; i--) {
          s_app_state.weekly_averages[i] =
              s_app_state.weekly_averages[i - 1];
        }

        s_app_state.weekly_averages[0] = new_avg;
      }

      s_app_state.current_week_start +=
          (7 * SECONDS_PER_DAY);

      s_app_state.current_week_total_hr = 0;
      s_app_state.current_week_days_counted = 0;
    }

    if (s_current_night_hr > 0) {

      s_app_state.current_week_total_hr +=
          s_current_night_hr;

      s_app_state.current_week_days_counted++;

      s_app_state.daily_history[0] =
          s_current_night_hr;
    }

    s_app_state.last_recorded_day = today;

    save_health_data();
  }
}

static void select_click_handler(ClickRecognizerRef recognizer,
                                 void *context) {

  s_showing_14_days = !s_showing_14_days;

  text_layer_set_text(
      s_label_text_layer,
      s_showing_14_days
          ? "14 Day History"
          : "120 Day History");

  layer_mark_dirty(s_graph_layer);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(
      BUTTON_ID_SELECT,
      select_click_handler);
}

static void graph_update_proc(Layer *layer,
                              GContext *ctx) {

  GRect bounds = layer_get_bounds(layer);

  graphics_context_set_fill_color(
      ctx,
      GColorDarkGray);

  graphics_fill_rect(
      ctx,
      bounds,
      0,
      GCornerNone);

  graphics_context_set_stroke_color(
      ctx,
      GColorWhite);

  graphics_draw_line(
      ctx,
      GPoint(10, bounds.size.h - 10),
      GPoint(bounds.size.w - 10,
             bounds.size.h - 10));

  graphics_draw_line(
      ctx,
      GPoint(10, 10),
      GPoint(10, bounds.size.h - 10));

  uint8_t latest_hr =
      s_app_state.daily_history[0]
          ? s_app_state.daily_history[0]
          : NEUTRAL_HR;

  int graph_min_hr =
      latest_hr - (GRAPH_SPREAD / 2);

  if (graph_min_hr < 0) {
    graph_min_hr = 0;
  }

  int max_graph_height =
      bounds.size.h - 20;

  int num_bars =
      s_showing_14_days
          ? MAX_DAYS
          : MAX_WEEKS;

  int bar_width =
      (bounds.size.w - 20) / num_bars;

  for (int i = 0; i < num_bars; i++) {

    int display_index =
        (num_bars - 1) - i;

    uint8_t val =
        s_showing_14_days
            ? s_app_state.daily_history[display_index]
            : s_app_state.weekly_averages[display_index];

    if (val > 0) {

      int adjusted = val - graph_min_hr;

      if (adjusted < 0) {
        adjusted = 0;
      }

      int bar_height =
          (adjusted * max_graph_height) /
          GRAPH_SPREAD;

      if (bar_height <= 0) {
        bar_height = 1;
      }

      if (bar_height > max_graph_height) {
        bar_height = max_graph_height;
      }

      int x_pos = 12 + (i * bar_width);

      int y_pos =
          bounds.size.h - 10 - bar_height;

      graphics_context_set_fill_color(
          ctx,
          (i == num_bars - 1)
              ? GColorDukeBlue
              : GColorPictonBlue);

      graphics_fill_rect(
          ctx,
          GRect(x_pos,
                y_pos,
                bar_width - 2,
                bar_height),
          0,
          GCornerNone);
    }
  }
}

static void main_window_load(Window *window) {

  Layer *window_layer =
      window_get_root_layer(window);

  GRect bounds =
      layer_get_bounds(window_layer);

  s_hr_text_layer =
      text_layer_create(
          GRect(0,
                5,
                bounds.size.w,
                35));

  text_layer_set_text_alignment(
      s_hr_text_layer,
      GTextAlignmentCenter);

  text_layer_set_font(
      s_hr_text_layer,
      fonts_get_system_font(
          FONT_KEY_GOTHIC_28_BOLD));

  static char s_hr_buffer[32];

  if (s_current_night_hr > 0) {
    snprintf(s_hr_buffer,
             sizeof(s_hr_buffer),
             "CURRENT RHR: %d",
             s_current_night_hr);
  } else {
    snprintf(s_hr_buffer,
             sizeof(s_hr_buffer),
             "Calculating...");
  }

  text_layer_set_text(
      s_hr_text_layer,
      s_hr_buffer);

  layer_add_child(
      window_layer,
      text_layer_get_layer(
          s_hr_text_layer));

  s_graph_layer =
      layer_create(
          GRect(0,
                45,
                bounds.size.w,
                bounds.size.h - 65));

  layer_set_update_proc(
      s_graph_layer,
      graph_update_proc);

  layer_add_child(
      window_layer,
      s_graph_layer);

  s_label_text_layer =
      text_layer_create(
          GRect(0,
                bounds.size.h - 20,
                bounds.size.w,
                20));

  text_layer_set_text_alignment(
      s_label_text_layer,
      GTextAlignmentCenter);

  text_layer_set_font(
      s_label_text_layer,
      fonts_get_system_font(
          FONT_KEY_GOTHIC_14));

  text_layer_set_text(
      s_label_text_layer,
      "14 Day History");

  text_layer_set_background_color(
      s_label_text_layer,
      GColorClear);

  text_layer_set_text_color(
      s_label_text_layer,
      GColorWhite);

  layer_add_child(
      window_layer,
      text_layer_get_layer(
          s_label_text_layer));
}

static void main_window_unload(Window *window) {

  text_layer_destroy(
      s_hr_text_layer);

  text_layer_destroy(
      s_label_text_layer);

  layer_destroy(
      s_graph_layer);
}

static void init() {

  load_health_data();

  // Best of Both Worlds: Safe, pointer-free math to avoid midnight crashes
  time_t today_start = time_start_of_today();
  time_t now = time(NULL);

  // Only finalize nightly HR after 4 AM (14,400 seconds)
  if ((now - today_start) >= (4 * SECONDS_PER_HOUR)) {
    calculate_last_night_hr();
    update_history();
  }

  s_main_window = window_create();

  window_set_window_handlers(
      s_main_window,
      (WindowHandlers) {
          .load = main_window_load,
          .unload = main_window_unload});

  window_set_click_config_provider(
      s_main_window,
      click_config_provider);

  window_set_background_color(
      s_main_window,
      GColorBlack);

  window_stack_push(
      s_main_window,
      true);
}

static void deinit() {

  save_health_data();

  window_destroy(
      s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}