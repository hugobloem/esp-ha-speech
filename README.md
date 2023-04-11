# esp-ha-speech: Local Speech Recognition on an ESP32
This repository provides the codes to do hot-word detection and speech recognition on an ESP32, fully on-device!

(Based on the Espressif example in [`esp-box`](https://github.com/espressif/esp-box))

This project is still in its infancy, but already it can do hotword detection, speech recognition, and sending speech to Home Assistant. At the moment the hotword is set to "Hi, ESP". The speech commands are fully customisable and can be added using MQTT. Text processing is handled either by Home Assistant's built-in text conversation integration, or by Rhasspy.

https://user-images.githubusercontent.com/42470993/226731674-cff14709-fd51-44b7-a3a5-f49a408dace7.mp4

## Getting started
### Prerequisites
  1. Home Assistant (>=2023.3)
  2. ESP-BOX
  3. MQTT
  4. Rhasspy (optional)

To get started please copy secrets_template.h to secrets.h and edit the variables in there. After that you can flash your ESP-BOX using esptool. I recommend using the Visual Studio Code ESP-IDF plugin as it installs all the required programs for you and flashed the device seamlessly. 

## Managing voice commands
As of now voice commands can be sending MQTT messages to the `esp-ha-speech/config/add_cmd` topic. As data you should provide a json like this: `{"text": "<your voice command>", "phonetic": "<phonetic voice command", "siteId": "<your-siteId>"}`. The `text` entry is the command you would like to send to Home Assistant/Rhasspy for recognition. The `phonetic` entry is the phonetic version of it. This phonetic version can be generated using the following python command `python esp-ha\managed_components\espressif__esp-sr\tool\multinet_g2p.py -t <your voice command>`. `siteId` is used to seperate different esp32s, so you can for example have one in the living room and one in the kitchen and they will only listen to the messages meant for that device.

Another option to add commands is to use the convenience script [`configure_sites.py`](./configure_sites.py). To get started create a `sites.yaml` file from the [`sites_template.yaml`](./sites_template.yaml) file. Most options are straightforward but note that under the `sites` tag multiple sites (or satellites) can be configured, each with their own set of devices. The Python script will fetch intent templates from the [Home Assistant intents repo](https://github.com/home-assistant/intents), it will then create some sentences and phonemes for the given entities and send to each site. At the moment this only supports turning on and off entities under the 'lights' tag. 

To delete all existing commands send an MQTT message to `esp-ha-speech/config/rm_all` with payload `{"confirm": "yes", "siteId": "<your-siteId>"}`. Note that there are now no voice commands in the system, thus trying to invoke the wake word will result in a crash.
