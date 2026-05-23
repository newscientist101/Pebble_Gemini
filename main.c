#include <pebble.h>

#define RING_BUFFER_SIZE 16384
#define MAX_TEXT_LENGTH 2048
#define PERSIST_KEY_VOLUME 100

#define PLAYBACK_ADPCM_BYTES 800
#define PLAYBACK_PCM_SAMPLES (PLAYBACK_ADPCM_BYTES * 2)
#define PLAYBACK_INTERVAL_MS 200
#define START_BUFFER_THRESHOLD 3000
#define STREAM_CLOSE_DELAY_MS 1500

typedef enum { UI_STATE_IDLE, UI_STATE_THINKING, UI_STATE_RESULTS } UIState;

static Window *s_main_window;

// Active UI (Results)
static ScrollLayer *s_scroll_layer;
static TextLayer *s_text_layer;
static TextLayer *s_volume_text_layer;
static TextLayer *s_status_layer;

// Idle UI (Splash Screen)
static Layer *s_idle_layer;
static GBitmap *s_logo_bitmap;
static BitmapLayer *s_logo_layer;
static TextLayer *s_title_layer;
static TextLayer *s_prompt_layer;
static Layer *s_arrow_layer;

static DictationSession *s_dictation_session;

static char s_gemini_text[MAX_TEXT_LENGTH];
static float s_volume = 1.0f;
static AppTimer *s_volume_timer = NULL;
static AppTimer *s_thinking_timer = NULL;
static int s_thinking_dots = 0;

static uint8_t s_ring_buffer[RING_BUFFER_SIZE];
static uint32_t s_head = 0;
static uint32_t s_tail = 0;
static uint32_t s_buffer_count = 0;

static AppTimer *s_playback_timer = NULL;
static AppTimer *s_stream_close_timer = NULL;

static bool s_speaker_open = false;

static const int16_t stepTable[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
  34,37,41,45,50,55,60,66,73,80,88,97,107,118,
  130,143,157,173,190,209,230,253,279,307,337,
  371,408,449,494,544,598,658,724,796,876,963,
  1060,1166,1282,1411,1552,1707,1878,2066,2272,
  2499,2749,3024,3327,3660,4026,4428,4871,5358,
  5894,6484,7132,7845,8630,9493,10442,11487,
  12635,13899,15289,16818,18500,20350,22385,
  24623,27086,29794,32767
};

static const int8_t indexTable[16] = {
  -1,-1,-1,-1,2,4,6,8,
  -1,-1,-1,-1,2,4,6,8
};

static int32_t s_adpcm_valpred = 0;
static int32_t s_adpcm_index = 0;

static void reset_adpcm_state() {
  s_adpcm_valpred = 0;
  s_adpcm_index = 0;
}

static int8_t decode_adpcm_to_8bit(uint8_t delta) {
  int32_t step = stepTable[s_adpcm_index];
  int32_t vpdiff = step >> 3;

  if (delta & 4) vpdiff += step;
  if (delta & 2) vpdiff += (step >> 1);
  if (delta & 1) vpdiff += (step >> 2);

  if (delta & 8) {
    s_adpcm_valpred -= vpdiff;
  } else {
    s_adpcm_valpred += vpdiff;
  }

  if (s_adpcm_valpred > 32767) s_adpcm_valpred = 32767;
  if (s_adpcm_valpred < -32768) s_adpcm_valpred = -32768;

  s_adpcm_index += indexTable[delta];

  if (s_adpcm_index < 0) s_adpcm_index = 0;
  if (s_adpcm_index > 88) s_adpcm_index = 88;

  int16_t scaled = (int16_t)(s_adpcm_valpred * s_volume);
  int16_t pcm8 = scaled >> 8;

  if (pcm8 > 127) pcm8 = 127;
  if (pcm8 < -128) pcm8 = -128;

  return (int8_t)pcm8;
}

static void thinking_timer_callback(void *data) {
  if (layer_get_hidden(s_idle_layer)) return;

  s_thinking_dots = (s_thinking_dots + 1) % 4;
  static char thinking_buf[16];
  
  if (s_thinking_dots == 0) snprintf(thinking_buf, sizeof(thinking_buf), "\nThinking");
  else if (s_thinking_dots == 1) snprintf(thinking_buf, sizeof(thinking_buf), "\nThinking.");
  else if (s_thinking_dots == 2) snprintf(thinking_buf, sizeof(thinking_buf), "\nThinking..");
  else snprintf(thinking_buf, sizeof(thinking_buf), "\nThinking...");

  text_layer_set_text(s_prompt_layer, thinking_buf);
  s_thinking_timer = app_timer_register(400, thinking_timer_callback, NULL);
}

