; Compile with TEST_KEY (an isolated HKCU key) and TEST_OUTPUT (test EXE path).
!include "..\..\app\src-tauri\windows\nsis-hooks.nsh"
Name "VectorXR installer registration regression"
OutFile "${TEST_OUTPUT}"
RequestExecutionLevel user
SilentInstall silent

Section
  SetRegView 64
  StrCpy $0 "C:\Program Files\VectorXR\vectorxr-layer\XR_APILAYER_DIENERTECH_VECTORXR.json"
  WriteRegDWORD HKCU "${TEST_KEY}" "$0" 0
  WriteRegDWORD HKCU "${TEST_KEY}" "C:\Dev\build\Release\XR_APILAYER_DIENERTECH_VECTORXR.json" 0
  WriteRegDWORD HKCU "${TEST_KEY}" "C:\Old Build\xr_apilayer_dienertech_vectorxr.json" 0
  WriteRegDWORD HKCU "${TEST_KEY}" "C:\OtherLayer\layer.json" 0
  WriteRegDWORD HKCU "${TEST_KEY}" "C:\DisabledLayer\layer.json" 1

  !insertmacro VECTORXR_DISABLE_OTHER_LAYER_REGISTRATIONS HKCU "${TEST_KEY}" "$0"
  ; Repeat to verify upgrading/reinstalling is idempotent.
  !insertmacro VECTORXR_DISABLE_OTHER_LAYER_REGISTRATIONS HKCU "${TEST_KEY}" "$0"
  ReadRegDWORD $1 HKCU "${TEST_KEY}" "$0"
  ${If} $1 != 0
    Goto failed
  ${EndIf}
  ReadRegDWORD $1 HKCU "${TEST_KEY}" "C:\Dev\build\Release\XR_APILAYER_DIENERTECH_VECTORXR.json"
  ${If} $1 != 1
    Goto failed
  ${EndIf}
  ReadRegDWORD $1 HKCU "${TEST_KEY}" "C:\Old Build\xr_apilayer_dienertech_vectorxr.json"
  ${If} $1 != 1
    Goto failed
  ${EndIf}
  ReadRegDWORD $1 HKCU "${TEST_KEY}" "C:\OtherLayer\layer.json"
  ${If} $1 != 0
    Goto failed
  ${EndIf}
  ReadRegDWORD $1 HKCU "${TEST_KEY}" "C:\DisabledLayer\layer.json"
  ${If} $1 != 1
    Goto failed
  ${EndIf}
  DeleteRegKey HKCU "${TEST_KEY}"
  SetErrorLevel 0
  Quit
failed:
  DeleteRegKey HKCU "${TEST_KEY}"
  SetErrorLevel 1
SectionEnd
