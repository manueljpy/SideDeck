#pragma once

#include <cstdio>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "sidedeck", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "sidedeck", __VA_ARGS__)
#define CACHE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "sidedeck.cache", __VA_ARGS__)
#else
#define LOGI(...)                                      \
  do {                                                 \
    fprintf(stderr, "sidedeck: " __VA_ARGS__);         \
    fputc('\n', stderr);                               \
  } while (0)
#define LOGE(...)                                      \
  do {                                                 \
    fprintf(stderr, "sidedeck error: " __VA_ARGS__);   \
    fputc('\n', stderr);                               \
  } while (0)
#define CACHE_LOGI(...)                                \
  do {                                                 \
    fprintf(stderr, "sidedeck.cache: " __VA_ARGS__);   \
    fputc('\n', stderr);                               \
  } while (0)
#endif
