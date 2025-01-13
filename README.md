# Tuiss ESP32 HomeKit Controller

This project enables HomeKit integration for Tuiss TS3000 motorized blinds using an ESP32 microcontroller. It allows you to control your Tuiss blinds through Apple HomeKit, providing seamless integration with your iOS devices and Siri commands.

## Features

- Control Tuiss TS3000 blinds through HomeKit
- Support for multiple blinds (currently configured for 2 blinds)
- Persistent BLE connections with automatic reconnection
- Position control with debouncing
- Periodic keepalive messages to maintain connections

## Hardware Requirements

- ESP32 microcontroller
- Tuiss TS3000 motorized blinds

## Software Dependencies

- [HomeSpan](https://github.com/HomeSpan/HomeSpan)
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)

## Installation

1. Install the required libraries in Arduino IDE or PlatformIO
2. Clone this repository
3. Upload the code to your ESP32

## Configuration

The following constants can be modified in the code:

```cpp
#define BLIND_COUNT 2           // Number of blinds to control
#define BLIND_NAME "TS3000"     // Blind model name to search for
```

HomeKit pairing code is set to: `46637726`

## Usage

1. Power up the ESP32
2. Enter the WiFi credentials via the debugger while connected via USB using the HomeSpan CLI
2. Add the accessory in the Apple Home app using the pairing code
3. The controller will automatically connect to nearby Tuiss blinds
4. Control your blinds through the Home app or Siri

## Technical Details

The system:
- Uses BLE to communicate with the blinds
- Maintains persistent connections
- Sends keepalive messages every 30 seconds
- Implements position control with 2-second debouncing
- Automatically reconnects if connection is lost

## Troubleshooting

If the blinds aren't responding:
- Check if the blinds are powered and within range
- Verify the ESP32 is connected to the blinds (check serial output)
- Reset the ESP32 if connections are unstable

## Contributing

Feel free to submit issues and pull requests.
