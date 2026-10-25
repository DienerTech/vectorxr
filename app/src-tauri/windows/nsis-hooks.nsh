!macro VECTORXR_RESOLVE_LAYER_MANIFEST
  StrCpy $0 "$INSTDIR\resources\vectorxr-layer\XR_APILAYER_DIENERTECH_VECTORXR.json"
  IfFileExists "$0" done

  StrCpy $0 "$INSTDIR\vectorxr-layer\XR_APILAYER_DIENERTECH_VECTORXR.json"
  IfFileExists "$0" done

  StrCpy $0 "$INSTDIR\resources\resources\vectorxr-layer\XR_APILAYER_DIENERTECH_VECTORXR.json"

done:
!macroend

; The install notice (the license page) in each installer language; English
; is bundle.licenseFile. windows/installer.nsi inserts these after the
; languages, which LicenseLangString needs.
!define VECTORXR_WINDOWS_DIR "${__FILEDIR__}"
!macro VECTORXR_NOTICE_LANG_STRINGS
  LicenseLangString VectorXRNotice ${LANG_ENGLISH} "${LICENSE}"
  LicenseLangString VectorXRNotice ${LANG_SIMPCHINESE} "${VECTORXR_WINDOWS_DIR}\install-notice.zh-CN.txt"
  LicenseLangString VectorXRNotice ${LANG_RUSSIAN} "${VECTORXR_WINDOWS_DIR}\install-notice.ru.txt"
  LicenseLangString VectorXRNotice ${LANG_SPANISH} "${VECTORXR_WINDOWS_DIR}\install-notice.es-ES.txt"
  LicenseLangString VectorXRNotice ${LANG_PORTUGUESEBR} "${VECTORXR_WINDOWS_DIR}\install-notice.pt-BR.txt"
  LicenseLangString VectorXRNotice ${LANG_GERMAN} "${VECTORXR_WINDOWS_DIR}\install-notice.de.txt"
  LicenseLangString VectorXRNotice ${LANG_JAPANESE} "${VECTORXR_WINDOWS_DIR}\install-notice.ja.txt"
  LicenseLangString VectorXRNotice ${LANG_FRENCH} "${VECTORXR_WINDOWS_DIR}\install-notice.fr.txt"
  LicenseLangString VectorXRNotice ${LANG_POLISH} "${VECTORXR_WINDOWS_DIR}\install-notice.pl.txt"
  LicenseLangString VectorXRNotice ${LANG_KOREAN} "${VECTORXR_WINDOWS_DIR}\install-notice.ko.txt"
  LicenseLangString VectorXRNotice ${LANG_TRADCHINESE} "${VECTORXR_WINDOWS_DIR}\install-notice.zh-TW.txt"
!macroend

; The installer's language as the app's language code (app/src/i18n/locales.ts),
; in $1.
!macro VECTORXR_INSTALLER_LANGUAGE_CODE
  StrCpy $1 "en"
  ${If} $LANGUAGE == ${LANG_SIMPCHINESE}
    StrCpy $1 "zh-CN"
  ${ElseIf} $LANGUAGE == ${LANG_RUSSIAN}
    StrCpy $1 "ru"
  ${ElseIf} $LANGUAGE == ${LANG_SPANISH}
    StrCpy $1 "es-ES"
  ${ElseIf} $LANGUAGE == ${LANG_PORTUGUESEBR}
    StrCpy $1 "pt-BR"
  ${ElseIf} $LANGUAGE == ${LANG_GERMAN}
    StrCpy $1 "de"
  ${ElseIf} $LANGUAGE == ${LANG_JAPANESE}
    StrCpy $1 "ja"
  ${ElseIf} $LANGUAGE == ${LANG_FRENCH}
    StrCpy $1 "fr"
  ${ElseIf} $LANGUAGE == ${LANG_POLISH}
    StrCpy $1 "pl"
  ${ElseIf} $LANGUAGE == ${LANG_KOREAN}
    StrCpy $1 "ko"
  ${ElseIf} $LANGUAGE == ${LANG_TRADCHINESE}
    StrCpy $1 "zh-TW"
  ${EndIf}
!macroend

