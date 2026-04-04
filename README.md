<p align="center">

# RmAnki

AnkiWeb wrapper for the ReMarkable device.

**Limitations**
- No image support
- No HTML support
- No editing support
- No pen support
- Not using the fastest refresh 

# How to build & run

Based on https://developer.remarkable.com/documentation/qt_epaper (Please check this if you're on version 3.17)

1. Download toolchain for your device 

2. Source the toolchain 
```
source <env_setup_file>
```
1. Run the build command
```
cmake . -B build
cmake --build build
```
1. Transfer files to the reMarkable
```
scp build/anki root@10.11.99.1:/home/root/anki
ssh root@10.11.99.1
systemctl stop xochitl 
chmod +x /home/root/anki

# reMarkable 1 only set this beforehand on the reMarkable:
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS="rotate=180"
 
# reMarkable 2 only set this beforehand on the reMarkable:
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS="rotate=180:invertx"

QT_QUICK_BACKEND=epaper /home/root/anki -platform epaper
```

# Disclaimer 

The original version was designed fully by my self, and this is just a port to make it easier to run. Please expect bugs! (and open an issue when you find them)
A lot of this port was with the help of Claude, I don't feel fully comfortable writing cpp and I'm not interested in learning it.

