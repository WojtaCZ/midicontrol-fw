#pragma once
// Host stand-in for the build-generated inc/git.hpp: fixed values so the exported images are stable.
// Same shape as the generated header (char arrays, not std::string).
namespace git {
	inline constexpr char revision[]   = "a1b2c3d";
	inline constexpr char branch[]     = "main";
	inline constexpr char build_date[] = "2026/10/25";
	inline constexpr char build_time[] = "12:00:00";
}
