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

Configure streamcore according to your hardware

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
The ESP32 will restart and begin running streamcore. You can monitor it using a serial console.

Optionally run as single command

```shell
# compile, flash and attach monitor
$ idf.py build flash monitor
```

# Architecture

## External interface

`streamcore` is meant to be used as a lightweight C++ library for playing back Spotify/Qobuz/Web-Radio music and receive control notifications from Spotify-/Qobuz-connect. 
It exposes an interface for starting the communication with Spotify and Qobuz servers trough MDNS.

## Internal details

The connection with Spotify servers to play music and recieve control information is pretty complex. First of all an access point address must be fetched from Spotify ([`ApResolve`](StreamCore32/src/ApResolve.cpp) fetches the list from http://apresolve.spotify.com/). Then a [`PlainConnection`](StreamCore32/include/PlainConnection.h) with the selected Spotify access point must be established. It is then upgraded to an encrypted [`ShannonConnection`](StreamCore32/include/ShannonConnection.h).
