!macro VECTORXR_RESOLVE_LAYER_MANIFEST
  StrCpy $0 "$INSTDIR\resources\vectorxr-layer\XR_APILAYER_DIENERTECH_VECTORXR.json"
  IfFileExists "$0" done

  StrCpy $0 "$INSTDIR\vectorxr-layer\XR_APILAYER_DIENERTECH_VECTORXR.json"
  IfFileExists "$0" done

  StrCpy $0 "$INSTDIR\resources\resources\vectorxr-layer\XR_APILAYER_DIENERTECH_VECTORXR.json"

done:
!macroend

!macro NSIS_HOOK_POSTINSTALL
  !insertmacro VECTORXR_RESOLVE_LAYER_MANIFEST

  SetRegView 64
  !insertmacro VECTORXR_DISABLE_OTHER_LAYER_REGISTRATIONS HKLM "Software\Khronos\OpenXR\1\ApiLayers\Implicit" "$0"
  WriteRegDWORD HKLM "Software\Khronos\OpenXR\1\ApiLayers\Implicit" "$0" 0
  DetailPrint "Registered VectorXR OpenXR layer manifest: $0"
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  !insertmacro VECTORXR_RESOLVE_LAYER_MANIFEST

  SetRegView 64
  DeleteRegValue HKLM "Software\Khronos\OpenXR\1\ApiLayers\Implicit" "$0"
  DetailPrint "Unregistered VectorXR OpenXR layer manifest: $0"
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
