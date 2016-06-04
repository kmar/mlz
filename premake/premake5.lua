workspace "mlz"
	configurations { "Debug", "Release" }
	platforms { "x32", "x64" }

project "mlz"
	kind "StaticLib"
	language "C"
	targetdir "bin/%{cfg.buildcfg}"
	files { "../mlz_dec.c", "../mlz_enc.c", "../mlz_stream_enc.c", "../mlz_stream_dec.c", "../mlz_thread.c",
		"../mlz_common.h", "../mlz_dec.h", "../mlz_enc.h", "../mlz_stream_common.h", "../mlz_stream_dec.h",
		"../mlz_stream_enc.h", "../mlz_version.h" }
	filter "configurations:Debug"
		defines { "DEBUG", "MLZ_THREADS" }
		flags { "Symbols" }
	filter "configurations:Release"
		defines { "NDEBUG", "MLZ_THREADS" }
		optimize "On"

project "mlzc"
	kind "ConsoleApp"
	language "C"
	targetdir "bin/%{cfg.buildcfg}"
	files { "../mlzc.c" }
	links { "mlz" }

	filter "system:not windows"
		links { "pthread" }

	filter "configurations:Debug"
		defines { "DEBUG", "MLZ_THREADS", "MLZ_COMMANDLINE_TOOL" }
		flags { "Symbols" }
	filter "configurations:Release"
		defines { "NDEBUG", "MLZ_THREADS", "MLZ_COMMANDLINE_TOOL" }
		optimize "On"