static void set_ui_state(UIState state) {
  if (s_thinking_timer) {
    app_timer_cancel(s_thinking_timer);
    s_thinking_timer = NULL;
  }

  switch (state) {
    case UI_STATE_IDLE:
      layer_set_hidden(s_idle_layer, false);
      layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), true);
      text_layer_set_text_alignment(s_prompt_layer, GTextAlignmentRight);
      text_layer_set_text(s_prompt_layer, "Press Select\nto record");
      layer_set_hidden(s_arrow_layer, false);
      layer_set_hidden(text_layer_get_layer(s_status_layer), true);
      break;

    case UI_STATE_THINKING:
      layer_set_hidden(s_idle_layer, false);
      layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), true);
      text_layer_set_text_alignment(s_prompt_layer, GTextAlignmentCenter);
      layer_set_hidden(s_arrow_layer, true);
      layer_set_hidden(text_layer_get_layer(s_status_layer), true);
      s_thinking_dots = 0;
      thinking_timer_callback(NULL);
      break;

    case UI_STATE_RESULTS:
      layer_set_hidden(s_idle_layer, true);
      layer_set_hidden(scroll_layer_get_layer(s_scroll_layer), false);
      layer_set_hidden(text_layer_get_layer(s_status_layer), false);
      break;
  }
}

static void playback_timer_callback(void *data);
static void delayed_close_callback(void *data);

static void cancel_close_timer() {
  if (s_stream_close_timer) {
    app_timer_cancel(s_stream_close_timer);
    s_stream_close_timer = NULL;
  }
}

static void schedule_close_timer() {
  cancel_close_timer();
  s_stream_close_timer = app_timer_register(STREAM_CLOSE_DELAY_MS, delayed_close_callback, NULL);
}

static void delayed_close_callback(void *data) {
  s_stream_close_timer = NULL;

  if (s_buffer_count == 0 && s_speaker_open) {
    speaker_stream_close();
    s_speaker_open = false;
  }
}

static void push_to_ring_buffer(uint8_t *data, uint16_t length) {
  for (uint16_t i = 0; i < length; i++) {
    if (s_buffer_count < RING_BUFFER_SIZE) {
      s_ring_buffer[s_tail] = data[i];
      s_tail = (s_tail + 1) % RING_BUFFER_SIZE;
      s_buffer_count++;
    }
  }
  cancel_close_timer();
}

static void start_playback() {
  cancel_close_timer();
  layer_set_hidden(text_layer_get_layer(s_status_layer), true);

  if (!s_speaker_open) {
    bool success = speaker_stream_open(SpeakerPcmFormat_8kHz_8bit, 100);

    if (!success) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Speaker failed to open");
      return;
    }
    s_speaker_open = true;
  }

  if (!s_playback_timer) {
    s_playback_timer = app_timer_register(PLAYBACK_INTERVAL_MS, playback_timer_callback, NULL);
  }
}

static void playback_timer_callback(void *data) {
  s_playback_timer = NULL;
  if (!s_speaker_open) return;

  static int8_t s_out_buf[PLAYBACK_PCM_SAMPLES];

  if (s_buffer_count >= PLAYBACK_ADPCM_BYTES) {
    for (int i = 0; i < PLAYBACK_ADPCM_BYTES; i++) {
      uint8_t adpcm_byte = s_ring_buffer[s_head];
      s_head = (s_head + 1) % RING_BUFFER_SIZE;
      s_buffer_count--;

      uint8_t nibble1 = (adpcm_byte >> 4) & 0x0F;
      uint8_t nibble2 = adpcm_byte & 0x0F;

      s_out_buf[i * 2] = decode_adpcm_to_8bit(nibble1);
      s_out_buf[(i * 2) + 1] = decode_adpcm_to_8bit(nibble2);
    }

    speaker_stream_write((uint8_t*)s_out_buf, sizeof(s_out_buf));
    s_playback_timer = app_timer_register(PLAYBACK_INTERVAL_MS, playback_timer_callback, NULL);

  } else if (s_buffer_count > 0) {
    s_playback_timer = app_timer_register(100, playback_timer_callback, NULL);
    schedule_close_timer();
  } else {
    schedule_close_timer();
  }
}

static void update_text_layout() {
  Layer *window_layer = window_get_root_layer(s_main_window);
  int16_t screen_width = layer_get_bounds(window_layer).size.w;
  int16_t text_width = screen_width - 10;

  text_layer_set_size(s_text_layer, GSize(text_width, 2000));
  GSize max_size = text_layer_get_content_size(s_text_layer);
  max_size.h += 10;

  text_layer_set_size(s_text_layer, max_size);
  scroll_layer_set_content_size(s_scroll_layer, GSize(screen_width, max_size.h + 20));
}

