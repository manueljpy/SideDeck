import 'dart:ffi';
import 'dart:io';

DynamicLibrary openDjEngine() {
  if (Platform.isAndroid) {
    return DynamicLibrary.open('libdj_engine.so');
  }
  return DynamicLibrary.process();
}
