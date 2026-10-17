; wmpmeta setup: installs wmpmeta.exe, points windowsmedia.com at it in the
; hosts file, starts it at logon and registers an uninstaller in Add/Remove
; Programs. Built by `make setup`:
;   makensis -DVERSION=x.y.z -DSRCDIR=build/dist/wmpmeta -DOUTFILE=... wmpmeta.nsi

Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "StrFunc.nsh"
${StrStr}
${UnStrStr}

!define APP      "wmpmeta"
!define UNINST   "Software\Microsoft\Windows\CurrentVersion\Uninstall\wmpmeta"
!define RUNKEY   "Software\Microsoft\Windows\CurrentVersion\Run"
!define URL      "https://github.com/savaughn/wmpmeta"

Name "wmpmeta ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES\wmpmeta"
InstallDirRegKey HKLM "${UNINST}" "InstallLocation"
RequestExecutionLevel admin
BrandingText "wmpmeta ${VERSION}"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This installs wmpmeta, which gives Windows Media Player 9 CD album and track names from MusicBrainz.$\r$\n$\r$\nIt adds windowsmedia.com and go.microsoft.com lines to the hosts file and starts wmpmeta.exe at logon.$\r$\n$\r$\nYou need an Administrator account."
!define MUI_FINISHPAGE_TITLE "wmpmeta is running"
!define MUI_FINISHPAGE_TEXT "Close Windows Media Player and open it again, then insert a CD.$\r$\n$\r$\nLookups are logged to wmpmeta.log in the install folder."
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Show the README"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "wmpmeta"
VIAddVersionKey "FileDescription" "wmpmeta setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "MIT License"

; ---------------------------------------------------------------- hosts

Var hosts

Function AddHosts
    StrCpy $hosts "$SYSDIR\drivers\etc\hosts"
    ; Lines already there (reinstall, upgrade or hand-edited) aren't added again.
    StrCpy $6 ""   ; windowsmedia.com seen
    StrCpy $7 ""   ; go.microsoft.com seen
    ClearErrors
    FileOpen $0 $hosts r
    ${IfNot} ${Errors}
        ${Do}
            FileRead $0 $1
            ${If} ${Errors}
                ${ExitDo}
            ${EndIf}
            ${StrStr} $2 $1 "windowsmedia.com"
            ${If} $2 != ""
                StrCpy $6 1
            ${EndIf}
            ${StrStr} $2 $1 "go.microsoft.com"
            ${If} $2 != ""
                StrCpy $7 1
            ${EndIf}
        ${Loop}
        FileClose $0
    ${EndIf}
    ${If} $6 == 1
    ${AndIf} $7 == 1
        DetailPrint "hosts already has the wmpmeta entries"
        Return
    ${EndIf}
    ${IfNot} ${FileExists} "$hosts.bak-wmpmeta"
        CopyFiles /SILENT "$hosts" "$hosts.bak-wmpmeta"
    ${EndIf}
    FileOpen $0 $hosts a
    ; Start on a new line, without stacking blank lines on reinstalls.
    FileSeek $0 -1 END
    FileReadByte $0 $1
    FileSeek $0 0 END
    ${If} $1 != 10
        FileWrite $0 "$\r$\n"
    ${EndIf}
    ${If} $6 != 1
        FileWrite $0 "# wmpmeta: WMP CD info from MusicBrainz$\r$\n"
        FileWrite $0 "127.0.0.1$\twindowsmedia.com$\r$\n"
        FileWrite $0 "127.0.0.1$\twww.windowsmedia.com$\r$\n"
        FileWrite $0 "127.0.0.1$\tservices.windowsmedia.com$\r$\n"
    ${EndIf}
    ${If} $7 != 1
        FileWrite $0 "127.0.0.1$\tgo.microsoft.com$\t# wmpmeta: More Skins gallery$\r$\n"
    ${EndIf}
    FileClose $0
    DetailPrint "Added the wmpmeta entries to hosts (backup: hosts.bak-wmpmeta)"
