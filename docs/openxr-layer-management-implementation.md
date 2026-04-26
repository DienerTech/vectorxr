# OpenXR Layer Management Implementation Note

VectorXR manages implicit OpenXR API layers as four separate registry slices:

- `HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit`
- `HKCU\Software\Khronos\OpenXR\1\ApiLayers\Implicit`
- `HKLM\Software\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit`
- `HKCU\Software\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit`

The app presents machine-wide 64-bit registration as the recommended path and labels the other slices as uncommon. Layer order is displayed and changed only within a single slice; VectorXR does not combine HKLM, HKCU, 64-bit, and 32-bit registrations into one global order.

Reorder actions are adjacent `Move Up` / `Move Down` operations. Internally, VectorXR rewrites the values in the selected registry key in the requested order while preserving each layer's enabled value.

HKLM writes still require elevation. In an unelevated app session, write failures are surfaced as user-facing status text rather than blocking read-only inspection.