static void hide_volume_layer_callback(void *data) {
  layer_set_hidden(text_layer_get_layer(s_volume_text_layer), true);
  s_volume_timer = NULL;
}

static void show_volume_indicator() {
  static char vol_buf[16];
  int percentage = (int)((s_volume + 0.05f) * 100.0f);
  if (percentage > 100) percentage = 100;

  snprintf(vol_buf, sizeof(vol_buf), "Vol: %d%%", percentage);
  text_layer_set_text(s_volume_text_layer, vol_buf);
  layer_set_hidden(text_layer_get_layer(s_volume_text_layer), false);

  if (s_volume_timer) {
    app_timer_cancel(s_volume_timer);
  }
  s_volume_timer = app_timer_register(2000, hide_volume_layer_callback, NULL);
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *command_tuple = dict_find(iterator, MESSAGE_KEY_COMMAND);
  if (!command_tuple) return;

  if (strcmp(command_tuple->value->cstring, "TEXT_RESPONSE") == 0) {
    Tuple *text_tuple = dict_find(iterator, MESSAGE_KEY_TEXT);
    if (text_tuple) {
      snprintf(s_gemini_text, sizeof(s_gemini_text), "%s", text_tuple->value->cstring);
      text_layer_set_text(s_text_layer, s_gemini_text);
      update_text_layout();
      reset_adpcm_state();
      set_ui_state(UI_STATE_RESULTS);
    }
  } else if (strcmp(command_tuple->value->cstring, "AUDIO_CHUNK") == 0) {
    Tuple *chunk_tuple = dict_find(iterator, MESSAGE_KEY_CHUNK);
    if (chunk_tuple) {
      push_to_ring_buffer(chunk_tuple->value->data, chunk_tuple->length);
      if (!s_speaker_open && s_buffer_count >= START_BUFFER_THRESHOLD) {
        start_playback();
      }
    }
  } else if (strcmp(command_tuple->value->cstring, "AUDIO_SENTENCE_END") == 0) {
    if (!s_speaker_open && s_buffer_count > 0) {
      start_playback();
    }
  } else if (strcmp(command_tuple->value->cstring, "UPDATE_FONT") == 0) {
    Tuple *font_tuple = dict_find(iterator, MESSAGE_KEY_FONT_SIZE);
    if (font_tuple) {
      int new_size = font_tuple->value->int32;
      if (new_size <= 14) {
        text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
      } else if (new_size <= 18) {
        text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
      } else if (new_size <= 24) {
        text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
      } else {
        text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28));
      }
      update_text_layout();
    }
  }
}

static void dictation_session_callback(DictationSession *session, DictationSessionStatus status, char *transcription, void *context) {
  if (status == DictationSessionStatusSuccess) {
    set_ui_state(UI_STATE_THINKING);

    DictionaryIterator *iter;
    app_message_outbox_begin(&iter);
    dict_write_cstring(iter, MESSAGE_KEY_COMMAND, "DICTATION");
    dict_write_cstring(iter, MESSAGE_KEY_TEXT, transcription);
    app_message_outbox_send();
  }
}

static void stop_audio() {
  if (s_speaker_open) {
    speaker_stream_close();
    s_speaker_open = false;
  }
  if (s_playback_timer) {
    app_timer_cancel(s_playback_timer);
    s_playback_timer = NULL;
  }
  cancel_close_timer();
  s_buffer_count = 0;
  s_head = 0;
  s_tail = 0;
  reset_adpcm_state();
  
  set_ui_state(UI_STATE_IDLE);
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_speaker_open || s_buffer_count > 0 || !layer_get_hidden(scroll_layer_get_layer(s_scroll_layer))) {
    stop_audio();
    vibes_double_pulse();
  } else {
    dictation_session_start(s_dictation_session);
  }
}

static void up_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_volume += 0.1f;
  if (s_volume > 1.0f) s_volume = 1.0f;
  persist_write_int(PERSIST_KEY_VOLUME, (int)(s_volume * 100));
  vibes_short_pulse();
  show_volume_indicator();
}

static void down_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_volume -= 0.1f;
  if (s_volume < 0.0f) s_volume = 0.0f;
  persist_write_int(PERSIST_KEY_VOLUME, (int)(s_volume * 100));
  vibes_short_pulse();
  show_volume_indicator();
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_UP, 500, up_long_click_handler, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 500, down_long_click_handler, NULL);
}

