#pragma once

#ifdef __APPLE__

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*IosAudioRenderFn)(void* ctx, float* interleaved, int frames, int channels);

typedef struct IosAudio IosAudio;

IosAudio* ios_audio_create(void);
void ios_audio_destroy(IosAudio* audio);

/** Open RemoteIO. channels is 2 (phone) or 4 (USB mixer). Returns 1 on success. */
int ios_audio_start(IosAudio* audio, int channels, int sample_rate, IosAudioRenderFn fn,
                    void* ctx);
void ios_audio_stop(IosAudio* audio);

int ios_audio_sample_rate(const IosAudio* audio);
int ios_audio_channels(const IosAudio* audio);

#ifdef __cplusplus
}
#endif

#endif
