workspace "Opensegaapi"
	configurations { "Debug", "Release"}
	platforms { "x86" }

	flags { "StaticRuntime", "No64BitChecks" }

	systemversion "10.0.16299.0"

	symbols "On"

	characterset "Unicode"

	flags { "NoIncrementalLink", "NoEditAndContinue", "NoMinimalRebuild" }

	buildoptions { "/MP", "/std:c++17" }

	configuration "Debug*"
		targetdir "build/bin/debug"
		defines "NDEBUG"
		objdir "build/obj/debug"

	configuration "Release*"
		targetdir "build/bin/release"
		defines "NDEBUG"
		optimize "speed"
		objdir "build/obj/release"

	filter "platforms:x86"
		architecture "x32"

include "Opensegaapi"