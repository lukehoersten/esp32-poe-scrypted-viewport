# Scrypted Viewport v1 Technical Design Specification

Version: 1.0

## Overview

Scrypted Viewport is an Ethernet-powered ambient display appliance optimized for Scrypted camera and doorbell events.

Design goals:
- No Matter
- No HomeKit
- No polling
- No configuration UI
- No authentication
- No rendering engine
- No business logic on the device

Scrypted owns rendering, overlays, camera selection and interaction logic.

Scrypted Viewport owns Ethernet, JPEG decode, display, touch input and callback delivery.

## Hardware

### Controller
Waveshare ESP32-P4-ETH-POE

### Display
5" 800x480 IPS Capacitive Touch MIPI DSI display

## Boot

Power
-> DHCP
-> mDNS (_scrypted-viewport._tcp.local)
-> Wait

## Resolution

800x480 native.

Scrypted always renders 800x480 JPEGs.

## API

GET /health

POST /config
{
  "display":"mudroom",
  "callback":"http://scrypted.local:11080/api/viewport/touch"
}

POST /frame
Content-Type: image/jpeg

POST /sleep

POST /brightness
{
  "brightness":75
}

## Touch Callback

{
  "display":"mudroom",
  "event":"tap",
  "timestamp":1730000000
}

Supported:
- tap
- long_press
- swipe_left
- swipe_right

## Build

```sh
source ~/Dev/code/git/esp32/env.sh
cd ~/Dev/code/git/esp32/projects/esp32-poe-scrypted-viewport
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

## Philosophy

Scrypted Viewport is a thin network framebuffer appliance.

ESP:
- DHCP
- mDNS
- HTTP server
- JPEG decode
- framebuffer
- touch
- callback

Everything else belongs in Scrypted.