static void arrow_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  
  #ifdef PBL_COLOR
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_fill_color(ctx, GColorBlack);
  #else
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_fill_color(ctx, GColorBlack);
  #endif

  graphics_context_set_stroke_width(ctx, 3);
  
  int y = bounds.size.h / 2;
  int x_start = 0;
  int x_end = bounds.size.w - 5;
  
  graphics_draw_line(ctx, GPoint(x_start, y), GPoint(x_end, y));
  graphics_draw_line(ctx, GPoint(x_end, y), GPoint(x_end - 6, y - 6));
  graphics_draw_line(ctx, GPoint(x_end, y), GPoint(x_end - 6, y + 6));
}

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // --- Active UI (Hidden initially) ---
  s_scroll_layer = scroll_layer_create(bounds);
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window);
  scroll_layer_set_callbacks(s_scroll_layer, (ScrollLayerCallbacks) {
    .click_config_provider = click_config_provider
  });
  layer_add_child(window_layer, scroll_layer_get_layer(s_scroll_layer));

  s_text_layer = text_layer_create(GRect(5, 5, bounds.size.w - 10, 2000));
  text_layer_set_text_alignment(s_text_layer, GTextAlignmentLeft);
  text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_text_layer));

  // --- Idle UI (Splash Screen) ---
  s_idle_layer = layer_create(bounds);

  s_logo_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_GEMINI_LOGO);
  s_logo_layer = bitmap_layer_create(GRect(15, 30, 48, 48));
  if (s_logo_bitmap) {
    bitmap_layer_set_bitmap(s_logo_layer, s_logo_bitmap);
    bitmap_layer_set_compositing_mode(s_logo_layer, GCompOpSet); 
  }
  layer_add_child(s_idle_layer, bitmap_layer_get_layer(s_logo_layer));

  s_title_layer = text_layer_create(GRect(70, 35, bounds.size.w - 75, 40));
  text_layer_set_font(s_title_layer, fonts_get_system_font(FONT_KEY_BITHAM_30_BLACK));
  text_layer_set_text_alignment(s_title_layer, GTextAlignmentLeft);
  text_layer_set_text(s_title_layer, "Gemini");
  layer_add_child(s_idle_layer, text_layer_get_layer(s_title_layer));

  s_prompt_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 25, bounds.size.w - 35, 60));
  text_layer_set_font(s_prompt_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_text_alignment(s_prompt_layer, GTextAlignmentRight);
  text_layer_set_text(s_prompt_layer, "Press Select\nto record");
  layer_add_child(s_idle_layer, text_layer_get_layer(s_prompt_layer));

  s_arrow_layer = layer_create(GRect(bounds.size.w - 30, bounds.size.h / 2 - 10, 30, 20));
  layer_set_update_proc(s_arrow_layer, arrow_update_proc);
  layer_add_child(s_idle_layer, s_arrow_layer);

  layer_add_child(window_layer, s_idle_layer);

  // --- Volume UI ---
  s_volume_text_layer = text_layer_create(GRect(bounds.size.w - 85, 5, 80, 24));
  text_layer_set_background_color(s_volume_text_layer, GColorBlack);
  text_layer_set_text_color(s_volume_text_layer, GColorWhite);
  text_layer_set_font(s_volume_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_volume_text_layer, GTextAlignmentCenter);
  layer_set_hidden(text_layer_get_layer(s_volume_text_layer), true);
  layer_add_child(window_layer, text_layer_get_layer(s_volume_text_layer));

  // --- Status UI (Bottom Overlay) ---
  s_status_layer = text_layer_create(GRect(0, bounds.size.h - 20, bounds.size.w, 20));
  text_layer_set_background_color(s_status_layer, GColorBlack);
  text_layer_set_text_color(s_status_layer, GColorWhite);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_text(s_status_layer, "Buffering Audio...");
  layer_set_hidden(text_layer_get_layer(s_status_layer), true);
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  s_dictation_session = dictation_session_create(sizeof(s_gemini_text), dictation_session_callback, NULL);

  set_ui_state(UI_STATE_IDLE); 
}

static void main_window_unload(Window *window) {
  stop_audio();
  dictation_session_destroy(s_dictation_session);

  if (s_logo_bitmap) gbitmap_destroy(s_logo_bitmap);
  bitmap_layer_destroy(s_logo_layer);
  text_layer_destroy(s_title_layer);
  text_layer_destroy(s_prompt_layer);
  layer_destroy(s_arrow_layer);
  layer_destroy(s_idle_layer);

  text_layer_destroy(s_volume_text_layer);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_text_layer);
  scroll_layer_destroy(s_scroll_layer);

  if (s_volume_timer) app_timer_cancel(s_volume_timer);
  if (s_thinking_timer) app_timer_cancel(s_thinking_timer);
}

static void init() {
  if (persist_exists(PERSIST_KEY_VOLUME)) {
    s_volume = persist_read_int(PERSIST_KEY_VOLUME) / 100.0f;
  } else {
    s_volume = 1.0f;
  }

  app_message_register_inbox_received(inbox_received_callback);
  app_message_open(12000, 1024);

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });

  window_stack_push(s_main_window, true);
}

static void deinit() {
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}