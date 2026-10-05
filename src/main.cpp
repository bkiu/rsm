// Really Simple Messenger (rsm): a minimal, readable Meshtastic messenger for the M5Stack Cardputer ADV with the Cap LoRa-1262.
#include <LittleFS.h>
#include <M5Cardputer.h>

#include "battery.h"
#include "cli.h"
#include "gps.h"
#include "input.h"
#include "mesh.h"
#include "messages.h"
#include "nodedb.h"
#include "radio.h"
#include "settings.h"
#include "ui.h"

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, false); // our own keyboard reader, see input.cpp
    inputBegin();
    Serial.begin(115200);

    LittleFS.begin(true);
    settingsLoad();
    nodeDB.load();
    messages.load();

    uiBegin();
    radio.begin();
    gpsBegin();
    mesh.begin();
    mesh.onIncomingText = uiNotifyIncoming;
    Serial.printf("rsm !%08x - %s. Type 'help'.\n", (unsigned)myNodeNum, radio.status());
}

void loop()
{
    static RxFrame frame;
    M5Cardputer.update();
    inputPoll();
    if (radio.poll(frame))
        mesh.onFrame(frame);
    gpsPoll();
    batteryPoll();
    mesh.tick();
    nodeDB.tick();
    messages.tick();
    cliPoll();
    uiTick();
    delay(1);
}
