#include "last_error.hpp"

#include <string>

namespace {
thread_local std::string g_djLastError;
}

void djClearLastError() { g_djLastError.clear(); }

void djSetLastError(const char* msg) { g_djLastError = msg ? msg : ""; }

const char* djGetLastError() { return g_djLastError.c_str(); }
