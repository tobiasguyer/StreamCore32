# StreamCore32

StreamCore32 is a Connect-device implementation for **Qobuz** and **Spotify**, written in C++, currently targeting **ESP32** with **VS1053** modules.

This library is derived from [feelfreelinux/cspot](https://github.com/feelfreelinux/cspot).

> **Note**
> - Spotify playback requires a **Spotify Premium** account.
> - Qobuz will only play **30 seconds per track** without a paid subscription.

## Building

### Prerequisites

Summary:

- [esp-idf](https://github.com/espressif/esp-idf) v5.5 or higher
- downloaded submodules
- protoc

This project utilizes submodules, please make sure you are cloning with the `--recursive` flag or use `git submodule update --init --recursive`.

This library uses nanopb to generate c files from protobuf definitions. Nanopb itself is included via submodules, but it requires a few external python libraries to run the generators.

To install them you can use pip:

```shell
$ pip3 install protobuf grpcio-tools
```

### Building for ESP32

The ESP32 target is built using the [esp-idf](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) toolchain

```shell
# Follow the instructions for setting up esp-idf for your operating system, up to `. ./export.sh` or equivalent
# esp-idf has a Python virtualenv, install nanopb's dependencies in it
$ pip3 install protobuf grpcio-tools
# update submodules after each code pull to avoid build errors
$ git submodule update --init --recursive
# navigate to the targets/esp32 directory
$ cd targets/esp32
# run once after pulling the repo
$ idf.py set-target esp32
```

Configure StreamCore32 according to your hardware

```shell
# run visual config editor, when done press Q to save and exit
$ idf.py menuconfig
```

Navigate to `WiFi Configuration` and provide wifi connection details

Navigate to `Spotify Configuration`, you may configure device name and audio quality.

Navigate to `Audio Sink Configuration`, you may configure the audio sink and further options.

Spotify should still be supporting different Audio Sinks, but Qobuz is currently solely focused on VS1053b.

#### Building and flashing

Build and upload the firmware

```shell
# compile
$ idf.py build

# upload
$ idf.py flash
```
The ESP32 will restart and begin running StreamCore32.
The first startup will take some time, to fetch missing ids. Those are stored in NVS, and will only be refetched, if they no more work.
You can monitor it using a serial console.

Optionally run as single command

```shell
# compile, flash and attach monitor
$ idf.py build flash monitor
```

## Web UI

StreamCore32 includes a lightweight Web UI that talks to the device via WebSockets.  
It is meant for:

- seeing the current player state (Qobuz / Spotify / Web radio)
- radio interface
- watching logs in the browser

> Status: experimental.

### Accessing the Web UI

Open a browser on a device in the same network and go to:

   http://sc32.local/

### Features

Current functionality (subject to change):

- Player view
  - Shows current track metadata (service / title / artist / album) when available
  - Displays the active stream service (e.g. Qobuz, Spotify, Web radio)
  - Reacts to WebSocket updates from the device

![player](/StreamCore32/stream/webstream/doc/player.jpg)

- Radio interface
  - Search for radio-stations
  - Save stations to favorites (name and url)
  - To delete from favorite, just click on the star-icon

![radio](/StreamCore32/stream/webstream/doc/radio.jpg)

- Web log output
  - Logging is routed through SC32_LOG
  - SC32_LOG can be configured with an optional ws_send callback so logs are forwarded to the Web UI
  - This allows you to debug the device without a serial cable, directly in the browser

![debug](/StreamCore32/stream/webstream/doc/debug.jpg)

# Architecture

## External interface

`StreamCore32` is meant to be used as a lightweight C++ library for playing back Spotify/Qobuz/Web-Radio music and receive control notifications from Spotify-/Qobuz-connect. 
It exposes an interface for starting the communication with Spotify and Qobuz servers trough MDNS.

## Internal details

The connection with Spotify servers to play music and recieve control information is pretty complex. First of all an access point address must be fetched from Spotify ([`ApResolve`](StreamCore32/stream/spotify/src/ApResolve.cpp) fetches the list from http://apresolve.spotify.com/). Then a [`PlainConnection`](StreamCore32/stream/spotify/include/PlainConnection.h) with the selected Spotify access point must be established. It is then upgraded to an encrypted [`ShannonConnection`](StreamCore32/stream/spotify/include/ShannonConnection.h).

# Known limitations

WebUI:
- The player page is currently mostly visual:
  - it reacts to WS messages (state, track changes, etc.)
  - buttons for skip / seek / play / pause are not fully wired up yet

Spotify:
- Spotify event reporting (event-service/v1) is no more supported:
  - played tracks do not show up in “recently played”
  - artists do not get additional “plays” reported from this device


