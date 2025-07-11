// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_MIDI_WIN32_H
#define DOSBOX_MIDI_WIN32_H

#include "midi_device.h"

#ifdef WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// clang-format off
// 'windows.h' must be included first, otherwise we'll get compilation errors
#include <windows.h>
#include <mmsystem.h>
// clang-format on

#include <sstream>
#include <string>

#include "programs.h"
#include "string_utils.h"

class MidiDeviceWin32 final : public MidiDevice {
public:
	// Throws `std::runtime_error` if the MIDI device cannot be initialiased
	MidiDeviceWin32(const char* conf)
	{
		m_event      = CreateEvent(nullptr, true, true, nullptr);
		MMRESULT res = MMSYSERR_NOERROR;

		if (conf && *conf) {
			std::string strconf(conf);
			std::istringstream configmidi(strconf);

			unsigned int total  = midiOutGetNumDevs();
			unsigned int nummer = total;

			configmidi >> nummer;

			if (configmidi.fail() && total) {
				lowcase(strconf);

				for (unsigned int i = 0; i < total; i++) {
					MIDIOUTCAPS mididev;
					midiOutGetDevCaps(i,
					                  &mididev,
					                  sizeof(MIDIOUTCAPS));

					std::string devname(mididev.szPname);
					lowcase(devname);

					if (devname.find(strconf) !=
					    std::string::npos) {
						nummer = i;
						break;
					}
				}
			}

			if (nummer < total) {
				MIDIOUTCAPS mididev;
				midiOutGetDevCaps(nummer, &mididev, sizeof(MIDIOUTCAPS));

				LOG_MSG("MIDI:WIN32: Selected output device %s",
				        mididev.szPname);

				res = midiOutOpen(&m_out,
				                  nummer,
				                  (DWORD_PTR)m_event,
				                  0,
				                  CALLBACK_EVENT);
			}
		} else {
			res = midiOutOpen(&m_out,
			                  MIDI_MAPPER,
			                  (DWORD_PTR)m_event,
			                  0,
			                  CALLBACK_EVENT);
		}

		if (res != MMSYSERR_NOERROR) {
			const auto msg = "MIDI:WIN32: Error opening device";
			LOG_WARNING("%s", msg);
			throw std::runtime_error(msg);
		}
	}

	~MidiDeviceWin32() override
	{
		MIDI_Reset(this);

		midiOutClose(m_out);
		CloseHandle(m_event);
	}

	// prevent copying
	MidiDeviceWin32(const MidiDeviceWin32&) = delete;
	// prevent assigment
	MidiDeviceWin32& operator=(const MidiDeviceWin32&) = delete;

	std::string GetName() const override
	{
		return MidiDeviceName::Win32;
	}

	Type GetType() const override
	{
		return MidiDevice::Type::External;
	}

	void SendMidiMessage(const MidiMessage& data) override
	{
		const auto status  = data[0];
		const auto data1   = data[1];
		const auto data2   = data[2];
		const uint32_t msg = status + (data1 << 8) + (data2 << 16);

		midiOutShortMsg(m_out, msg);
	}

	void SendSysExMessage(uint8_t* sysex, size_t len) override
	{
		if (WaitForSingleObject(m_event, 2000) == WAIT_TIMEOUT) {
			LOG_WARNING("MIDI:WIN32: Can't send midi message");
			return;
		}

		midiOutUnprepareHeader(m_out, &m_hdr, sizeof(m_hdr));

		m_hdr.lpData          = (char*)sysex;
		m_hdr.dwBufferLength  = len;
		m_hdr.dwBytesRecorded = len;
		m_hdr.dwUser          = 0;

		MMRESULT result = midiOutPrepareHeader(m_out, &m_hdr, sizeof(m_hdr));
		if (result != MMSYSERR_NOERROR) {
			return;
		}

		ResetEvent(m_event);

		result = midiOutLongMsg(m_out, &m_hdr, sizeof(m_hdr));
		if (result != MMSYSERR_NOERROR) {
			SetEvent(m_event);
			return;
		}
	}

private:
	HMIDIOUT m_out = nullptr;
	MIDIHDR m_hdr  = {};
	HANDLE m_event = nullptr;
};

void MIDI_WIN32_ListDevices(MidiDeviceWin32* device, Program* caller);

#endif // WIN32

#endif
