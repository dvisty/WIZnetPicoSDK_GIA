# WIZnetPicoSDK_GIA

TCP-based air quality monitoring examples for **W6100-EVB-Pico** using **Sensirion SGP41** gas sensors and **Sensirion SGP30**.

## Overview

This repository contains example programs for the WIZnet W6100-EVB-Pico platform, which demonstrate data collection and transmission from gas sensors over Ethernet.

The examples periodically read measurements from Sensirion gas sensors and send the results to a remote TCP server.

### Supported Sensors

| Sensor | Measured Values |
| ------ | ------------------------------------- |
| SGP30 | TVOC, eCO₂ |
| SGP41 | VOC Index, NOx Index, VOC Raw, NOx Raw |

## Features

* Ethernet communication for W6100
* TCP client implementation
* Static IPv4 configuration
* Support for Sensirion SGP30
* Support for Sensirion SGP41
* Gas index algorithm integration
* Raspberry Pi Pico SDK
* CMake build system
* Dual-core implementation for SGP41 example

## Hardware Requirements

* W6100-EVB-Pico
* Sensirion SGP30 or SGP41 sensor
* Ethernet connection
* USB cable

## Repository Structure

```text
examples/
├── client_sgp30/
│ └── client_sgp30.c
└── client_sgp41/
└── client_sgp41.c

libraries/
external/
port/
```

## How It Works

### SGP30 Example

SGP30 Client:

1. Measures TVOC and eCO₂ every second.

2. Verifies CRC of received sensor data.

3. Sends measurement packets to TCP server.

Data packet:

```c
typedef struct {
uint32_t timestamp;
uint16_t tvoc;
uint16_t eco2;
} sensor_data_t;
```

### SGP41 Example

SGP41 Client:

1. Reads raw VOC and NOx signals.

2. Processes values using Sensirion gas index algorithm.

3. Calculates VOC Index and NOx Index.

4. Sends measurement packets to TCP server.

Data packet:

```c
typedef struct {
uint32_t timestamp;
uint16_t voc_raw;
uint16_t nox_raw;
uint16_t voc_index;
uint16_t nox_index;
} sensor_data_t;
```

The application uses both RP2040 cores:

* Core 0: sensor data acquisition and processing
* Core 1: TCP communication

## Network Configuration

The examples use static network settings.

Default server address:

```text
192.168.1.100
```

Default ports:

```text
SGP30 : 8889
SGP41 : 8888
```

Modify network parameters directly in source files as needed.

## Building

### Requirements

* [Visual Studio Code](https://code.visualstudio.com/)
* [Raspberry Pi Pico Extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico)
* [CMake Tools Extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools)
* [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)

### Build Steps

1. Open the repository in Visual Studio Code.
2. Select the required example.
3. Configure the project using the Raspberry Pi Pico extension.
4. Build the project using CMake.
5. Write the generated UF2 file to the W6100-EVB-Pico board.

## Data Sample

### SGP30

```text
TVOC=60 ppb
eCO2=430 ppm
```

### SGP41

```text
VOC Raw = 32189
NOx Raw = 14272
VOC Index = 96
NOx Index = 1
```

## Applications

* Indoor air quality monitoring
* Environmental sensors
* IoT sensor nodes
* Ethernet-based monitoring systems
* Smart building projects

## Used Resources

This project is built on code from the following repositories:

### 1. [WIZnet-ioNIC/WIZnet-PICO-C](https://github.com/WIZnet-ioNIC/WIZnet-PICO-C)
- **Used:** TCP client implementation for W6100 and network utilities
- **Components:** Ethernet communication, static IPv4 configuration, WIZnet ioLibrary

### 2. [Sensirion/gas-index-algorithm](https://github.com/Sensirion/gas-index-algorithm)
- **Used:** Gas index algorithm for SGP41 processing
- **Components:** VOC Index and NOx Index calculation from raw sensor data

### 3. [Sensirion/raspberry-pi-i2c-sgp41](https://github.com/Sensirion/raspberry-pi-i2c-sgp41)
- **Used:** Driver and examples for SGP41 sensor
- **Components:** I2C communication with SGP41, sensor data reading

## License

This repository contains components of the Sensirion gas index algorithm and WIZnet ioLibrary. Please refer to the respective license files for details.

## Author

Dmytrii Chalykh

Technical University of Košice

Bachelor's thesis project
