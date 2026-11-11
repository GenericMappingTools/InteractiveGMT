@echo off
rem Julia of the iGMT install, on the install's own depot (where GMT.jl and InteractiveGMT are).
rem Without this the bundled julia would use ~/.julia. Set for this window only.
set "JULIA_DEPOT_PATH=%~dp0depot;"
"%~dp0julia\bin\julia.exe" %*
