// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_MIDI_FLUIDSYNTH_H
#define DOSBOX_MIDI_FLUIDSYNTH_H

#include "midi_device.h"

#include <atomic>
#include <fluidsynth.h>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "dynlib.h"
#include "mixer.h"
#include "rwqueue.h"
#include "std_filesystem.h"

namespace FluidSynth {

extern void (*delete_fluid_settings)(fluid_settings_t*);
extern void (*delete_fluid_synth)(fluid_synth_t*);

} // namespace FluidSynth

class MidiDeviceFluidSynth final : public MidiDevice {
public:
	// Throws `std::runtime_error` if the MIDI device cannot be initialiased
	// (e.g., the requested SoundFont cannot be loaded).
	MidiDeviceFluidSynth();

	~MidiDeviceFluidSynth() override;

	void PrintStats();

	std::string GetName() const override
	{
		return MidiDeviceName::FluidSynth;
	}

	Type GetType() const override
	{
		return MidiDevice::Type::Internal;
	}

	void SendMidiMessage(const MidiMessage& msg) override;
	void SendSysExMessage(uint8_t* sysex, size_t len) override;

	std_fs::path GetSoundFontPath();
private:
	void ApplyChannelMessage(const std::vector<uint8_t>& msg);
	void ApplySysExMessage(const std::vector<uint8_t>& msg);
	void MixerCallback(const int requested_audio_frames);
	void ProcessWorkFromFifo();

	int GetNumPendingAudioFrames();
	void RenderAudioFramesToFifo(const int num_audio_frames = 1);
	void Render();

	using FluidSynthSettingsPtr =
	        std::unique_ptr<fluid_settings_t, decltype(FluidSynth::delete_fluid_settings)>;

	using FluidSynthPtr = std::unique_ptr<fluid_synth_t, decltype(FluidSynth::delete_fluid_synth)>;

	FluidSynthSettingsPtr settings{nullptr, FluidSynth::delete_fluid_settings};
	FluidSynthPtr synth{nullptr, FluidSynth::delete_fluid_synth};

	MixerChannelPtr mixer_channel = nullptr;
	RWQueue<AudioFrame> audio_frame_fifo{1};
	RWQueue<MidiWork> work_fifo{1};
	std::thread renderer = {};

	std_fs::path soundfont_path = {};

	// Used to track the balance of time between the last mixer callback
	// versus the current MIDI SysEx or Msg event.
	double last_rendered_ms   = 0.0;
	double ms_per_audio_frame = 0.0;

	bool had_underruns = false;
};

void FSYNTH_ListDevices(MidiDeviceFluidSynth* device, Program* caller);

#endif
