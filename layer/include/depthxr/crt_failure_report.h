#pragma once

namespace depthxr {

class Logger;

// The layer carries its own C runtime (/MT), so a game's crash handlers registered in its runtime never see
// the layer's fatal runtime failures: abort() (which std::terminate calls, for example when an exception
// leaves a layer thread), a pure virtual call, or an invalid parameter to a runtime function. Left alone,
// each one fails fast: the game vanishes without its crash report or a line in the VectorXR log. Installed,
// each one is logged and then handed to the process's unhandled-exception filter (the game's crash reporter,
// or Windows Error Reporting), and the process ends as before. Installs into the calling module's runtime only.
void InstallCrtFailureReporting();

// The log that names such a failure; nullptr detaches it (before the logger is destroyed).
void SetCrtFailureLogger(Logger* logger);

} // namespace depthxr
