<p align="center">
<img src="assets/demo.mp4">

# RmAnki

A port of the Anki flaschards app for the ReMarkable device. 

## WARNING - THIS APP IS IN EARLY BETA, IT IS NOT STABLE - USE AT YOUR OWN DISCRETION

To run you must have the `Carta` python lib on the device. You must also have an active anki instance with the ankiConnect plugin running on a reachable device (which is what it syncs)

In a SSH session - `python3 main.py <config>`, where `<config>` is the file located in `/assets`. Two examples have been provided with you. 

Currently the application is very much limited to cloze & basic questions. (BR works, but doesn't reverse the sides). Media, LaTeX, HTML and any other special formatting are not supported. Only answering the card is supported.

My current estimate for a stable release with more features is around 2/3 weeks. Please open any issues with any questions :)