FunctionEnd

Function un.RemoveHosts
    StrCpy $hosts "$SYSDIR\drivers\etc\hosts"
    ClearErrors
    FileOpen $0 $hosts r
    ${If} ${Errors}
        Return
    ${EndIf}
    GetTempFileName $3
    FileOpen $4 $3 w
    ${Do}
        FileRead $0 $1
        ${If} ${Errors}
            ${ExitDo}
        ${EndIf}
        ${UnStrStr} $2 $1 "windowsmedia.com"
        ${UnStrStr} $5 $1 "# wmpmeta"
        ${If} $2 == ""
        ${AndIf} $5 == ""
            FileWrite $4 $1
        ${EndIf}
    ${Loop}
    FileClose $0
    FileClose $4
    CopyFiles /SILENT $3 $hosts
    Delete $3
    DetailPrint "Removed the wmpmeta entries from hosts"
FunctionEnd

; ---------------------------------------------------------------- install

Section "wmpmeta"
    SetShellVarContext all
    ; Upgrade: stop the running copy so its exe can be replaced.
    ${If} ${FileExists} "$INSTDIR\wmpmeta.exe"
        ExecWait '"$INSTDIR\wmpmeta.exe" -stop'
        Sleep 1500
    ${EndIf}

    SetOutPath "$INSTDIR"
    File "${SRCDIR}\wmpmeta.exe"
    File "${SRCDIR}\README.txt"
    File "${SRCDIR}\LICENSE.txt"
    File "${SRCDIR}\LICENSE-mbedtls.txt"
    File "${SRCDIR}\LICENSE-cjson.txt"
    File "${SRCDIR}\THIRD_PARTY_NOTICES.txt"
    WriteUninstaller "$INSTDIR\uninstall.exe"

    Call AddHosts
    WriteRegStr HKLM "${RUNKEY}" "wmpmeta" '"$INSTDIR\wmpmeta.exe"'

    WriteRegStr   HKLM "${UNINST}" "DisplayName" "wmpmeta (WMP CD info from MusicBrainz)"
    WriteRegStr   HKLM "${UNINST}" "DisplayVersion" "${VERSION}"
    WriteRegStr   HKLM "${UNINST}" "Publisher" "savaughn"
    WriteRegStr   HKLM "${UNINST}" "URLInfoAbout" "${URL}"
    WriteRegStr   HKLM "${UNINST}" "InstallLocation" "$INSTDIR"
    WriteRegStr   HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\uninstall.exe"
    WriteRegStr   HKLM "${UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
    WriteRegDWORD HKLM "${UNINST}" "EstimatedSize" 700

    nsExec::Exec "ipconfig /flushdns"
    Pop $0
    Exec '"$INSTDIR\wmpmeta.exe"'
    DetailPrint "Started wmpmeta"
SectionEnd

; ---------------------------------------------------------------- uninstall

Section "Uninstall"
    ExecWait '"$INSTDIR\wmpmeta.exe" -stop'
    Sleep 1500
    DeleteRegValue HKLM "${RUNKEY}" "wmpmeta"
    DeleteRegKey HKLM "${UNINST}"
    Call un.RemoveHosts
    nsExec::Exec "ipconfig /flushdns"
    Pop $0

    Delete "$INSTDIR\wmpmeta.exe"
    Delete "$INSTDIR\wmpmeta.log"
    Delete "$INSTDIR\wmpmeta.ini"
    Delete "$INSTDIR\wayback-services.html"
    Delete "$INSTDIR\wmpmeta-albums.txt"
    RMDir /r "$INSTDIR\skins"
    Delete "$INSTDIR\README.txt"
    Delete "$INSTDIR\LICENSE.txt"
    Delete "$INSTDIR\LICENSE-mbedtls.txt"
    Delete "$INSTDIR\LICENSE-cjson.txt"
    Delete "$INSTDIR\THIRD_PARTY_NOTICES.txt"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
