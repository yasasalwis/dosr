#pragma once
#include <Arduino.h>

class Ui {
public:
  bool begin();
  void tick();
  void toast(const char* msg);
  bool present() const { return _present; }

private:
  bool _present = false;
};

extern Ui ui;

// Encoder ISRs live in ui.cpp but are attached from there too; exposed only
// so main.cpp can report the encoder position while debugging.
int32_t uiEncoderPosition();
