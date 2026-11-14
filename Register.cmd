@echo off
rem Registers LiteView for the current user (no admin needed), then opens Windows "Default apps".
set EXE=%~dp0LiteView.exe
set PROG=HKCU\Software\Classes\LiteView.Media
set CAP=HKCU\Software\LiteView\Capabilities

reg add "%PROG%" /ve /d "LiteView media" /f >nul
reg add "%PROG%\shell\open\command" /ve /d "\"%EXE%\" \"%%1\"" /f >nul
reg add "HKCU\Software\Classes\Applications\LiteView.exe\shell\open\command" /ve /d "\"%EXE%\" \"%%1\"" /f >nul

reg add "%CAP%" /v ApplicationName /d "LiteView" /f >nul
reg add "%CAP%" /v ApplicationDescription /d "Instant photo and video viewer" /f >nul

for %%e in (jpg jpeg jfif png gif webp bmp tif tiff avif jxl ico apng mp4 mkv avi mov wmv webm flv m4v ts mpg mpeg 3gp m2ts ogv vob) do (
  reg add "%CAP%\FileAssociations" /v .%%e /d "LiteView.Media" /f >nul
  reg add "HKCU\Software\Classes\Applications\LiteView.exe\SupportedTypes" /v .%%e /d "" /f >nul
)

reg add "HKCU\Software\RegisteredApplications" /v LiteView /d "Software\LiteView\Capabilities" /f >nul
echo Done. In the window that opens, pick LiteView as default for photos and videos.
start ms-settings:defaultapps
pause
