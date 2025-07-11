// SPDX-FileCopyrightText:  2019-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_DOSBOX_H
#define DOSBOX_DOSBOX_H

#include "compiler.h"
#include "config.h"
#include "messages.h"
#include "types.h"

#include <memory>

// Project name, lower-case and without spaces
#define DOSBOX_PROJECT_NAME "dosbox-staging"

// Name of the emulator
#define DOSBOX_NAME "DOSBox Staging"

// Development team name
#define DOSBOX_TEAM "The " DOSBOX_NAME " Team"

// Copyright string
#define DOSBOX_COPYRIGHT "(C) " DOSBOX_TEAM

// Fully qualified application ID for the emulator; see
// https://dbus.freedesktop.org/doc/dbus-specification.html#message-protocol-names
// for more details
#define DOSBOX_APP_ID "org.dosbox_staging.dosbox_staging"


int sdl_main(int argc, char *argv[]);

// The shutdown_requested bool is a conditional break in the parse-loop and
// machine-loop. Set it to true to gracefully quit in expected circumstances.
extern bool shutdown_requested;

// The E_Exit function throws an exception to quit. Call it in unexpected
// circumstances.
[[noreturn]] void E_Exit(const char *message, ...)
        GCC_ATTRIBUTE(__format__(__printf__, 1, 2));

class Section;
class Section_prop;

typedef Bitu (LoopHandler)(void);

const char* DOSBOX_GetVersion() noexcept;
const char* DOSBOX_GetDetailedVersion() noexcept;

double DOSBOX_GetUptime();

void DOSBOX_RunMachine();
void DOSBOX_SetLoop(LoopHandler * handler);
void DOSBOX_SetNormalLoop();

void DOSBOX_InitAllModuleConfigsAndMessages(void);

void DOSBOX_SetMachineTypeFromConfig(Section_prop* section);

int64_t DOSBOX_GetTicksDone();
void DOSBOX_SetTicksDone(const int64_t ticks_done);
void DOSBOX_SetTicksScheduled(const int64_t ticks_scheduled);

enum SVGACards {
	SVGA_None,
	SVGA_S3Trio,
	SVGA_TsengET4K,
	SVGA_TsengET3K,
	SVGA_ParadisePVGA1A
}; 

extern SVGACards svgaCard;
extern bool mono_cga;

enum MachineType {
	// In rough age-order: Hercules is the oldest and VGA is the newest
	// (Tandy started out as a clone of the PCjr, so PCjr came first)
	MCH_INVALID = 0,
	MCH_HERC    = 1 << 0,
	MCH_CGA     = 1 << 1,
	MCH_TANDY   = 1 << 2,
	MCH_PCJR    = 1 << 3,
	MCH_EGA     = 1 << 4,
	MCH_VGA     = 1 << 5,
};

extern MachineType machine;

inline bool is_machine(const int type) {
	return machine & type;
}
#define IS_TANDY_ARCH ((machine==MCH_TANDY) || (machine==MCH_PCJR))
#define IS_EGAVGA_ARCH ((machine==MCH_EGA) || (machine==MCH_VGA))
#define IS_VGA_ARCH (machine==MCH_VGA)

#ifndef DOSBOX_LOGGING_H
#include "logging.h"
#endif // the logging system.

constexpr auto DefaultMt32RomsDir   = "mt32-roms";
constexpr auto DefaultSoundfontsDir = "soundfonts";
constexpr auto GlShadersDir         = "glshaders";
constexpr auto DiskNoiseDir         = "disknoises";
constexpr auto PluginsDir           = "plugins";

constexpr auto MicrosInMillisecond = 1000;
constexpr auto BytesPerKilobyte    = 1024;

enum class DiskSpeed { Maximum, Fast, Medium, Slow };

#endif /* DOSBOX_DOSBOX_H */
