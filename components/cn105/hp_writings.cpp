#include "cn105.h"



byte CN105Climate::checkSum(byte bytes[], int len) {
    byte sum = 0;
    for (int i = 0; i < len; i++) {
        sum += bytes[i];
    }
    return (0xfc - sum) & 0xff;
}


void CN105Climate::sendFirstConnectionPacket() {
    if (this->isConnected_) {
        this->isHeatpumpConnected_ = false;

        ESP_LOGD(TAG, "Envoi du packet de connexion...");
        byte packet[CONNECT_LEN];
        memcpy(packet, CONNECT, CONNECT_LEN);
        //for(int count = 0; count < 2; count++) {

        this->writePacket(packet, CONNECT_LEN, false);      // checkIsActive=false because it's the first packet and we don't have any reply yet

        lastSend = CUSTOM_MILLIS;

        // we wait for a 4s timeout to check if the hp has replied to connection packet
        this->set_timeout("checkFirstConnection", 4000, [this]() {
            if (!this->isHeatpumpConnected_) {
                ESP_LOGE(TAG, "--> Heatpump did not reply: NOT CONNECTED <--");
            } else {
                // not usefull because the response has been processed in processCommand()
            }

            });

    } else {
        ESP_LOGE(TAG, "Vous devez dabord connecter l'appareil via l'UART");
    }
}




void CN105Climate::statusChanged() {
    ESP_LOGD(TAG, "hpStatusChanged ->");
    this->current_temperature = currentStatus.roomTemperature;

    ESP_LOGD(TAG, "t°: %f", currentStatus.roomTemperature);
    ESP_LOGD(TAG, "operating: %d", currentStatus.operating);
    ESP_LOGD(TAG, "compressor freq: %d", currentStatus.compressorFrequency);

    this->updateAction();
    this->publish_state();
}

void CN105Climate::setActionIfOperatingTo(climate::ClimateAction action) {
    if (currentStatus.operating) {
        this->action = action;
    } else {
        this->action = climate::CLIMATE_ACTION_IDLE;
    }
    ESP_LOGD(TAG, "setting action to -> %d", this->action);
}

void CN105Climate::setActionIfOperatingAndCompressorIsActiveTo(climate::ClimateAction action) {
    if (currentStatus.compressorFrequency <= 0) {
        this->action = climate::CLIMATE_ACTION_IDLE;
    } else {
        this->setActionIfOperatingTo(action);
    }
}

void CN105Climate::updateAction() {
    ESP_LOGV(TAG, "updating action back to espHome...");
    switch (this->mode) {
    case climate::CLIMATE_MODE_HEAT:
        this->setActionIfOperatingAndCompressorIsActiveTo(climate::CLIMATE_ACTION_HEATING);
        break;
    case climate::CLIMATE_MODE_COOL:
        this->setActionIfOperatingAndCompressorIsActiveTo(climate::CLIMATE_ACTION_COOLING);
        break;
    case climate::CLIMATE_MODE_HEAT_COOL:
        this->setActionIfOperatingAndCompressorIsActiveTo(
            (this->current_temperature > this->target_temperature ?
                climate::CLIMATE_ACTION_COOLING :
                climate::CLIMATE_ACTION_HEATING));
        break;
    case climate::CLIMATE_MODE_DRY:
        this->setActionIfOperatingAndCompressorIsActiveTo(climate::CLIMATE_ACTION_DRYING);
        break;
    case climate::CLIMATE_MODE_FAN_ONLY:
        this->action = climate::CLIMATE_ACTION_FAN;
        break;
    default:
        this->action = climate::CLIMATE_ACTION_OFF;
    }

    ESP_LOGD(TAG, "Climate mode is: %i", this->mode);
    ESP_LOGD(TAG, "Climate action is: %i", this->action);
}



void CN105Climate::prepareInfoPacket(byte* packet, int length) {
    ESP_LOGV(TAG, "preparing info packet...");

    memset(packet, 0, length * sizeof(byte));

    for (int i = 0; i < INFOHEADER_LEN && i < length; i++) {
        packet[i] = INFOHEADER[i];
    }
}

