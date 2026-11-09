#ifdef __APPLE__

#include "ios_audio.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "log.hpp"

constexpr int kMaxFrames = 4096;
constexpr int kMaxChannels = 8;

struct IosAudio {
  AudioUnit unit = nullptr;
  IosAudioRenderFn callback = nullptr;
  void* ctx = nullptr;
  int sampleRate = 48000;
  int channels = 2;
  std::vector<float> interleaved;
  std::mutex mutex;
  std::atomic<bool> running{false};
  id interruptionObserver = nil;

  IosAudio() { interleaved.assign((size_t)kMaxFrames * (size_t)kMaxChannels, 0.0f); }
};

namespace {

OSStatus renderCallback(void* inRefCon, AudioUnitRenderActionFlags* /*ioActionFlags*/,
                        const AudioTimeStamp* /*inTimeStamp*/, UInt32 /*inBusNumber*/,
                        UInt32 inNumberFrames, AudioBufferList* ioData) {
  auto* s = static_cast<IosAudio*>(inRefCon);
  if (s == nullptr || ioData == nullptr || s->callback == nullptr) {
    return noErr;
  }
  const int frames = std::min((int)inNumberFrames, kMaxFrames);
  const int ch = std::max(1, std::min(s->channels, kMaxChannels));
  s->callback(s->ctx, s->interleaved.data(), frames, ch);

  if (ioData->mNumberBuffers == 1) {
    float* dst = static_cast<float*>(ioData->mBuffers[0].mData);
    if (dst != nullptr) {
      std::memcpy(dst, s->interleaved.data(), (size_t)frames * (size_t)ch * sizeof(float));
    }
  } else {
    for (UInt32 b = 0; b < ioData->mNumberBuffers && (int)b < ch; ++b) {
      float* dst = static_cast<float*>(ioData->mBuffers[b].mData);
      if (dst == nullptr) {
        continue;
      }
      for (int i = 0; i < frames; ++i) {
        dst[i] = s->interleaved[(size_t)i * (size_t)ch + b];
      }
    }
  }
  return noErr;
}

bool configureSession(int channels) {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* err = nil;
  // MultiRoute is what lets class-compliant USB interfaces keep 4+ channels
  // instead of being downmixed to stereo.
  const auto category =
      channels >= 4 ? AVAudioSessionCategoryMultiRoute : AVAudioSessionCategoryPlayback;
  [session setCategory:category mode:AVAudioSessionModeDefault options:0 error:&err];
  if (err != nil) {
    LOGE("AVAudioSession category: %s", err.localizedDescription.UTF8String);
    if (channels >= 4) {
      err = nil;
      [session setCategory:AVAudioSessionCategoryPlayback
                      mode:AVAudioSessionModeDefault
                   options:0
                     error:&err];
    }
  }
  [session setPreferredSampleRate:48000 error:&err];
  [session setPreferredIOBufferDuration:(256.0 / 48000.0) error:&err];
  [session setActive:YES error:&err];
  if (err != nil) {
    LOGE("AVAudioSession activate: %s", err.localizedDescription.UTF8String);
    return false;
  }
  const NSInteger maxCh = session.maximumOutputNumberOfChannels;
  const NSInteger want = std::max((NSInteger)1, std::min((NSInteger)channels, maxCh));
  [session setPreferredOutputNumberOfChannels:want error:&err];
  if (channels >= 4 && session.outputNumberOfChannels < 4) {
    LOGE("USB mixer needs 4 output channels; route has %ld (max %ld)",
         (long)session.outputNumberOfChannels, (long)maxCh);
    return false;
  }
  return true;
}

void teardownUnit(IosAudio* s) {
  if (s->unit != nullptr) {
    AudioOutputUnitStop(s->unit);
    AudioUnitUninitialize(s->unit);
    AudioComponentInstanceDispose(s->unit);
    s->unit = nullptr;
  }
}

void stopUnlocked(IosAudio* s) {
  s->running.store(false);
  if (s->interruptionObserver != nil) {
    [[NSNotificationCenter defaultCenter] removeObserver:s->interruptionObserver];
    s->interruptionObserver = nil;
  }
  teardownUnit(s);
}

} // namespace

