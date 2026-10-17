@echo off
rem wmpmeta uninstaller: stops wmpmeta, removes the logon entry and the
rem windowsmedia.com lines from hosts. The folder itself is left in place.
setlocal
set DIR=%~dp0
set HOSTS=%SystemRoot%\system32\drivers\etc\hosts

if exist "%DIR%wmpmeta.exe" "%DIR%wmpmeta.exe" -stop

> "%TEMP%\wmpmeta-unrun.reg" echo REGEDIT4
>>"%TEMP%\wmpmeta-unrun.reg" echo.
>>"%TEMP%\wmpmeta-unrun.reg" echo [HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Run]
>>"%TEMP%\wmpmeta-unrun.reg" echo "wmpmeta"=-
regedit /s "%TEMP%\wmpmeta-unrun.reg"
del "%TEMP%\wmpmeta-unrun.reg"
echo Removed the logon entry.

find /i "windowsmedia.com" "%HOSTS%" >nul 2>&1
if errorlevel 1 goto flush
findstr /v /i /c:"windowsmedia.com" /c:"# wmpmeta" "%HOSTS%" > "%TEMP%\wmpmeta-hosts.txt"
copy /y "%TEMP%\wmpmeta-hosts.txt" "%HOSTS%" >nul
del "%TEMP%\wmpmeta-hosts.txt"
echo Removed the wmpmeta lines from hosts.

:flush
ipconfig /flushdns >nul 2>&1
echo Done. Close and reopen Windows Media Player.
endlocal
if /i not "%1"=="/q" pause