void CN105Climate::prepareSetPacket(byte* packet, int length) {
    ESP_LOGV(TAG, "preparing Set packet...");
    memset(packet, 0, length * sizeof(byte));

    for (int i = 0; i < HEADER_LEN && i < length; i++) {
        packet[i] = HEADER[i];
    }
}

void CN105Climate::writePacket(byte* packet, int length, bool checkIsActive) {

    if ((this->isConnected_) &&
        (this->isHeatpumpConnectionActive() || (!checkIsActive))) {

        if (this->get_hw_serial_()->availableForWrite() >= length) {
            ESP_LOGD(TAG, "writing packet...");
            this->hpPacketDebug(packet, length, "WRITE");

            for (int i = 0; i < length; i++) {
                this->get_hw_serial_()->write((uint8_t)packet[i]);
            }
        } else {
            ESP_LOGW(TAG, "delaying packet writing because serial buffer is not ready...");
            this->set_timeout("write", 200, [this, packet, length]() { this->writePacket(packet, length); });
        }
    } else {
        ESP_LOGW(TAG, "could not write as asked, because UART is not connected");
        this->disconnectUART();
        this->setupUART();
        this->sendFirstConnectionPacket();

        ESP_LOGW(TAG, "delaying packet writing because we need to reconnect first...");
        this->set_timeout("write", 500, [this, packet, length]() { this->writePacket(packet, length); });
    }
}

void CN105Climate::createPacket(byte* packet, heatpumpSettings settings) {
    prepareSetPacket(packet, PACKET_LEN);

    ESP_LOGD(TAG, "checking differences bw asked settings and current ones...");

    if (this->hasChanged(currentSettings.power, settings.power, "power (wantedSettings)")) {
        //if (settings.power != currentSettings.power) {
        ESP_LOGD(TAG, "power changed -> %s", settings.power);
        packet[8] = POWER[lookupByteMapIndex(POWER_MAP, 2, settings.power)];
        packet[6] += CONTROL_PACKET_1[0];
    }
    if (this->hasChanged(currentSettings.mode, settings.mode, "mode (wantedSettings)")) {
        //if (settings.mode != currentSettings.mode) {
        ESP_LOGD(TAG, "heatpump mode changed -> %s", settings.mode);
        packet[9] = MODE[lookupByteMapIndex(MODE_MAP, 5, settings.mode)];
        packet[6] += CONTROL_PACKET_1[1];
    }

    if (settings.temperature != -1) {   // a target temperature was (not) set
        if (!tempMode && settings.temperature != currentSettings.temperature) {
            ESP_LOGD(TAG, "temperature changed (tempmode is false) -> %f", settings.temperature);
            packet[10] = TEMP[lookupByteMapIndex(TEMP_MAP, 16, settings.temperature)];
            packet[6] += CONTROL_PACKET_1[2];
        } else if (tempMode && settings.temperature != currentSettings.temperature) {
            ESP_LOGD(TAG, "temperature changed (tempmode is true) -> %f", settings.temperature);
            float temp = (settings.temperature * 2) + 128;
            packet[19] = (int)temp;
            packet[6] += CONTROL_PACKET_1[2];
        }
    }

    if (this->hasChanged(currentSettings.fan, settings.fan, "fan (wantedSettings)")) {
        //if (settings.fan != currentSettings.fan) {
        ESP_LOGD(TAG, "heatpump fan changed -> %s", settings.fan);
        packet[11] = FAN[lookupByteMapIndex(FAN_MAP, 6, settings.fan)];
        packet[6] += CONTROL_PACKET_1[3];
    }

    if (this->hasChanged(currentSettings.vane, settings.vane, "vane (wantedSettings)")) {
        //if (settings.vane != currentSettings.vane) {
        ESP_LOGD(TAG, "heatpump vane changed -> %s", settings.vane);
        packet[12] = VANE[lookupByteMapIndex(VANE_MAP, 7, settings.vane)];
        packet[6] += CONTROL_PACKET_1[4];
    }
    if (this->hasChanged(currentSettings.wideVane, settings.wideVane, "wideVane (wantedSettings)")) {
        //if (settings.wideVane != currentSettings.wideVane) {        
        ESP_LOGD(TAG, "heatpump widevane changed -> %s", settings.wideVane);
        packet[18] = WIDEVANE[lookupByteMapIndex(WIDEVANE_MAP, 7, settings.wideVane)] | (wideVaneAdj ? 0x80 : 0x00);
        packet[7] += CONTROL_PACKET_2[0];
    }
    // add the checksum
    byte chkSum = checkSum(packet, 21);
    packet[21] = chkSum;
    //ESP_LOGD(TAG, "debug before write packet:");
    //this->hpPacketDebug(packet, 22, "WRITE");
}

