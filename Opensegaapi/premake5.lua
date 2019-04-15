project "Opensegaapi"
	targetname "Opensegaapi"
	language "C++"
	kind "SharedLib"
	removeplatforms { "x64" }

	files
	{
		"src/**.cpp", "src/**.h",
		"deps/cpp/**.cpp", "deps/inc/**.h",
		"src/Opensegaapi.aps", "src/Opensegaapi.rc"
	}

	includedirs { "src" }

postbuildcommands {
  "if not exist $(TargetDir)output mkdir $(TargetDir)output",
  "{COPY} $(TargetDir)Opensegaapi.dll $(TargetDir)output/"
}