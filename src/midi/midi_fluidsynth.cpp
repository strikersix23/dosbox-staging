//  SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "midi_fluidsynth.h"

#include <bitset>
#include <cassert>
#include <compare>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

#include "../ints/int10.h"
#include "ansi_code_markup.h"
#include "channel_names.h"
#include "control.h"
#include "cross.h"
#include "fs_utils.h"
#include "math_utils.h"
#include "mixer.h"
#include "notifications.h"
#include "pic.h"
#include "programs.h"
#include "string_utils.h"
#include "support.h"

constexpr auto SoundFontExtension = ".sf2";

/**
 * Platform specific FluidSynth shared library name
 */
#if defined(WIN32)
constexpr const char* fsynth_dynlib_file = "libfluidsynth-3.dll";
#elif defined(MACOSX)
constexpr const char* fsynth_dynlib_file = "libfluidsynth.3.dylib";
#else
constexpr const char* fsynth_dynlib_file = "libfluidsynth.so.3";
#endif

struct FsynthVersion {
	int major = 0;
	int minor = 0;
	int micro = 0;

// Workaround for clang bug
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
	auto operator<=>(const FsynthVersion&) const = default;
#pragma clang diagnostic pop
};

constexpr FsynthVersion min_fsynth_version           = {2, 2, 3};
constexpr FsynthVersion max_fsynth_version_exclusive = {3, 0, 0};

namespace FluidSynth {
/**
 * FluidSynth dynamic library handle
 */
static dynlib_handle fsynth_lib = {};

// The following function pointers will be set to their corresponding symbols in
// the FluidSynth library

/**
 * A 'X-Macro' to generate a list of function pointers to symbols in the
 * Fluidsynth library. While hacky, this ensures that all symbols will
 * be declared and resolved without risk of accidentally forgetting one or more.
 *
 * The FSFUNC macro should have the signature (return_type, symbol_name,
 * signature)
 */
// clang-format off
#define FSYNTH_FUNC_LIST(FSFUNC) \
	FSFUNC(void, delete_fluid_settings, (fluid_settings_t*)) \
	FSFUNC(void, delete_fluid_synth, (fluid_synth_t*)) \
	FSFUNC(void, fluid_version, (int *major, int *minor, int *micro)) \
	FSFUNC(fluid_settings_t*, new_fluid_settings, (void)) \
	FSFUNC(fluid_synth_t*, new_fluid_synth, (fluid_settings_t *settings)) \
	FSFUNC(fluid_log_function_t, fluid_set_log_function, (int level, fluid_log_function_t fun, void *data)) \
	FSFUNC(int, fluid_settings_setnum, (fluid_settings_t *settings, const char *name, double val)) \
	FSFUNC(int, fluid_synth_chorus_on, (fluid_synth_t *synth, int fx_group, int on)) \
	FSFUNC(int, fluid_synth_set_chorus_group_nr, (fluid_synth_t *synth, int fx_group, int nr)) \
	FSFUNC(int, fluid_synth_set_chorus_group_level, (fluid_synth_t *synth, int fx_group, double level)) \
	FSFUNC(int, fluid_synth_set_chorus_group_speed, (fluid_synth_t *synth, int fx_group, double speed)) \
	FSFUNC(int, fluid_synth_set_chorus_group_depth, (fluid_synth_t *synth, int fx_group, double depth_ms)) \
	FSFUNC(int, fluid_synth_set_chorus_group_type, (fluid_synth_t *synth, int fx_group, int type)) \
	FSFUNC(int, fluid_synth_reverb_on, (fluid_synth_t *synth, int fx_group, int on)) \
	FSFUNC(int, fluid_synth_set_reverb_group_roomsize, (fluid_synth_t *synth, int fx_group, double roomsize)) \
	FSFUNC(int, fluid_synth_set_reverb_group_damp, (fluid_synth_t *synth, int fx_group, double damping)) \
	FSFUNC(int, fluid_synth_set_reverb_group_width, (fluid_synth_t *synth, int fx_group, double width)) \
	FSFUNC(int, fluid_synth_set_reverb_group_level, (fluid_synth_t *synth, int fx_group, double level)) \
	FSFUNC(int, fluid_synth_sfcount, (fluid_synth_t *synth)) \
	FSFUNC(int, fluid_synth_sfload, (fluid_synth_t *synth, const char *filename, int reset_presets)) \
	FSFUNC(void, fluid_synth_set_gain, (fluid_synth_t *synth, float gain)) \
	FSFUNC(int, fluid_synth_set_interp_method, (fluid_synth_t *synth, int chan, int interp_method)) \
	FSFUNC(int, fluid_synth_noteoff, (fluid_synth_t *synth, int chan, int key)) \
	FSFUNC(int, fluid_synth_noteon, (fluid_synth_t *synth, int chan, int key, int vel)) \
	FSFUNC(int, fluid_synth_key_pressure, (fluid_synth_t *synth, int chan, int key, int val)) \
	FSFUNC(int, fluid_synth_cc, (fluid_synth_t *synth, int chan, int ctrl, int val)) \
	FSFUNC(int, fluid_synth_program_change, (fluid_synth_t *synth, int chan, int program)) \
	FSFUNC(int, fluid_synth_channel_pressure, (fluid_synth_t *synth, int chan, int val)) \
	FSFUNC(int, fluid_synth_pitch_bend, (fluid_synth_t *synth, int chan, int val)) \
	FSFUNC(int, fluid_synth_sysex, (fluid_synth_t *synth, const char *data, int len, char *response, int *response_len, int *handled, int dryrun)) \
	FSFUNC(int, fluid_synth_write_float, (fluid_synth_t *synth, int len, void *lout,  int loff, int lincr, void *rout, int roff, int rincr))
// clang-format on

/**
 * Macro to declare function pointers
 */
#define FSYNTH_FUNC_DECLARE(ret_type, name, sig) ret_type(*name) sig = nullptr;

FSYNTH_FUNC_LIST(FSYNTH_FUNC_DECLARE)

} // namespace FluidSynth

/**
 * A filthy macro to resolve fluidsynth symbols, and return from the
 * calling function below on error.
 */