!macro NSIS_HOOK_POSTINSTALL
  !insertmacro VECTORXR_RESOLVE_LAYER_MANIFEST

  ; The app starts in the language picked here until one is picked in its
  ; Settings (installer_language in src-tauri/src/main.rs).
  Push $1
  Push $2
  !insertmacro VECTORXR_INSTALLER_LANGUAGE_CODE
  ClearErrors
  FileOpen $2 "$INSTDIR\installer-language.txt" w
  ${IfNot} ${Errors}
    FileWrite $2 "$1"
    FileClose $2
  ${EndIf}
  Pop $2
  Pop $1

  SetRegView 64
  !insertmacro VECTORXR_DISABLE_OTHER_LAYER_REGISTRATIONS HKLM "Software\Khronos\OpenXR\1\ApiLayers\Implicit" "$0"
  WriteRegDWORD HKLM "Software\Khronos\OpenXR\1\ApiLayers\Implicit" "$0" 0
  DetailPrint "Registered VectorXR OpenXR layer manifest: $0"
!macroend

; Runs after the uninstaller has asked the user to close VectorXR and removed
; the files. As a pre-uninstall hook it ran before that check, so Cancel there
; left VectorXR installed but unregistered. The files are gone by now, so
; remove every manifest location VECTORXR_RESOLVE_LAYER_MANIFEST can register.
; An update keeps the registration, and with it VectorXR's place in the OpenXR
; layer order; the new version's install then rewrites it in place.
!macro NSIS_HOOK_POSTUNINSTALL
  ${If} $UpdateMode <> 1
    SetRegView 64
    DeleteRegValue HKLM "Software\Khronos\OpenXR\1\ApiLayers\Implicit" "$INSTDIR\resources\vectorxr-layer\XR_APILAYER_DIENERTECH_VECTORXR.json"
    DeleteRegValue HKLM "Software\Khronos\OpenXR\1\ApiLayers\Implicit" "$INSTDIR\vectorxr-layer\XR_APILAYER_DIENERTECH_VECTORXR.json"
    DeleteRegValue HKLM "Software\Khronos\OpenXR\1\ApiLayers\Implicit" "$INSTDIR\resources\resources\vectorxr-layer\XR_APILAYER_DIENERTECH_VECTORXR.json"
    Delete "$INSTDIR\installer-language.txt"
    RMDir "$INSTDIR"
    DetailPrint "Unregistered the VectorXR OpenXR layer manifest from $INSTDIR"
    !insertmacro VECTORXR_DELETE_USER_DATA
  ${EndIf}
!macroend

; VectorXR's settings, logs and runtime state. Tests point this elsewhere.
!ifndef VECTORXR_DATA_DIR
  !define VECTORXR_DATA_DIR "$LOCALAPPDATA\VectorXR"
!endif

; The uninstaller's "Delete the application data" checkbox only removes
; folders named after the bundle identifier (local.vectorxr.app), which hold
; the app's WebView data. VectorXR's own data lives under its product name, so
; remove it too when the box is checked. A game still running with the layer
; loaded can hold a log open; that file stays and the uninstall says so.
; RMDir /r does not set the error flag for a file it cannot delete, so check
; whether the folder is still there instead.
!macro VECTORXR_DELETE_USER_DATA
  ${If} $DeleteAppDataCheckboxState = 1
    SetShellVarContext current
    RMDir /r "${VECTORXR_DATA_DIR}"
    ${If} ${FileExists} "${VECTORXR_DATA_DIR}\*.*"
      DetailPrint "Some VectorXR data in ${VECTORXR_DATA_DIR} is in use and was not removed. Close any VR apps and delete that folder."
    ${Else}
      DetailPrint "Removed VectorXR settings and logs from ${VECTORXR_DATA_DIR}"
    ${EndIf}
  ${EndIf}
!macroend
!include "FileFunc.nsh"
!include "LogicLib.nsh"

; Share the registration rule with the isolated registry regression harness.
; Changing values (rather than deleting them) keeps enumeration indices stable.
!macro VECTORXR_DISABLE_OTHER_LAYER_REGISTRATIONS ROOT KEY MANIFEST
  Push $1
  Push $2
  Push $3
  StrCpy $1 0
  ${Do}
    EnumRegValue $2 ${ROOT} "${KEY}" $1
    ${If} $2 == ""
      ${ExitDo}
    ${EndIf}
    ${If} $2 != "${MANIFEST}"
      ${GetFileName} "$2" $3
      ${If} $3 == "XR_APILAYER_DIENERTECH_VECTORXR.json"
        WriteRegDWORD ${ROOT} "${KEY}" "$2" 1
      ${EndIf}
    ${EndIf}
    IntOp $1 $1 + 1
  ${Loop}
  Pop $3
  Pop $2
  Pop $1
!macroend