/**
 * builds and send all an update packet to the heatpump
 * SHEDULER_INTERVAL_SYNC_NAME scheduler is canceled
 *
 *
*/
void CN105Climate::sendWantedSettings() {

    if (this->autoUpdate) {
        ESP_LOGD(TAG, "cancelling the update loop during the push of the settings..");
        /*  we don't want the autoupdate loop to interfere with this packet communication
            So we first cancel the SHEDULER_INTERVAL_SYNC_NAME */
        this->cancel_timeout(SHEDULER_INTERVAL_SYNC_NAME);
        this->cancel_timeout("2ndPacket");
        this->cancel_timeout("3rdPacket");
    }

    // and then we send the update packet
    byte packet[PACKET_LEN] = {};
    this->createPacket(packet, wantedSettings);
    this->writePacket(packet, PACKET_LEN);

    // wantedSettings are sent so we don't need to keep them anymore
    // this is usefull because we might look at wantedSettings later to check if a request is pending
    wantedSettings = {};
    wantedSettings.temperature = -1;    // to know user did not ask 

    // here we restore the update scheduler we had canceled 
    this->set_timeout(DEFER_SHEDULER_INTERVAL_SYNC_NAME, DEFER_SCHEDULE_UPDATE_LOOP_DELAY, [this]() {
        this->programUpdateInterval();
        });

}



// void CN105Climate::programResponseCheck(byte* packet) {
//     int packetType = packet[5];
//     // 0x01 Settings
//     // 0x06 status
//     // 0x07 Update remote temp
//     if ((packetType == 0x01) || (packetType == 0x06) || (packetType == 0x07)) {
//         // increment the counter which will be decremented by the response handled by
//         // getDataFromResponsePacket() method case 0x06 
//         // processCommand (case 0x61)
//         this->nonResponseCounter++;

//     }

// }

// TODO: changer cette methode afin qu'elle programme le check aussi pour les paquets de 
// setRemoteTemperature et pour les sendWantedSettings
// deprecated: we now use isHeatpumpConnectionActive() each time a packet is written 
// checkPoints are when we get a packet and before we send one

void CN105Climate::programResponseCheck(int packetType) {
    if (packetType == RQST_PKT_STATUS) {
        // increment the counter which will be decremented by the response handled by
        //getDataFromResponsePacket() method case 0x06
        this->nonResponseCounter++;

        this->set_timeout("checkpacketResponse", this->update_interval_ * 0.9, [this]() {

            if (this->nonResponseCounter > MAX_NON_RESPONSE_REQ) {
                ESP_LOGI(TAG, "There are too many status resquests without response: %d of max %d", this->nonResponseCounter, MAX_NON_RESPONSE_REQ);
                ESP_LOGI(TAG, "Heater is not connected anymore");
                this->disconnectUART();
                this->setupUART();
                this->sendFirstConnectionPacket();
                this->nonResponseCounter = 0;
            }

            });
    }

}
void CN105Climate::buildAndSendRequestPacket(int packetType) {
    byte packet[PACKET_LEN] = {};
    createInfoPacket(packet, packetType);
    this->writePacket(packet, PACKET_LEN);

    // When we send a status request, we expect a response
    // and we use that expectation as a connection status
    // deprecade: cet appel est remplacé par une check isHeatpumpConnectionActive dans la methode writePacket     
    // this->programResponseCheck(packetType);
}


