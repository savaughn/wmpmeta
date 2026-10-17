@echo off
rem wmpmeta installer: points windowsmedia.com at this PC, starts wmpmeta.exe
rem at logon from this folder, and starts it now. Run it from the folder you
rem want to keep wmpmeta in. Needs an Administrator account (hosts, HKLM).
setlocal
set DIR=%~dp0
set EXE=%DIR%wmpmeta.exe
set HOSTS=%SystemRoot%\system32\drivers\etc\hosts

if not exist "%EXE%" (
    echo wmpmeta.exe is not next to install.cmd.
    goto end
)

rem hosts: only add the lines once.
if not exist "%HOSTS%.bak-wmpmeta" copy "%HOSTS%" "%HOSTS%.bak-wmpmeta" >nul
find /i "windowsmedia.com" "%HOSTS%" >nul 2>&1
if not errorlevel 1 (
    echo hosts already has windowsmedia.com entries.
    goto golink
)
>>"%HOSTS%" echo.
>>"%HOSTS%" echo # wmpmeta: WMP CD info from MusicBrainz
>>"%HOSTS%" echo 127.0.0.1	windowsmedia.com
>>"%HOSTS%" echo 127.0.0.1	www.windowsmedia.com
>>"%HOSTS%" echo 127.0.0.1	services.windowsmedia.com
echo Added windowsmedia.com to hosts (backup: hosts.bak-wmpmeta).

:golink
rem The More Skins button goes through go.microsoft.com.
find /i "go.microsoft.com" "%HOSTS%" >nul 2>&1
if errorlevel 1 (
    >>"%HOSTS%" echo 127.0.0.1	go.microsoft.com	# wmpmeta: More Skins gallery
    echo Added go.microsoft.com to hosts.
)

:run
rem Autostart: a .reg file needs every backslash doubled.
set REGEXE=%EXE:\=\\%
> "%TEMP%\wmpmeta-run.reg" echo REGEDIT4
>>"%TEMP%\wmpmeta-run.reg" echo.
>>"%TEMP%\wmpmeta-run.reg" echo [HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Run]
>>"%TEMP%\wmpmeta-run.reg" echo "wmpmeta"="%REGEXE%"
regedit /s "%TEMP%\wmpmeta-run.reg"
del "%TEMP%\wmpmeta-run.reg"
echo wmpmeta will start at logon.

ipconfig /flushdns >nul 2>&1
"%EXE%" -stop
ping -n 3 127.0.0.1 >nul
start "" "%EXE%"
echo wmpmeta is running. Close and reopen Windows Media Player, then insert a CD.
echo Requests and lookups are logged to %DIR%wmpmeta.log

:end
endlocal
if /i not "%1"=="/q" pause
