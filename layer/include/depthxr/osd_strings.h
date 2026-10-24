#pragma once
#include <string>
#include <string_view>

namespace depthxr {

struct OsdSettings;

// The languages the OSD draws in: the app's languages (app/src/i18n/locales.ts),
// compiled into the layer so a game process reads no extra file.
enum class OsdLanguage { En, ZhCn, Ru, EsEs, PtBr, De, Ja, Fr, Pl, Ko, ZhTw, Count };

// Every text the OSD draws that is not data (the app's name, the runtime's,
// a profile's). Some are fragments joined in a fixed order; {0} marks where a
// value goes.
enum class OsdText {
    AppFps, AppFrameAvg, AppFrameP95, MsAvg, FrameTimeGraph, Performance, PerformanceCompact,
    Turbo, FrameGen, Pivot, Enhancements, ExperimentalSuffix, ExperimentalCompactSuffix, RestartPending, Am, Pm,
    BadgeOn, BadgeStandby, BadgeWait, BadgePaused, BadgeOff, BadgeUnavailable,
    Disabled, Off, Active, Suspended, TurboPausedForFrameGen, TurboEnabledOff, TurboRecoveryBlocked, TurboAsync,
    TurboSequenced, TurboEnabledWaiting, TurboAnalyzingSuffix,
    ModuleDepth, ModulePivot, ModuleTurbo, ModuleFrameGen, ModuleQuadviews,
    Motion, MotionReleasing, MotionContinuousShort, MotionSteppedShort, MotionContinuous, MotionStepped,
    PlusPosition, PlusNudge, SnapReturning, SnapEntering, SnapActive, SnapOff, SnapOffNudge, StackedSuffix,
    RestartTurboExperiments, RestartQuadviews, RestartFrameGeneration,
    DiagnosticsDebugLog, DiagnosticsFlowView, DiagnosticsConfidenceView, DiagnosticsTintView,
    FlowHardware, FlowShader, DiagnosticsResearch,
    FpsTarget, CpuBound, GpuBound, CpuThread, CpuPipelined, CpuTurboPaces, CpuWaiting, GpuBusy, GpuGame,
    GpuNoCounters, GpuReading, VramOf, VramUnknown,
    Count
};

// The language core.osd.language and core.osd.appLanguage name: "app" (the
// default) follows the app's language, which the app saves as appLanguage;
// without one, the Windows display language. Unknown codes (a newer app's)
// fall back the same way.
OsdLanguage ResolveOsdLanguage(const OsdSettings& settings);
// A code the app writes ("de", "zh-CN"), or Count when this layer has no such
// language. "app" is not a language.
OsdLanguage OsdLanguageFromCode(std::string_view code);
std::string_view OsdLanguageCode(OsdLanguage language);
// The language matching a BCP 47 tag (Windows' "de-AT", "zh-Hant-TW"), as the
// app's matchLocale does; Count when none does.
OsdLanguage OsdLanguageFromTag(std::string_view tag);

// UTF-8 text for the OSD.
const char* OsdString(OsdLanguage language, OsdText text);
// The text with {0} replaced by `value`.
std::string OsdFormat(OsdLanguage language, OsdText text, std::string_view value);
// GDI face and charset for the language: the Windows UI font of its script, so
// Chinese, Japanese and Korean get their own glyph forms rather than whatever
// font linking picks for Segoe UI.
const wchar_t* OsdFontFace(OsdLanguage language);
unsigned char OsdFontCharset(OsdLanguage language);

} // namespace depthxr
