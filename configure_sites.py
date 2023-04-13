'''
Load sites/sattelites from a file and configure the commands.

The site configuration is loaded from sites.yaml, the intents are loaded from the github.com/home-assistant/intents repo.
'''

import re
import time

import requests
import yaml
from g2p_en import G2p
from paho.mqtt import client as mqtt_client

base_repo = 'https://raw.githubusercontent.com/home-assistant/intents/main/sentences/en/'
intent_scripts = ['homeassistant_HassTurnOff.yaml', 'homeassistant_HassTurnOn.yaml']
common_script = '_common.yaml'

# Load sites
with open('sites.yaml', 'r') as f:
    conf = yaml.safe_load(f)
    sites = conf['sites']

# Load intents from repo
intents = {}
for script in intent_scripts:
    r = requests.get(base_repo + script)
    if r.status_code == 200:
        ryaml = yaml.safe_load(r.text)
        for intent, data in ryaml['intents'].items():
            intents[intent] = data['data'][0]['sentences']
    else:
        SystemError('Failed to load intent script: ' + script)


# Load expansion rules
r = requests.get(base_repo + common_script)
if r.status_code == 200:
    ryaml = yaml.safe_load(r.text)
    expansions = ryaml['expansion_rules']
    for k, v in expansions.items():
        expansions[k] = v[1:-1].split('|') # Remove quotes and split

        
# Construct sentences
sentences = [
    'turn on the <name>',
    'turn off the <name>',
    'turn on <name>',
    'turn off <name>',
    'switch on the <name>',
    'switch off the <name>',
    'switch on <name>',
    'switch off <name>',
    'activate the <name>',
    'deactivate the <name>',
    'activate <name>',
    'deactivate <name>',
]

def english_g2p(text_list, alphabet=None):
    g2p = G2p()
    outs = []
    out = ''
    if alphabet is None:
        alphabet={"AE1": "a", "N": "N", " ": " ", "OW1": "b", "V": "V", "AH0": "c", "L": "L", "F": "F", "EY1": "d", "S": "S", "B": "B", "R": "R", "AO1": "e", "D": "D", "AH1": "c", "EH1": "f", "OW0": "b", "IH0": "g", "G": "G", "HH": "h", "K": "K", "IH1": "g", "W": "W", "AY1": "i", "T": "T", "M": "M", "Z": "Z", "DH": "j", "ER0": "k", "P": "P", "NG": "l", "IY1": "m", "AA1": "n", "Y": "Y", "UW1": "o", "IY0": "m", "EH2": "f", "CH": "p", "AE0": "a", "JH": "q", "ZH": "r", "AA2": "n", "SH": "s", "AW1": "t", "OY1": "u", "AW2": "t", "IH2": "g", "AE2": "a", "EY2": "d", "ER1": "k", "TH": "v", "UH1": "w", "UW2": "o", "OW2": "b", "AY2": "i", "UW0": "o", "AH2": "c", "EH0": "f", "AW0": "t", "AO2": "e", "AO0": "e", "UH0": "w", "UH2": "w", "AA0": "n", "AY0": "i", "IY2": "m", "EY0": "d", "ER2": "k", "OY2": "u", "OY0": "u"}

    for item in text_list:
        item = item.split(",")
        for phrase in item:
            labels = g2p(phrase)
            for char in labels:
                if char not in alphabet:
                    print("skip %s, not found in alphabet")
                    continue
                else:
                    out += alphabet[char]
            if phrase != item[-1]:
                out += ','
        outs.append(out)
        out = ''
    
    return outs

site_sentences = {}
for siteId, entities in sites.items():
    site_sentences[siteId] = {'text': []}
    for entity in entities['lights']: # TODO: Add other entities
        for sentence in sentences:
            site_sentences[siteId]['text'].append(sentence.replace('<name>', entity))
    site_sentences[siteId]['phonetic'] = english_g2p(site_sentences[siteId]['text'])

    assert len(site_sentences[siteId]['text']) == len(site_sentences[siteId]['phonetic'])
    assert len(site_sentences[siteId]['text']) <= 200

# Connect to MQTT
mqtt_connected = False
def on_connect(client, userdata, flags, rc):
    global mqtt_connected
    if rc == 0:
        print("Connected to MQTT")
        mqtt_connected = True
    else:
        print("Failed to connect, return code %d\n", rc)

def on_connect_fail(client, userdata, flags, rc):
    print("Failed to connect, return code %d\n", rc)

print("Trying to connect to:")
print(f"\thost: {conf['mqtt']['host']}")
print(f"\tport: {conf['mqtt']['port']}")
print(f"\tusername: {conf['mqtt']['username']}")
print(f"\tpassword: {conf['mqtt']['password']}")

client = mqtt_client.Client()
client.username_pw_set(conf['mqtt']['username'], conf['mqtt']['password'])
client.on_connect = on_connect
client.on_connect_fail = on_connect_fail
client.connect(conf['mqtt']['host'], conf['mqtt']['port'])
client.loop_start()

counter = 0
while not mqtt_connected:
    print("Waiting to connect...")
    time.sleep(3)
    if counter > 10:
        print("Could not connect")
        exit

# Send intents
for siteId, data in site_sentences.items():
    for i, (text, phonetic) in enumerate(zip(data['text'], data['phonetic'])):
        message = f'{{"text": "{text}", "phonetic": "{phonetic}", "siteId": "{siteId}"}}'
        client.publish(f'{conf["mqtt"]["topic"]}/add_cmd', message)
        print(f'Sent {i+1}/{len(data["text"])}: {message}')
        time.sleep(0.5)