#define FSYNTH_FUNC_GET_SYM(ret_type, name, sig) \
	FluidSynth::name = (decltype(FluidSynth::name)) \
	        dynlib_get_symbol(FluidSynth::fsynth_lib, #name); \
	if (!FluidSynth::name) { \
		dynlib_close(FluidSynth::fsynth_lib); \
		err_str = "FSYNTH: Failed to get symbol: '" #ret_type \
		          " " #name #sig "'"; \
		return DynLibResult::ResolveSymErr; \
	}

/**
 * Load the FluidSynth library and resolve all required symbols.
 *
 * If the library is already loaded, does nothing.
 */
static DynLibResult load_fsynth_dynlib(std::string& err_str)
{
	if (!FluidSynth::fsynth_lib) {
		FluidSynth::fsynth_lib = dynlib_open(fsynth_dynlib_file);
		if (!FluidSynth::fsynth_lib) {
			err_str = "FSYNTH: Failed to load FluidSynth library";
			return DynLibResult::LibOpenErr;
		}
		FSYNTH_FUNC_LIST(FSYNTH_FUNC_GET_SYM)

		// Keep ERR and PANIC logging only
		for (auto level : {FLUID_DBG, FLUID_INFO, FLUID_WARN}) {
			FluidSynth::fluid_set_log_function(level, nullptr, nullptr);
		}
	}
	return DynLibResult::Success;
}

constexpr auto ChorusSettingName    = "fsynth_chorus";
constexpr auto DefaultChorusSetting = "auto";
constexpr auto NumChorusParams      = 5;

constexpr auto ReverbSettingName    = "fsynth_reverb";
constexpr auto DefaultReverbSetting = "auto";
constexpr auto NumReverbParams      = 4;

struct ChorusParameters {
	int voice_count = {};
	double level    = {};
	double speed    = {};
	double depth    = {};
	int mod_wave    = {};
};

struct ReverbParameters {
	double room_size = {};
	double damping   = {};
	double width     = {};
	double level     = {};
};

// clang-format off

// Use reasonable chorus and reverb settings matching ScummVM's defaults
constexpr ChorusParameters DefaultChorusParameters = {
	3,   // voice count
	1.2, // level
	0.3, // speed
	8.0, // depth
	fluid_chorus_mod::FLUID_CHORUS_MOD_SINE // mod wave
};

constexpr ReverbParameters DefaultReverbParameters = {
	0.61, // room size
	0.23, // damping
	0.76, // width
	0.56  // level
};
// clang-format on

static void init_fluidsynth_dosbox_settings(SectionProp& secprop)
{
	constexpr auto WhenIdle = Property::Changeable::WhenIdle;

	// Name 'default.sf2' picks the default SoundFont if it's installed
	// in the OS (usually "Fluid_R3").
	auto str_prop = secprop.AddString("soundfont", WhenIdle, "default.sf2");
	str_prop->SetHelp(
	        "Name or path of SoundFont file to use ('default.sf2' by default).\n"
	        "The SoundFont will be looked up in the following locations in order:\n"
	        "  - The user-defined SoundFont directory (see 'soundfont_dir').\n"
	        "  - The 'soundfonts' directory in your DOSBox configuration directory.\n"
	        "  - Other common system locations.\n"
	        "The '.sf2' extension can be omitted. You can use paths relative to the above\n"
	        "locations or absolute paths as well.\n"
	        "Note: Run `MIXER /LISTMIDI` to see the list of available SoundFonts.");

	str_prop = secprop.AddString("soundfont_dir", WhenIdle, "");
	str_prop->SetHelp(
	        "Extra user-defined SoundFont directory (unset by default).\n"
	        "If this is set, SoundFonts are looked up in this directory first, then in the\n"
	        "the standard system locations.");

	constexpr auto DefaultVolume = 100;
	constexpr auto MinVolume     = 1;
	constexpr auto MaxVolume     = 800;

	auto int_prop = secprop.AddInt("soundfont_volume", WhenIdle, DefaultVolume);
	int_prop->SetMinMax(MinVolume, MaxVolume);
	int_prop->SetHelp(
	        format_str("Set the SoundFont's volume as a percentage (%d by default).\n"
	                   "This is useful for normalising the volume of different SoundFonts.\n"
	                   "The percentage value can range from %d to %d.",
	                   DefaultVolume,
	                   MinVolume,
	                   MaxVolume));

	str_prop = secprop.AddString(ChorusSettingName, WhenIdle, DefaultChorusSetting);
	str_prop->SetHelp(
	        "Configure the FluidSynth chorus. Possible values:\n"
	        "  auto:      Enable chorus, except for known problematic SoundFonts (default).\n"
	        "  on:        Always enable chorus.\n"
	        "  off:       Disable chorus.\n"
	        "  <custom>:  Custom setting via five space-separated values:\n"
	        "               - voice-count:      Integer from 0 to 99\n"
	        "               - level:            Decimal from 0.0 to 10.0\n"
	        "               - speed:            Decimal from 0.1 to 5.0 (in Hz)\n"
	        "               - depth:            Decimal from 0.0 to 21.0\n"
	        "               - modulation-wave:  'sine' or 'triangle'\n"
	        "             For example: 'fsynth_chorus = 3 1.2 0.3 8.0 sine'\n"
	        "Note: You can disable the FluidSynth chorus and enable the mixer-level chorus\n"
	        "      on the FluidSynth channel instead, or enable both chorus effects at the\n"
	        "      same time. Whether this sounds good depends on the SoundFont and the\n"
	        "      chorus settings being used.");

	str_prop = secprop.AddString(ReverbSettingName, WhenIdle, DefaultReverbSetting);
	;
	str_prop->SetHelp(
	        "Configure the FluidSynth reverb. Possible values:\n"
	        "  auto:      Enable reverb (default).\n"
	        "  on:        Enable reverb.\n"
	        "  off:       Disable reverb.\n"
	        "  <custom>:  Custom setting via four space-separated values:\n"
	        "               - room-size:  Decimal from 0.0 to 1.0\n"
	        "               - damping:    Decimal from 0.0 to 1.0\n"
	        "               - width:      Decimal from 0.0 to 100.0\n"
	        "               - level:      Decimal from 0.0 to 1.0\n"
	        "             For example: 'fsynth_reverb = 0.61 0.23 0.76 0.56'\n"
	        "Note: You can disable the FluidSynth reverb and enable the mixer-level reverb\n"
	        "      on the FluidSynth channel instead, or enable both reverb effects at the\n"
	        "      same time. Whether this sounds good depends on the SoundFont and the\n"
	        "      reverb settings being used.");

	str_prop = secprop.AddString("fsynth_filter", WhenIdle, "off");
	assert(str_prop);
	str_prop->SetHelp(
	        "Filter for the FluidSynth audio output:\n"
	        "  off:       Don't filter the output (default).\n"
	        "  <custom>:  Custom filter definition; see 'sb_filter' for details.");
}

#if defined(WIN32)

static std::vector<std_fs::path> get_platform_data_dirs()
{
	return {
	        GetConfigDir() / DefaultSoundfontsDir,

	        // C:\soundfonts is the default place where FluidSynth places
	        // default.sf2
	        // https://www.fluidsynth.org/api/fluidsettings.xml#synth.default-soundfont
	        std::string("C:\\") + DefaultSoundfontsDir + "\\",
	};
}

#elif defined(MACOSX)

static std::vector<std_fs::path> get_platform_data_dirs()
{
	return {
	        GetConfigDir() / DefaultSoundfontsDir,
	        resolve_home("~/Library/Audio/Sounds/Banks"),
	};
}

#else

static std::vector<std_fs::path> get_platform_data_dirs()
{
	// First priority is user-specific data location
	const auto xdg_data_home = get_xdg_data_home();

	std::vector<std_fs::path> dirs = {
	        xdg_data_home / "dosbox" / DefaultSoundfontsDir,
	        xdg_data_home / DefaultSoundfontsDir,
	        xdg_data_home / "sounds/sf2",
	};

	// Second priority are the $XDG_DATA_DIRS
	for (const auto& data_dir : get_xdg_data_dirs()) {
		dirs.emplace_back(data_dir / DefaultSoundfontsDir);
		dirs.emplace_back(data_dir / "sounds/sf2");
	}

	// Third priority is $XDG_CONF_HOME, for convenience
	dirs.emplace_back(GetConfigDir() / DefaultSoundfontsDir);

	return dirs;
}

#endif

static SectionProp* get_fluidsynth_section()
{
	assert(control);

	auto sec = static_cast<SectionProp*>(control->GetSection("fluidsynth"));
	assert(sec);

	return sec;
}

static std::vector<std_fs::path> get_data_dirs()
{
	auto dirs = get_platform_data_dirs();

	auto sf_dir = get_fluidsynth_section()->GetString("soundfont_dir");
	if (!sf_dir.empty()) {
		// The user-provided SoundFont dir might use a different casing
		// of the actual path on Linux & Windows, so we need to
		// normalise that to avoid some subtle bugs downstream (see
		// `find_sf_file()` as well).
		if (path_exists(sf_dir)) {
			std::error_code err = {};
			const auto canonical_path = std_fs::canonical(sf_dir, err); //-V821
			if (!err) {
				dirs.insert(dirs.begin(), canonical_path);
			}
		} else {
			NOTIFY_DisplayWarning(Notification::Source::Console,
			                      "FSYNTH",
			                      "FLUIDSYNTH_INVALID_SOUNDFONT_DIR",
			                      sf_dir.c_str());

			LOG_WARNING(
			        "Invalid 'soundfont_dir' setting; "
			        "cannot open directory '%s', using ''",
			        sf_dir.c_str());

			set_section_property_value("fluidsynth", "soundfont_dir", "");
		}
	}
	return dirs;
}

static std_fs::path find_sf_file(const std::string& sf_name)
{
	const std_fs::path sf_path = resolve_home(sf_name);
	if (path_exists(sf_path)) {
		return sf_path;
	}
	for (const auto& dir : get_data_dirs()) {
		for (const auto& sf :
		     {dir / sf_name, dir / (sf_name + SoundFontExtension)}) {
#if 0
			LOG_MSG("FSYNTH: FluidSynth checking if '%s' exists", sf.c_str());
#endif
			if (path_exists(sf)) {
				// Parts of the path come from the `soundfont`
				// setting, and `soundfont = FluidR3_GM.sf2` and
				// `soundfont = fluidr3_gm.sf2` refer to the
				// same file on case-preserving filesystems on
				// Windows and macOS.
				//
				// `std_fs::canonical` returns the absolute path
				// and matches its casing to that of the actual
				// physical file. This prevents certain subtle
				// bugs downstream when we use this path in
				// comparisons.
				std::error_code err = {};
				const auto canonical_path = std_fs::canonical(sf, err);

				if (err) {
					return {};
				}
				return canonical_path;
			}
		}
	}
	return {};
}

static void log_unknown_midi_message(const std::vector<uint8_t>& msg)
{
	auto append_as_hex = [](const std::string& str, const uint8_t val) {
		constexpr char HexChars[] = "0123456789ABCDEF";
		std::string hex_str;

		hex_str.reserve(2);
		hex_str += HexChars[val >> 4];
		hex_str += HexChars[val & 0x0F];

		return str + (str.empty() ? "" : ", ") + hex_str;
	};

	const auto hex_values = std::accumulate(msg.begin(),
	                                        msg.end(),
	                                        std::string(),
	                                        append_as_hex);

	LOG_WARNING("FSYNTH: Unknown MIDI message sequence (hex): %s",
	            hex_values.c_str());
}

// Checks if the passed effect parameter value is within the valid range and
// returns the default if it's not
static std::optional<double> validate_effect_parameter(
        const char* setting_name, const char* param_name,
        const std::string& value, const double min_value,
        const double max_value, const std::string& default_setting_value)
{
	// Convert the string to a double
	const auto val = parse_float(value);

	if (!val || (*val < min_value || *val > max_value)) {
		NOTIFY_DisplayWarning(Notification::Source::Console,
		                      "FSYNTH",
		                      "FLUIDSYNTH_INVALID_EFFECT_PARAMETER",
		                      setting_name,
		                      param_name,
		                      value.c_str(),
		                      min_value,
		                      max_value,
		                      default_setting_value.c_str());
	}
	return val;
}

std::optional<ChorusParameters> parse_custom_chorus_params(const std::string& chorus_pref)
{
	const auto params = split(chorus_pref);

	if (params.size() != NumChorusParams) {
		NOTIFY_DisplayWarning(Notification::Source::Console,
		                      "FSYNTH",
		                      "FLUIDSYNTH_INVALID_NUM_EFFECT_PARAMS",
		                      ChorusSettingName,
		                      params.size(),
		                      NumChorusParams);
		return {};
	}

	auto validate = [&](const char* param_name,
	                    const std::string& value,
	                    const double min_value,
	                    const double max_value) -> std::optional<double> {
		return validate_effect_parameter(ChorusSettingName,
		                                 param_name,
		                                 value,
		                                 min_value,
		                                 max_value,
		                                 DefaultChorusSetting);
	};

	const auto voice_count_opt = validate("params voice-count", params[0], 0, 99);
	const auto level_opt = validate("params level", params[1], 0.0, 10.0);
	const auto speed_opt = validate("params speed", params[2], 0.1, 5.0);
	const auto depth_opt = validate("params depth", params[3], 0.0, 21.0);

	const auto mod_wave_opt = [&]() -> std::optional<int> {
		if (params[4] != "triangle" && params[4] != "sine") {
			NOTIFY_DisplayWarning(Notification::Source::Console,
			                      "FSYNTH",
			                      "FLUIDSYNTH_INVALID_CHORUS_WAVE",
			                      params[4].c_str(),
			                      DefaultChorusSetting);
			return {};
		} else {
			return (params[4] == "sine"
			                ? fluid_chorus_mod::FLUID_CHORUS_MOD_SINE
			                : fluid_chorus_mod::FLUID_CHORUS_MOD_TRIANGLE);
		}
	}();

	// One or more parameter couldn't be parsed
	if (!(voice_count_opt && level_opt && speed_opt && depth_opt && mod_wave_opt)) {
		return {};
	}

	// Success
	return ChorusParameters{iround(*voice_count_opt),
	                        *level_opt,
	                        *speed_opt,
	                        *depth_opt,
	                        *mod_wave_opt};
}

static void set_chorus_params(fluid_synth_t* synth, const ChorusParameters& params)
{
	// Apply setting to all groups
	constexpr int FxGroup = -1;

	FluidSynth::fluid_synth_set_chorus_group_nr(synth, FxGroup, params.voice_count);
	FluidSynth::fluid_synth_set_chorus_group_level(synth, FxGroup, params.level);
	FluidSynth::fluid_synth_set_chorus_group_speed(synth, FxGroup, params.speed);
	FluidSynth::fluid_synth_set_chorus_group_depth(synth, FxGroup, params.depth);
	FluidSynth::fluid_synth_set_chorus_group_type(synth, FxGroup, params.mod_wave);

	LOG_MSG("FSYNTH: Chorus enabled with %d voices at level %.2f, "
	        "%.2f Hz speed, %.2f depth, and %s-wave modulation",
	        params.voice_count,
	        params.level,
	        params.speed,
	        params.depth,
	        (params.mod_wave == fluid_chorus_mod::FLUID_CHORUS_MOD_SINE
	                 ? "sine"
	                 : "triangle"));
}

static void setup_chorus(fluid_synth_t* synth, const std_fs::path& sf_path)
{
	assert(synth);

	const auto chorus_pref = get_fluidsynth_section()->GetString(ChorusSettingName);
	const auto chorus_enabled_opt = parse_bool_setting(chorus_pref);

	auto enable_chorus = [&](const bool enabled) {
		// Apply setting to all groups
		constexpr int FxGroup = -1;
		FluidSynth::fluid_synth_chorus_on(synth, FxGroup, enabled);

		if (!enabled) {
			LOG_MSG("FSYNTH: Chorus disabled");
		}
	};

	auto handle_auto_setting = [&]() {
		// Does the SoundFont have known-issues with chorus?
		const auto is_problematic_font =
		        find_in_case_insensitive("FluidR3", sf_path.string()) ||
		        find_in_case_insensitive("zdoom", sf_path.string());

		if (is_problematic_font) {
			enable_chorus(false);

			LOG_INFO(
			        "FSYNTH: Chorus auto-disabled due to known issues with "
			        "the '%s' soundfont",
			        get_fluidsynth_section()
			                ->GetString("soundfont")
			                .c_str());
		} else {
			set_chorus_params(synth, DefaultChorusParameters);
			enable_chorus(true);

			// TODO setting the recommended chorus setting for
			// GeneralUserGS will happen here
		}
	};

	if (chorus_enabled_opt) {
		const auto enabled = *chorus_enabled_opt;
		if (enabled) {
			set_chorus_params(synth, DefaultChorusParameters);
		}
		enable_chorus(enabled);

	} else if (chorus_pref == "auto") {
		handle_auto_setting();

	} else {
		if (const auto chorus_params = parse_custom_chorus_params(chorus_pref);
		    chorus_params) {

			set_chorus_params(synth, *chorus_params);
			enable_chorus(true);

		} else {
			set_section_property_value("fluidsynth",
			                           ChorusSettingName,
			                           DefaultChorusSetting);
			handle_auto_setting();
		}
	}
}

std::optional<ReverbParameters> parse_custom_reverb_params(const std::string& reverb_pref)
{
	const auto reverb = split(reverb_pref);

	if (reverb.size() != NumReverbParams) {
		NOTIFY_DisplayWarning(Notification::Source::Console,
		                      "FSYNTH",
		                      "FLUIDSYNTH_INVALID_NUM_EFFECT_PARAMS",
		                      ReverbSettingName,
		                      reverb.size(),
		                      NumReverbParams);
		return {};
	}

	auto validate = [&](const char* param_name,
	                    const std::string& value,
	                    const double min_value,
	                    const double max_value) -> std::optional<double> {
		return validate_effect_parameter(ReverbSettingName,
		                                 param_name,
		                                 value,
		                                 min_value,
		                                 max_value,
		                                 DefaultReverbSetting);
	};

	const auto room_size_opt = validate("reverb room-size", reverb[0], 0.0, 1.0);
	const auto damping_opt = validate("reverb damping", reverb[1], 0.0, 1.0);
	const auto width_opt = validate("reverb width", reverb[2], 0.0, 100.0);
	const auto level_opt = validate("reverb level", reverb[3], 0.0, 1.0);

	// One or more parameter couldn't be parsed
	if (!(room_size_opt && damping_opt && width_opt && level_opt)) {
		return {};
	}

	// Success
	return ReverbParameters{*room_size_opt, *damping_opt, *width_opt, *level_opt};
}

static void set_reverb_params(fluid_synth_t* synth, const ReverbParameters& params)

{
	// Apply setting to all groups
	constexpr int FxGroup = -1;

	FluidSynth::fluid_synth_set_reverb_group_roomsize(synth,
	                                                  FxGroup,
	                                                  params.room_size);

	FluidSynth::fluid_synth_set_reverb_group_damp(synth, FxGroup, params.damping);
	FluidSynth::fluid_synth_set_reverb_group_width(synth, FxGroup, params.width);
	FluidSynth::fluid_synth_set_reverb_group_level(synth, FxGroup, params.level);

	LOG_MSG("FSYNTH: Reverb enabled with a %.2f room size, "
	        "%.2f damping, %.2f width, and level %.2f",
	        params.room_size,
	        params.damping,
	        params.width,
	        params.level);
}

static void setup_reverb(fluid_synth_t* synth)
{
	assert(synth);

	const auto reverb_pref = get_fluidsynth_section()->GetString(ReverbSettingName);
	const auto reverb_enabled_opt = parse_bool_setting(reverb_pref);

	auto enable_reverb = [&](const bool enabled) {
		// Apply setting to all groups
		constexpr int FxGroup = -1;
		FluidSynth::fluid_synth_reverb_on(synth, FxGroup, enabled);

		if (!enabled) {
			LOG_MSG("FSYNTH: Reverb disabled");
		}
	};

	auto handle_auto_setting = [&]() {
		// TODO setting the recommended reverb setting for GeneralUserGS
		// will happen here
	};

	if (reverb_enabled_opt) {
		const auto enabled = *reverb_enabled_opt;
		if (enabled) {
			set_reverb_params(synth, DefaultReverbParameters);
		}
		enable_reverb(enabled);

	} else if (reverb_pref == "auto") {
		handle_auto_setting();

	} else {
		if (const auto reverb_params = parse_custom_reverb_params(reverb_pref);
		    reverb_params) {

			set_reverb_params(synth, *reverb_params);
			enable_reverb(true);

		} else {
			set_section_property_value("fluidsynth",
			                           ReverbSettingName,
			                           DefaultReverbSetting);
			handle_auto_setting();
		}
	}
}

MidiDeviceFluidSynth::MidiDeviceFluidSynth()
{
	std::string sym_err_msg;
	DynLibResult res = load_fsynth_dynlib(sym_err_msg);
	switch (res) {
	case DynLibResult::Success: break;
	case DynLibResult::LibOpenErr:
	case DynLibResult::ResolveSymErr: {
		LOG_ERR("%s", sym_err_msg.c_str());
		throw std::runtime_error(sym_err_msg);
		break;
	}
	}

	FsynthVersion vers = {};
	FluidSynth::fluid_version(&vers.major, &vers.minor, &vers.micro);
	if (vers < min_fsynth_version || vers >= max_fsynth_version_exclusive) {
		const auto msg = "FSYNTH: FluidSynth version must be at least 2.2.3 and less than 3.0.0";
		LOG_ERR("%s. Version loaded is %d.%d.%d",
		        msg,
		        vers.major,
		        vers.minor,
		        vers.micro);
		throw std::runtime_error(msg);
	} else {
		LOG_MSG("FSYNTH: Successfully loaded FluidSynth %d.%d.%d",
		        vers.major,
		        vers.minor,
		        vers.micro);
	}

	FluidSynthSettingsPtr fluid_settings(FluidSynth::new_fluid_settings(),
	                                     FluidSynth::delete_fluid_settings);
	if (!fluid_settings) {
		const auto msg = "FSYNTH: Failed to initialise the FluidSynth settings";
		LOG_ERR("%s", msg);
		throw std::runtime_error(msg);
	}

	auto section = get_fluidsynth_section();

	// Detailed explanation of all available FluidSynth settings:
	// http://www.fluidsynth.org/api/fluidsettings.xml

	// Per the FluidSynth API, the sample-rate should be part of the
	// settings used to instantiate the synth, so we use the mixer's
	// native rate to configure FluidSynth.
	const auto sample_rate_hz = MIXER_GetSampleRate();
	ms_per_audio_frame        = MillisInSecond / sample_rate_hz;

	FluidSynth::fluid_settings_setnum(fluid_settings.get(),
	                                  "synth.sample-rate",
	                                  sample_rate_hz);

	FluidSynthPtr fluid_synth(FluidSynth::new_fluid_synth(fluid_settings.get()),
	                          FluidSynth::delete_fluid_synth);
	if (!fluid_synth) {
		const auto msg = "FSYNTH: Failed to create the FluidSynth synthesizer";
		LOG_ERR("%s", msg);
		throw std::runtime_error(msg);
	}

	// Load the requested SoundFont or quit if none provided
	const auto sf_name = section->GetString("soundfont");
	const auto sf_path = find_sf_file(sf_name);

	if (!sf_path.empty() &&
	    FluidSynth::fluid_synth_sfcount(fluid_synth.get()) == 0) {
		constexpr auto ResetPresets = true;
		FluidSynth::fluid_synth_sfload(fluid_synth.get(),
		                               sf_path.string().c_str(),
		                               ResetPresets);
	}

	if (FluidSynth::fluid_synth_sfcount(fluid_synth.get()) == 0) {
		const auto msg = format_str("FSYNTH: Error loading SoundFont '%s'",
		                            sf_name.c_str());

		LOG_ERR("%s", msg.c_str());
		throw std::runtime_error(msg);
	}

	auto sf_volume_percent = section->GetInt("soundfont_volume");
	FluidSynth::fluid_synth_set_gain(fluid_synth.get(),
	                                 static_cast<float>(sf_volume_percent) /
	                                         100.0f);

	// Let the user know that the SoundFont was loaded
	if (sf_volume_percent == 100) {
		LOG_MSG("FSYNTH: Using SoundFont '%s'", sf_path.string().c_str());
	} else {
		LOG_MSG("FSYNTH: Using SoundFont '%s' with volume scaled to %d%%",
		        sf_path.string().c_str(),
		        sf_volume_percent);
	}

	// Applies setting to all groups
	constexpr int FxGroup = -1;

	// Use a 7th-order (highest) polynomial to generate MIDI channel
	// waveforms
	FluidSynth::fluid_synth_set_interp_method(fluid_synth.get(),
	                                          FxGroup,
	                                          FLUID_INTERP_HIGHEST);

	setup_chorus(fluid_synth.get(), sf_path);
	setup_reverb(fluid_synth.get());

	MIXER_LockMixerThread();

	// Set up the mixer callback
	const auto mixer_callback = std::bind(&MidiDeviceFluidSynth::MixerCallback,
	                                      this,
	                                      std::placeholders::_1);

	auto fluidsynth_channel = MIXER_AddChannel(mixer_callback,
	                                           sample_rate_hz,
	                                           ChannelName::FluidSynth,
	                                           {ChannelFeature::Sleep,
	                                            ChannelFeature::Stereo,
	                                            ChannelFeature::ReverbSend,
	                                            ChannelFeature::ChorusSend,
	                                            ChannelFeature::Synthesizer});

	// FluidSynth renders float audio frames between -1.0f and
	// +1.0f, so we ask the channel to scale all the samples up to
	// its 0db level.
	fluidsynth_channel->Set0dbScalar(Max16BitSampleValue);

	const std::string filter_prefs = section->GetString("fsynth_filter");

	if (!fluidsynth_channel->TryParseAndSetCustomFilter(filter_prefs)) {
		if (filter_prefs != "off") {
			NOTIFY_DisplayWarning(Notification::Source::Console,
			                      "FSYNTH",
			                      "PROGRAM_CONFIG_INVALID_SETTING",
			                      "fsynth_filter",
			                      filter_prefs.c_str(),
			                      "off");
		}

		fluidsynth_channel->SetHighPassFilter(FilterState::Off);
		fluidsynth_channel->SetLowPassFilter(FilterState::Off);

		set_section_property_value("fluidsynth", "fsynth_filter", "off");
	}

	// Double the baseline PCM prebuffer because MIDI is demanding
	// and bursty. The mixer's default of ~20 ms becomes 40 ms here,
	// which gives slower systems a better chance to keep up (and
	// prevent their audio frame FIFO from running dry).
	const auto render_ahead_ms = MIXER_GetPreBufferMs() * 2;

	// Size the out-bound audio frame FIFO
	assertm(sample_rate_hz >= 8000, "Sample rate must be at least 8 kHz");

	const auto audio_frames_per_ms = iround(sample_rate_hz / MillisInSecond);
	audio_frame_fifo.Resize(
	        check_cast<size_t>(render_ahead_ms * audio_frames_per_ms));

	// Size the in-bound work FIFO
	work_fifo.Resize(MaxMidiWorkFifoSize);

	// If we haven't failed yet, then we're ready to begin so move
	// the local objects into the member variables.
	settings      = std::move(fluid_settings);
	synth         = std::move(fluid_synth);
	mixer_channel = std::move(fluidsynth_channel);

	soundfont_path = sf_path;

	// Start rendering audio
	const auto render = std::bind(&MidiDeviceFluidSynth::Render, this);
	renderer          = std::thread(render);
	set_thread_name(renderer, "dosbox:fsynth");

	// Start playback
	MIXER_UnlockMixerThread();
}

MidiDeviceFluidSynth::~MidiDeviceFluidSynth()
{
	LOG_MSG("FSYNTH: Shutting down");

	if (had_underruns) {
		LOG_WARNING(
		        "FSYNTH: Fix underruns by lowering the CPU load, increasing "
		        "the 'prebuffer' or 'blocksize' settings, or using a simpler SoundFont");
	}

	MIXER_LockMixerThread();

	// Stop playback
	if (mixer_channel) {
		mixer_channel->Enable(false);
	}

	// Stop queueing new MIDI work and audio frames
	work_fifo.Stop();
	audio_frame_fifo.Stop();

	// Wait for the rendering thread to finish
	if (renderer.joinable()) {
		renderer.join();
	}

	// Deregister the mixer channel and remove it
	assert(mixer_channel);
	MIXER_DeregisterChannel(mixer_channel);
	mixer_channel.reset();

	MIXER_UnlockMixerThread();
}

int MidiDeviceFluidSynth::GetNumPendingAudioFrames()
{
	const auto now_ms = PIC_FullIndex();

	// Wake up the channel and update the last rendered time datum.
	assert(mixer_channel);
	if (mixer_channel->WakeUp()) {
		last_rendered_ms = now_ms;
		return 0;
	}
	if (last_rendered_ms >= now_ms) {
		return 0;
	}

	// Return the number of audio frames needed to get current again
	assert(ms_per_audio_frame > 0.0);

	const auto elapsed_ms = now_ms - last_rendered_ms;
	const auto num_audio_frames = iround(ceil(elapsed_ms / ms_per_audio_frame));
	last_rendered_ms += (num_audio_frames * ms_per_audio_frame);

	return num_audio_frames;
}

// The request to play the channel message is placed in the MIDI work FIFO
void MidiDeviceFluidSynth::SendMidiMessage(const MidiMessage& msg)
{
	std::vector<uint8_t> message(msg.data.begin(), msg.data.end());

	MidiWork work{std::move(message),
	              GetNumPendingAudioFrames(),
	              MessageType::Channel,
	              PIC_AtomicIndex()};

	work_fifo.Enqueue(std::move(work));
}

// The request to play the sysex message is placed in the MIDI work FIFO
void MidiDeviceFluidSynth::SendSysExMessage(uint8_t* sysex, size_t len)
{
	std::vector<uint8_t> message(sysex, sysex + len);

	MidiWork work{std::move(message),
	              GetNumPendingAudioFrames(),
	              MessageType::SysEx,
	              PIC_AtomicIndex()};

	work_fifo.Enqueue(std::move(work));
}

void MidiDeviceFluidSynth::ApplyChannelMessage(const std::vector<uint8_t>& msg)
{
	const auto status_byte = msg[0];
	const auto status      = get_midi_status(status_byte);
	const auto channel     = get_midi_channel(status_byte);

	// clang-format off
	switch (status) {
	case MidiStatus::NoteOff:         FluidSynth::fluid_synth_noteoff(     synth.get(), channel, msg[1]);         break;
	case MidiStatus::NoteOn:          FluidSynth::fluid_synth_noteon(      synth.get(), channel, msg[1], msg[2]); break;
	case MidiStatus::PolyKeyPressure: FluidSynth::fluid_synth_key_pressure(synth.get(), channel, msg[1], msg[2]); break;

	case MidiStatus::ControlChange: {
		const auto controller = msg[1];
		const auto value = msg[2];

		if (controller == MidiController::Portamento ||
			controller == MidiController::PortamentoTime ||
			controller == MidiController::PortamentoControl) {

			// The Roland SC-55 and its clones (Yamaha MU80 or Roland's own
			// later modules that emulate the SC-55) handle portamento (pitch
			// glides between consecutive notes on the same channel) in a very
			// specific and unique way, just like most synthesisers.
			//
			// The SC-55 accepts only 7-bit Portamento Time values via MIDI
			// CC5, where the min value of 0 sets the fastest portamento time
			// (effectively turns it off), and the max value of 127 the
			// slowest (up to 8 minutes!). There is an exponential mapping
			// between the CC values and the duration of the portamento (pitch
			// slides/glides); this custom curve is apparently approximated by
			// multiple linear segments. Moreover, the distance between the
			// source and destination notes also affect the portamento time,
			// making portamento dynamic and highly dependent on the notes
			// being played.
			//
			// FluidSynth, on the other hand, implements a very different
			// portamento model. Portament Time values are set via 14-bit CC
			// messages (via MIDI CC5 (coarse) and CC37 (fine)), and there is
			// a linear mapping between CC values and the portamento time as
			// per the following formula:
			//
			//   (CC5 * 127 ms) + (CC37 ms)
			//
			// Because of these fundamental differences, emulating Roland
			// SC-55 style portamento on FluidSynth is practically not
			// possible. Music written for the SC-55 that use portamento
			// sounds weirdly out of tune on FluidSynth (e.g. the Level 8
			// music of Descent), and "mapping" SC-55 portamento behaviour to
			// the FluidSynth range is not possible due to dynamic nature of
			// the SC-55 portamento handling. All in all, it's for the best to
			// ignore portamento altogether. This is not a great loss as it's
			// used rarely and usually only to add some subtle flair to the
			// start of the notes in synth-oriented soundtracks.
		} else {
			FluidSynth::fluid_synth_cc(synth.get(), channel, controller, value);
		}
	} break;

	case MidiStatus::ProgramChange:   FluidSynth::fluid_synth_program_change(  synth.get(), channel, msg[1]);                 break;
	case MidiStatus::ChannelPressure: FluidSynth::fluid_synth_channel_pressure(synth.get(), channel, msg[1]);                 break;
	case MidiStatus::PitchBend:       FluidSynth::fluid_synth_pitch_bend(      synth.get(), channel, msg[1] + (msg[2] << 7)); break;
	default: log_unknown_midi_message(msg); break;
	}
	// clang-format on
}

// Apply the sysex message to the service
void MidiDeviceFluidSynth::ApplySysExMessage(const std::vector<uint8_t>& msg)
{
	const char* data = reinterpret_cast<const char*>(msg.data());
	const auto n     = static_cast<int>(msg.size());

	FluidSynth::fluid_synth_sysex(synth.get(), data, n, nullptr, nullptr, nullptr, false);
}

// The callback operates at the audio frame-level, steadily adding
// samples to the mixer until the requested numbers of audio frames is
// met.
void MidiDeviceFluidSynth::MixerCallback(const int requested_audio_frames)
{
	assert(mixer_channel);

	// Report buffer underruns
	constexpr auto WarningPercent = 5.0f;

	if (const auto percent_full = audio_frame_fifo.GetPercentFull();
	    percent_full < WarningPercent) {
		static auto iteration = 0;
		if (iteration++ % 100 == 0) {
			LOG_WARNING("FSYNTH: Audio buffer underrun");
		}
		had_underruns = true;
	}

	static std::vector<AudioFrame> audio_frames = {};

	const auto has_dequeued = audio_frame_fifo.BulkDequeue(audio_frames,
	                                                       requested_audio_frames);

	if (has_dequeued) {
		assert(check_cast<int>(audio_frames.size()) == requested_audio_frames);
		mixer_channel->AddSamples_sfloat(requested_audio_frames,
		                                 &audio_frames[0][0]);

		last_rendered_ms = PIC_AtomicIndex();
	} else {
		assert(!audio_frame_fifo.IsRunning());
		mixer_channel->AddSilence();
	}
}

void MidiDeviceFluidSynth::RenderAudioFramesToFifo(const int num_audio_frames)
{
	static std::vector<AudioFrame> audio_frames = {};

	// Maybe expand the vector
	if (check_cast<int>(audio_frames.size()) < num_audio_frames) {
		audio_frames.resize(num_audio_frames);
	}

	FluidSynth::fluid_synth_write_float(synth.get(),
	                                    num_audio_frames,
	                                    &audio_frames[0][0],
	                                    0,
	                                    2,
	                                    &audio_frames[0][0],
	                                    1,
	                                    2);

	audio_frame_fifo.BulkEnqueue(audio_frames, num_audio_frames);
}

void MidiDeviceFluidSynth::ProcessWorkFromFifo()
{
	const auto work = work_fifo.Dequeue();
	if (!work) {
		return;
	}

#if 0
	// To log inter-cycle rendering
	if (work->num_pending_audio_frames > 0) {
		LOG_MSG("FSYNTH: %2u audio frames prior to %s message, followed by "
		        "%2lu more messages. Have %4lu audio frames queued",
		        work->num_pending_audio_frames,
		        work->message_type == MessageType::Channel ? "channel" : "sysex",
		        work_fifo.Size(),
		        audio_frame_fifo.Size());
	}
#endif

	if (work->num_pending_audio_frames > 0) {
		RenderAudioFramesToFifo(work->num_pending_audio_frames);
	}

	if (work->message_type == MessageType::Channel) {
		assert(work->message.size() <= MaxMidiMessageLen);
		ApplyChannelMessage(work->message);
	} else {
		assert(work->message_type == MessageType::SysEx);
		ApplySysExMessage(work->message);
	}
}

// Keep the fifo populated with freshly rendered buffers
void MidiDeviceFluidSynth::Render()
{
	while (work_fifo.IsRunning()) {
		work_fifo.IsEmpty() ? RenderAudioFramesToFifo()
		                    : ProcessWorkFromFifo();
	}
}

std_fs::path MidiDeviceFluidSynth::GetSoundFontPath()
{
	return soundfont_path;
}

std::string format_sf_line(size_t width, const std_fs::path& sf_path)
{
	assert(width > 0);
	std::vector<char> line_buf(width);

	const auto& name = sf_path.filename().string();
	const auto& path = simplify_path(sf_path).string();

	snprintf(line_buf.data(), width, "%-16s - %s", name.c_str(), path.c_str());
	std::string line = line_buf.data();

	// Formatted line did not fill the whole buffer - no further
	// formatting is necessary.
	if (line.size() + 1 < width) {
		return line;
	}

	// The description was too long and got trimmed; place three
	// dots in the end to make it clear to the user.
	const std::string cutoff = "...";

	assert(line.size() > cutoff.size());

	const auto start = line.end() - static_cast<int>(cutoff.size());
	line.replace(start, line.end(), cutoff);

	return line;
}

void FSYNTH_ListDevices(MidiDeviceFluidSynth* device, Program* caller)
{
	const size_t term_width = INT10_GetTextColumns();

	constexpr auto Indent = "  ";

	auto write_line = [&](const std_fs::path& sf_path) {
		const auto line = format_sf_line(term_width - 2, sf_path);

		const auto do_highlight = [&] {
			if (device) {
				const auto curr_sf_path = device->GetSoundFontPath();
				return curr_sf_path == sf_path;
			}
			return false;
		}();

		if (do_highlight) {
			constexpr auto Green = "[color=light-green]";
			constexpr auto Reset = "[reset]";

			const auto output = format_str("%s* %s%s\n",
			                               Green,
			                               line.c_str(),
			                               Reset);

			caller->WriteOut(convert_ansi_markup(output));
		} else {
			caller->WriteOut("%s%s\n", Indent, line.c_str());
		}
	};

	// Print SoundFont found from user config.
	std::error_code err = {};

	std::vector<std_fs::path> sf_files = {};

	// Go through all SoundFont directories and list all .sf2 files.
	for (const auto& dir_path : get_data_dirs()) {
		for (const auto& entry : std_fs::directory_iterator(dir_path, err)) {
			if (err) {
				// Problem iterating, so skip the directory
				break;
			}

			if (!entry.is_regular_file(err)) {
				// Problem with entry, move onto the
				// next one
				continue;
			}

			const auto& sf_path = entry.path();

			// Is it an .sf2 file?
			auto ext = sf_path.extension().string();
			lowcase(ext);
			if (ext != SoundFontExtension) {
				continue;
			}

			sf_files.emplace_back(sf_path);
		}
	}

	std::sort(sf_files.begin(),
	          sf_files.end(),
	          [](const std_fs::path& a, const std_fs::path& b) {
		          return a.filename() < b.filename();
	          });

	if (sf_files.empty()) {
		caller->WriteOut("%s%s\n",
		                 Indent,
		                 MSG_Get("FLUIDSYNTH_NO_SOUNDFONTS").c_str());
	} else {
		for (const auto& path : sf_files) {
			write_line(path);
		}
	}

	caller->WriteOut("\n");
}

static void fluidsynth_init([[maybe_unused]] Section* sec)
{
	if (const auto device = MIDI_GetCurrentDevice();
	    device && device->GetName() == MidiDeviceName::FluidSynth) {
		MIDI_Init();
	}
}

static void register_fluidsynth_text_messages()
{
	MSG_Add("FLUIDSYNTH_NO_SOUNDFONTS", "No available SoundFonts");

	MSG_Add("FLUIDSYNTH_INVALID_EFFECT_PARAMETER",
	        "Invalid [color=light-green]'%s'[reset] synth parameter (%s): "
	        "[color=white]%s[reset];\n"
	        "must be between %.2f and %.2f, using [color=white]'%s'[reset]");

	MSG_Add("FLUIDSYNTH_INVALID_CHORUS_WAVE",
	        "Invalid [color=light-green]'fsynth_chorus'[reset] synth parameter "
	        "(modulation wave type): [color=white]%s[reset];\n"
	        "must be [color=white]'sine'[reset] or [color=white]'triangle'[reset]");

	MSG_Add("FLUIDSYNTH_INVALID_NUM_EFFECT_PARAMS",
	        "Invalid number of [color=light-green]'%s'[reset] parameters: "
	        "[color=white]%d[/reset];\n"
	        "must be %d space-separated values, using [color=white]'auto'[reset]");
}

void FSYNTH_AddConfigSection(const ConfigPtr& conf)
{
	constexpr auto ChangeableAtRuntime = true;

	assert(conf);
	SectionProp* sec = conf->AddSectionProp("fluidsynth",
	                                          &fluidsynth_init,
	                                          ChangeableAtRuntime);
	assert(sec);
	init_fluidsynth_dosbox_settings(*sec);

	register_fluidsynth_text_messages();
}
