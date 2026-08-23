// RBJ Audio EQ Cookbook biquads (public-domain formulas).
#pragma once

#include <cmath>

struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1 = 0, z2 = 0;

  float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void reset() {
    z1 = 0;
    z2 = 0;
  }

  void identity() {
    b0 = 1;
    b1 = b2 = a1 = a2 = 0;
    reset();
  }

  void lowShelf(float sampleRate, float freq, float gainDb) {
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * 3.14159265358979323846f * freq / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float S = 1.0f;
    const float alpha =
        sinw / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
    const float twoSqrtA = 2.0f * std::sqrt(A) * alpha;
    const float b0n = A * ((A + 1.0f) - (A - 1.0f) * cosw + twoSqrtA);
    const float b1n = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw);
    const float b2n = A * ((A + 1.0f) - (A - 1.0f) * cosw - twoSqrtA);
    const float a0n = (A + 1.0f) + (A - 1.0f) * cosw + twoSqrtA;
    const float a1n = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw);
    const float a2n = (A + 1.0f) + (A - 1.0f) * cosw - twoSqrtA;
    b0 = b0n / a0n;
    b1 = b1n / a0n;
    b2 = b2n / a0n;
    a1 = a1n / a0n;
    a2 = a2n / a0n;
  }

  void peaking(float sampleRate, float freq, float q, float gainDb) {
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * 3.14159265358979323846f * freq / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float alpha = sinw / (2.0f * q);
    const float b0n = 1.0f + alpha * A;
    const float b1n = -2.0f * cosw;
    const float b2n = 1.0f - alpha * A;
    const float a0n = 1.0f + alpha / A;
    const float a1n = -2.0f * cosw;
    const float a2n = 1.0f - alpha / A;
    b0 = b0n / a0n;
    b1 = b1n / a0n;
    b2 = b2n / a0n;
    a1 = a1n / a0n;
    a2 = a2n / a0n;
  }

  void highShelf(float sampleRate, float freq, float gainDb) {
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * 3.14159265358979323846f * freq / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float S = 1.0f;
    const float alpha =
        sinw / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
    const float twoSqrtA = 2.0f * std::sqrt(A) * alpha;
    const float b0n = A * ((A + 1.0f) + (A - 1.0f) * cosw + twoSqrtA);
    const float b1n = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw);
    const float b2n = A * ((A + 1.0f) + (A - 1.0f) * cosw - twoSqrtA);
    const float a0n = (A + 1.0f) - (A - 1.0f) * cosw + twoSqrtA;
    const float a1n = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw);
    const float a2n = (A + 1.0f) - (A - 1.0f) * cosw - twoSqrtA;
    b0 = b0n / a0n;
    b1 = b1n / a0n;
    b2 = b2n / a0n;
    a1 = a1n / a0n;
    a2 = a2n / a0n;
  }

  void lowPass(float sampleRate, float freq, float q) {
    const float w0 = 2.0f * 3.14159265358979323846f * freq / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float alpha = sinw / (2.0f * q);
    const float b0n = (1.0f - cosw) * 0.5f;
    const float b1n = 1.0f - cosw;
    const float b2n = (1.0f - cosw) * 0.5f;
    const float a0n = 1.0f + alpha;
    const float a1n = -2.0f * cosw;
    const float a2n = 1.0f - alpha;
    b0 = b0n / a0n;
    b1 = b1n / a0n;
    b2 = b2n / a0n;
    a1 = a1n / a0n;
    a2 = a2n / a0n;
  }

  void highPass(float sampleRate, float freq, float q) {
    const float w0 = 2.0f * 3.14159265358979323846f * freq / sampleRate;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float alpha = sinw / (2.0f * q);
    const float b0n = (1.0f + cosw) * 0.5f;
    const float b1n = -(1.0f + cosw);
    const float b2n = (1.0f + cosw) * 0.5f;
    const float a0n = 1.0f + alpha;
    const float a1n = -2.0f * cosw;
    const float a2n = 1.0f - alpha;
    b0 = b0n / a0n;
    b1 = b1n / a0n;
    b2 = b2n / a0n;
    a1 = a1n / a0n;
    a2 = a2n / a0n;
  }
};