extern "C" {

IosAudio* ios_audio_create(void) { return new IosAudio(); }

void ios_audio_destroy(IosAudio* audio) {
  if (audio == nullptr) {
    return;
  }
  ios_audio_stop(audio);
  delete audio;
}

int ios_audio_start(IosAudio* audio, int channels, int sample_rate, IosAudioRenderFn fn,
                    void* ctx) {
  if (audio == nullptr || fn == nullptr || channels < 2) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(audio->mutex);
  stopUnlocked(audio);

  audio->callback = fn;
  audio->ctx = ctx;
  audio->channels = channels;
  audio->sampleRate = sample_rate > 0 ? sample_rate : 48000;

  if (!configureSession(channels)) {
    return 0;
  }

  AudioComponentDescription desc{};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_RemoteIO;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;

  AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
  if (comp == nullptr) {
    LOGE("RemoteIO component missing");
    return 0;
  }
  OSStatus st = AudioComponentInstanceNew(comp, &audio->unit);
  if (st != noErr) {
    LOGE("AudioComponentInstanceNew %d", (int)st);
    return 0;
  }

  AURenderCallbackStruct cb{};
  cb.inputProc = renderCallback;
  cb.inputProcRefCon = audio;
  st = AudioUnitSetProperty(audio->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                            0, &cb, sizeof(cb));
  if (st != noErr) {
    LOGE("set render callback %d", (int)st);
    teardownUnit(audio);
    return 0;
  }

  AudioStreamBasicDescription asbd{};
  asbd.mSampleRate = (Float64)audio->sampleRate;
  asbd.mFormatID = kAudioFormatLinearPCM;
  asbd.mFormatFlags =
      kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
  asbd.mChannelsPerFrame = (UInt32)audio->channels;
  asbd.mBitsPerChannel = 32;
  asbd.mFramesPerPacket = 1;
  asbd.mBytesPerFrame = 4 * (UInt32)audio->channels;
  asbd.mBytesPerPacket = asbd.mBytesPerFrame;

  st = AudioUnitSetProperty(audio->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                            &asbd, sizeof(asbd));
  if (st != noErr) {
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                        kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagsNativeEndian;
    asbd.mBytesPerFrame = 4;
    asbd.mBytesPerPacket = 4;
    st = AudioUnitSetProperty(audio->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                              &asbd, sizeof(asbd));
    if (st != noErr) {
      LOGE("set stream format %d", (int)st);
      teardownUnit(audio);
      return 0;
    }
  }

  UInt32 maxFrames = (UInt32)kMaxFrames;
  AudioUnitSetProperty(audio->unit, kAudioUnitProperty_MaximumFramesPerSlice,
                       kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames));

  st = AudioUnitInitialize(audio->unit);
  if (st != noErr) {
    LOGE("AudioUnitInitialize %d", (int)st);
    teardownUnit(audio);
    return 0;
  }

  AudioStreamBasicDescription actual{};
  UInt32 size = sizeof(actual);
  if (AudioUnitGetProperty(audio->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                           &actual, &size) == noErr) {
    if (actual.mSampleRate > 1) {
      audio->sampleRate = (int)actual.mSampleRate;
    }
    if (actual.mChannelsPerFrame >= 2) {
      audio->channels = (int)actual.mChannelsPerFrame;
    }
  }

  st = AudioOutputUnitStart(audio->unit);
  if (st != noErr) {
    LOGE("AudioOutputUnitStart %d", (int)st);
    teardownUnit(audio);
    return 0;
  }

  audio->running.store(true);

  IosAudio* s = audio;
  s->interruptionObserver = [[NSNotificationCenter defaultCenter]
      addObserverForName:AVAudioSessionInterruptionNotification
                  object:nil
                   queue:[NSOperationQueue mainQueue]
              usingBlock:^(NSNotification* note) {
                const NSInteger type =
                    [note.userInfo[AVAudioSessionInterruptionTypeKey] integerValue];
                if (type == AVAudioSessionInterruptionTypeEnded && s->unit != nullptr &&
                    s->running.load()) {
                  NSError* err = nil;
                  [[AVAudioSession sharedInstance] setActive:YES error:&err];
                  AudioOutputUnitStart(s->unit);
                }
              }];

  LOGI("ios RemoteIO started sr=%d ch=%d", audio->sampleRate, audio->channels);
  return 1;
}

void ios_audio_stop(IosAudio* audio) {
  if (audio == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(audio->mutex);
  stopUnlocked(audio);
}

int ios_audio_sample_rate(const IosAudio* audio) {
  if (audio == nullptr) {
    return 48000;
  }
  return audio->sampleRate;
}

int ios_audio_channels(const IosAudio* audio) {
  if (audio == nullptr) {
    return 2;
  }
  return audio->channels;
}

} // extern "C"

#endif