/**
 * builds ans send all 3 types of packet to get a full informations back from heatpump
 * 3 packets are sent at 300 ms interval
*/
void CN105Climate::buildAndSendRequestsInfoPackets() {


    // TODO: faire 3 fonctions au lieu d'une
    // TODO: utiliser this->retry() de Component pour différer l'exécution si une écriture ou une lecture est en cours.

    /*if (this->isHeatpumpConnected_) {
        ESP_LOGD(TAG, "buildAndSendRequestsInfoPackets..");
        ESP_LOGD(TAG, "sending a request for settings packet (0x02)");
        this->buildAndSendRequestPacket(RQST_PKT_SETTINGS);
        ESP_LOGD(TAG, "sending a request room temp packet (0x03)");
        this->buildAndSendRequestPacket(RQST_PKT_ROOM_TEMP);
        ESP_LOGD(TAG, "sending a request status paquet (0x06)");
        this->buildAndSendRequestPacket(RQST_PKT_STATUS);
    } else {
        ESP_LOGE(TAG, "sync impossible: heatpump not connected");
    }

    this->programUpdateInterval();*/

    if (this->isHeatpumpConnected_) {

        uint32_t interval = 300;
        if (this->update_interval_ > 0) {
            // we get the max interval of update_interval_ / 4 or interval (300)
            interval = (this->update_interval_ / 4) > interval ? interval : (this->update_interval_ / 4);
        }

        ESP_LOGD(TAG, "buildAndSendRequestsInfoPackets: sending 3 request packet at interval: %d", interval);

        ESP_LOGD(TAG, "sending a request for settings packet (0x02)");
        this->buildAndSendRequestPacket(RQST_PKT_SETTINGS);
        this->set_timeout("2ndPacket", interval, [this, interval]() {
            ESP_LOGD(TAG, "sending a request room temp packet (0x03)");
            this->buildAndSendRequestPacket(RQST_PKT_ROOM_TEMP);
            this->set_timeout("3rdPacket", interval, [this]() {
                ESP_LOGD(TAG, "sending a request status paquet (0x06)");
                this->buildAndSendRequestPacket(RQST_PKT_STATUS);
                });
            });

    } else {
        ESP_LOGE(TAG, "sync impossible: heatpump not connected");
        //this->setupUART();
        //this->sendFirstConnectionPacket();
    }
    this->programUpdateInterval();
}




void CN105Climate::createInfoPacket(byte* packet, byte packetType) {
    ESP_LOGD(TAG, "creating Info packet");
    // add the header to the packet
    for (int i = 0; i < INFOHEADER_LEN; i++) {
        packet[i] = INFOHEADER[i];
    }

    // set the mode - settings or room temperature
    if (packetType != PACKET_TYPE_DEFAULT) {
        packet[5] = INFOMODE[packetType];
    } else {
        // request current infoMode, and increment for the next request
        packet[5] = INFOMODE[infoMode];
        if (infoMode == (INFOMODE_LEN - 1)) {
            infoMode = 0;
        } else {
            infoMode++;
        }
    }

    // pad the packet out
    for (int i = 0; i < 15; i++) {
        packet[i + 6] = 0x00;
    }

    // add the checksum
    byte chkSum = checkSum(packet, 21);
    packet[21] = chkSum;
}
void CN105Climate::set_remote_temperature(float setting) {
    byte packet[PACKET_LEN] = {};

    prepareSetPacket(packet, PACKET_LEN);

    packet[5] = 0x07;
    if (setting > 0) {
        packet[6] = 0x01;
        setting = setting * 2;
        setting = round(setting);
        setting = setting / 2;
        float temp1 = 3 + ((setting - 10) * 2);
        packet[7] = (int)temp1;
        float temp2 = (setting * 2) + 128;
        packet[8] = (int)temp2;
    } else {
        packet[6] = 0x00;
        packet[8] = 0x80; //MHK1 send 80, even though it could be 00, since ControlByte is 00
    }
    // add the checksum
    byte chkSum = checkSum(packet, 21);
    packet[21] = chkSum;
    ESP_LOGD(TAG, "sending remote temperature packet...");
    writePacket(packet, PACKET_LEN);
}