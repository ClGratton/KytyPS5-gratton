#ifndef KYTY_LIBS_DUALSENSE_SPEAKER_SINK_H_
#define KYTY_LIBS_DUALSENSE_SPEAKER_SINK_H_

namespace Libs::Controller::DualSenseSpeakerSink {

// Linux has no audio device for a Bluetooth DualSense, so its speaker has no OS volume control.
// This creates a PipeWire/PulseAudio null sink named "DualSense Speaker (Kyty)" (through pactl;
// nothing happens when pactl or the sound server is missing) and follows its volume and mute
// state. The emulator never plays into the sink: its volume and mute act as a software gain on
// the audio sent to the controller, so they add no latency. Other platforms: no-ops.

// Starts the background setup once. Cheap and non-blocking, safe to call from the audio thread.
void Ensure();
// Linear amplitude gain of the sink's volume (PulseAudio cubic scale) and mute; 1 while inactive.
float Gain();
bool  Active();
// Stops tracking and unloads the sink if this process loaded it.
void Shutdown();

} // namespace Libs::Controller::DualSenseSpeakerSink

#endif
