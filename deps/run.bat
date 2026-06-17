@echo OFF
REM Launch the standalone demo with VTK + Qt DLLs on PATH.
set PATH=C:\programs\compa_libs\VTK-9.6.2\compileds\bin;C:\programs\Qt6\6.11.1\msvc2022_64\bin;%PATH%
set QT_QPA_PLATFORM_PLUGIN_PATH=C:\programs\Qt6\6.11.1\msvc2022_64\plugins\platforms
"%~dp0build\gmtvtk_demo.exe" %*
