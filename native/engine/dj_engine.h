#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void* DjEngine;

DjEngine dj_create(void);
void dj_destroy(DjEngine engine);

int dj_start(DjEngine engine);
void dj_stop(DjEngine engine);

/** Output mode: 0 = internal stereo (phone), 1 = external 4ch USB (deck A 1-2, B 3-4). */
int dj_set_output_mode(DjEngine engine, int mode);
/** Android AudioDeviceInfo id, or 0 for default. Call before dj_set_output_mode. */
void dj_set_output_device(DjEngine engine, int device_id);
int dj_get_output_mode(DjEngine engine);
int dj_get_output_channels(DjEngine engine);

int dj_load(DjEngine engine, int deck, const char* path);
void dj_unload(DjEngine engine, int deck);

void dj_play(DjEngine engine, int deck, int playing);
void dj_seek(DjEngine engine, int deck, double seconds);
void dj_set_cue(DjEngine engine, int deck);
void dj_jump_cue(DjEngine engine, int deck);

double dj_position(DjEngine engine, int deck);
double dj_duration(DjEngine engine, int deck);
int dj_playing(DjEngine engine, int deck);
int dj_loaded(DjEngine engine, int deck);

void dj_set_gain(DjEngine engine, int deck, float linear);
void dj_set_eq(DjEngine engine, int deck, float low_db, float mid_db, float high_db);
void dj_set_filter(DjEngine engine, int deck, float amount);
void dj_set_fader(DjEngine engine, int deck, float linear);
void dj_set_xfader(DjEngine engine, float x);
void dj_set_master(DjEngine engine, float linear);
void dj_set_rate(DjEngine engine, int deck, float rate);
void dj_set_keylock(DjEngine engine, int deck, int enabled);

float dj_get_bpm(DjEngine engine, int deck);
int dj_get_key(DjEngine engine, int deck);
float dj_get_beat_offset(DjEngine engine, int deck);
void dj_nudge_grid(DjEngine engine, int deck, float seconds);

void dj_set_loop(DjEngine engine, int deck, int enabled, float bars);
double dj_loop_start(DjEngine engine, int deck);
double dj_loop_end(DjEngine engine, int deck);
void dj_set_hotcue(DjEngine engine, int deck, int index, double seconds);
void dj_jump_hotcue(DjEngine engine, int deck, int index);
double dj_get_hotcue(DjEngine engine, int deck, int index);
void dj_clear_hotcue(DjEngine engine, int deck, int index);
void dj_beat_jump(DjEngine engine, int deck, int beats);
/** Match slave tempo to master. Does not move the playhead. */
void dj_sync_to(DjEngine engine, int slave, int master);

int dj_waveform_bins(void);
int dj_waveform_copy(DjEngine engine, int deck, float* min_out, float* max_out, int bins);

/** Decode a 60s prefix (22 kHz mono) + BPM/key/grid. Uses ID3 TBPM/TKEY when present. */
int dj_analyze_file(const char* path, float* bpm, int* key, float* beat_offset);

/** Length in seconds from the file header / decoder. Does not decode PCM. */
double dj_file_duration(const char* path);

/** Load PCM and apply previously cached analysis (skips BPM/key). bpm<=0 runs a full analyze. */
int dj_load_with_analysis(DjEngine engine, int deck, const char* path, float bpm, int key,
                          float beat_offset);

#ifdef __cplusplus
}
#endif
