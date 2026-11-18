@echo off
rem Registers LiteView for the current user (no admin needed), then opens Windows "Default apps".
set EXE=%~dp0LiteView.exe
set PROG=HKCU\Software\Classes\LiteView.Media
set CAP=HKCU\Software\LiteView\Capabilities

reg add "%PROG%" /ve /d "LiteView media" /f >nul
reg add "%PROG%\shell\open\command" /ve /d "\"%EXE%\" \"%%1\"" /f >nul
reg add "HKCU\Software\Classes\Applications\LiteView.exe\shell\open\command" /ve /d "\"%EXE%\" \"%%1\"" /f >nul

reg add "%PROG%\DefaultIcon" /ve /d "\"%EXE%\",0" /f >nul
reg add "HKCU\Software\Classes\Applications\LiteView.exe\DefaultIcon" /ve /d "\"%EXE%\",0" /f >nul
reg add "%CAP%" /v ApplicationName /d "LiteView" /f >nul
reg add "%CAP%" /v ApplicationIcon /d "\"%EXE%\",0" /f >nul
reg add "%CAP%" /v ApplicationDescription /d "Instant photo and video viewer" /f >nul

for %%e in (jpg jpeg jpe jfif jif png apng gif webp bmp dib tif tiff avif jxl ico cur tga pcx pbm pgm ppm pnm pam pfm xbm xpm sgi ras xwd exr hdr dds psd jp2 j2k jpf jpx qoi dpx fits heic heif hif jxr wdp hdp dng cr2 cr3 crw nef nrw arw srf sr2 raf orf rw2 rwl pef srw x3f 3fr erf kdc dcr mrw mef mos iiq mp4 m4v mp4v mkv mk3d webm avi mov qt wmv asf flv f4v mts m2ts m2t tp trp mpg mpeg mpe m2v vob ogv ogm 3gp 3g2 3gpp divx xvid rm rmvb hevc h265 265 h264 264 avc mxf dv wtv dvr-ms nsv y4m ivf mjpeg mjpg vc1 amv swf evo bik smk mp3 flac wav ogg oga opus m4a aac wma ape wv aiff aif alac mka ac3 dts mp2 mpc tta caf) do (
  reg add "%CAP%\FileAssociations" /v .%%e /d "LiteView.Media" /f >nul
  reg add "HKCU\Software\Classes\Applications\LiteView.exe\SupportedTypes" /v .%%e /d "" /f >nul
)

reg add "HKCU\Software\RegisteredApplications" /v LiteView /d "Software\LiteView\Capabilities" /f >nul
echo Done. In the window that opens, pick LiteView as default for photos and videos.
start ms-settings:defaultapps
pause
