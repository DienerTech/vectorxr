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
  WriteRegDWORD HKLM "Software\Khronos\OpenXR\1\ApiLayers\Implicit" "$0" 0
  DetailPrint "Registered VectorXR OpenXR layer manifest: $0"
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  !insertmacro VECTORXR_RESOLVE_LAYER_MANIFEST

  SetRegView 64
  DeleteRegValue HKLM "Software\Khronos\OpenXR\1\ApiLayers\Implicit" "$0"
  DetailPrint "Unregistered VectorXR OpenXR layer manifest: $0"
!macroend
