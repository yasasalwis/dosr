#pragma once
#include <Arduino.h>

void netBegin();
void netTick();
void netReconnect();

const char* netIpText();
const char* netWifiStateText();
const char* netMqttStateText();

bool netApMode();
