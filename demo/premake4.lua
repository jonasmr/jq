solution "jq"
   configurations { "Debug", "Release" }
   platforms { "x32", "x64" }
   location "build/" 
   -- A project defines one build target
   project "jq"
      kind "WindowedApp"
      language "C++"
      files { "*.h", "*.cpp", "glew/*.c", "../jq.h"}
      includedirs {"sdl2/include/", "glew/", ".." } 
      
      defines {"GLEW_STATIC;_CRT_SECURE_NO_WARNINGS"} 

      links {"SDL2"}

      debugdir "."

      configuration "windows"
         links { "opengl32", "glu32", "winmm", "dxguid"}

      configuration "Debug"
         defines { "DEBUG" }
         flags { "Symbols", "StaticRuntime" }
 
      configuration "Release"
         defines { "NDEBUG" }
         flags { "Optimize", "Symbols", "StaticRuntime" }

      configuration "x32"      
         libdirs {"sdl2/VisualC/SDL/Win32/Release/"}
      configuration "x64"      
         libdirs {"sdl2/VisualC/SDL/x64/Release/"}
