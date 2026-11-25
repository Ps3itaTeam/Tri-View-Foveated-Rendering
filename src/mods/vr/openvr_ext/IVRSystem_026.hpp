// IVRSystem_026.hpp
//
// Minimaler Zugriff auf die Eye-Tracking-API von OpenVR SDK 2.15.6 (Maerz 2026), OHNE den gesamten
// mitgelieferten SDK-Header (1.16.8) auszutauschen.
//
// Warum ein Shim statt Header-Update?
//   * openvr_api.dll/.lib sind nur der Lader: VR_InitInternal / VR_GetGenericInterface haben seit
//     Jahren dieselbe ABI. Die Interface-Version ("IVRSystem_022" vs "IVRSystem_026") wird erst zur
//     Laufzeit angefragt.
//   * Ein Komplett-Update des Headers wuerde IVRSystem_026 + IVRInput_011 + ... fuer JEDEN Start
//     erzwingen -- mit einem aelteren SteamVR-Runtime scheitert dann VR_Init (Init_InterfaceNotFound)
//     und das ganze VR-Mod startet nicht mehr. Der Shim fragt "IVRSystem_026" separat an und faellt
//     bei Fehlschlag sauber auf "kein Eye Tracking" zurueck.
//
// Die Reihenfolge der virtuellen Methoden ist die von IVRSystem_026 (openvr_api.cs / openvr_capi.h des
// SDK 2.15.6). Gegenueber IVRSystem_022 (1.16.8) kommen hinzu:
//   - PollNextEventWithPoseAndOverlays   (seit 023)
//   - ComputeDistortionSet               (026)
//   - GetEyeTrackedFoveationCenter(+ForProjection) (026)
//   - SetSDKVersion                      (026, ganz am Ende)
// Nur die Slots, die wir aufrufen, haben echte Signaturen; alle anderen sind Platzhalter, die nie
// aufgerufen werden -- wichtig ist ausschliesslich die Position in der vtable.
#pragma once

#include <openvr.h>

namespace vr_ext {

static const char* const IVRSystem_026_Version = "IVRSystem_026";

// SDK-Version, die wir dem Runtime melden koennen (siehe IVRSystem_026::SetSDKVersion)
static const uint32_t k_nSdkMajor = 2;
static const uint32_t k_nSdkMinor = 15;
static const uint32_t k_nSdkBuild = 6;

class IVRSystem_026 {
public:
    // --- Slots 0..34 (nie aufgerufen) -----------------------------------------------------------
    virtual void slot_GetRecommendedRenderTargetSize() = 0;                 // 0
    virtual void slot_GetProjectionMatrix() = 0;                            // 1
    virtual void slot_GetProjectionRaw() = 0;                               // 2
    virtual void slot_ComputeDistortion() = 0;                              // 3
    virtual void slot_ComputeDistortionSet() = 0;                           // 4  (neu in 026)
    virtual void slot_GetEyeToHeadTransform() = 0;                          // 5
    virtual void slot_GetTimeSinceLastVsync() = 0;                          // 6
    virtual void slot_GetD3D9AdapterIndex() = 0;                            // 7
    virtual void slot_GetDXGIOutputInfo() = 0;                              // 8
    virtual void slot_GetOutputDevice() = 0;                                // 9
    virtual void slot_IsDisplayOnDesktop() = 0;                             // 10
    virtual void slot_SetDisplayVisibility() = 0;                           // 11
    virtual void slot_GetDeviceToAbsoluteTrackingPose() = 0;                // 12
    virtual void slot_GetSeatedZeroPoseToStandingAbsoluteTrackingPose() = 0;// 13
    virtual void slot_GetRawZeroPoseToStandingAbsoluteTrackingPose() = 0;   // 14
    virtual void slot_GetSortedTrackedDeviceIndicesOfClass() = 0;           // 15
    virtual void slot_GetTrackedDeviceActivityLevel() = 0;                  // 16
    virtual void slot_ApplyTransform() = 0;                                 // 17
    virtual void slot_GetTrackedDeviceIndexForControllerRole() = 0;         // 18
    virtual void slot_GetControllerRoleForTrackedDeviceIndex() = 0;         // 19
    virtual void slot_GetTrackedDeviceClass() = 0;                          // 20
    virtual void slot_IsTrackedDeviceConnected() = 0;                       // 21
    virtual void slot_GetBoolTrackedDeviceProperty() = 0;                   // 22
    virtual void slot_GetFloatTrackedDeviceProperty() = 0;                  // 23
    virtual void slot_GetInt32TrackedDeviceProperty() = 0;                  // 24
    virtual void slot_GetUint64TrackedDeviceProperty() = 0;                 // 25
    virtual void slot_GetMatrix34TrackedDeviceProperty() = 0;               // 26
    virtual void slot_GetArrayTrackedDeviceProperty() = 0;                  // 27
    virtual void slot_GetStringTrackedDeviceProperty() = 0;                 // 28
    virtual void slot_GetPropErrorNameFromEnum() = 0;                       // 29
    virtual void slot_PollNextEvent() = 0;                                  // 30
    virtual void slot_PollNextEventWithPose() = 0;                          // 31
    virtual void slot_PollNextEventWithPoseAndOverlays() = 0;               // 32 (neu seit 023)
    virtual void slot_GetEventTypeNameFromEnum() = 0;                       // 33
    virtual void slot_GetHiddenAreaMesh() = 0;                              // 34

    // --- Eye Tracking (neu in 026) --------------------------------------------------------------
    // Pro-Auge-Foveationszentrum in NDC, bezogen auf die Projektionsmatrizen von
    // IVRSystem::GetProjectionMatrix(). false = kein Eye Tracker oder Blick gerade ungueltig.
    virtual bool GetEyeTrackedFoveationCenter(vr::HmdVector2_t* pNdcLeft, vr::HmdVector2_t* pNdcRight) = 0;      // 35
    // Variante mit eigener Projektionsmatrix (z.B. die tatsaechlich gerenderte).
    virtual bool GetEyeTrackedFoveationCenterForProjection(const vr::HmdMatrix44_t* pProjMat, vr::HmdVector2_t* pNdc) = 0; // 36

    // --- Slots 37..49 (nie aufgerufen) ----------------------------------------------------------
    virtual void slot_GetControllerState() = 0;                             // 37
    virtual void slot_GetControllerStateWithPose() = 0;                     // 38
    virtual void slot_TriggerHapticPulse() = 0;                             // 39
    virtual void slot_GetButtonIdNameFromEnum() = 0;                        // 40
    virtual void slot_GetControllerAxisTypeNameFromEnum() = 0;              // 41
    virtual void slot_IsInputAvailable() = 0;                               // 42
    virtual void slot_IsSteamVRDrawingControllers() = 0;                    // 43
    virtual void slot_ShouldApplicationPause() = 0;                         // 44
    virtual void slot_ShouldApplicationReduceRenderingWork() = 0;           // 45
    virtual void slot_PerformFirmwareUpdate() = 0;                          // 46
    virtual void slot_AcknowledgeQuit_Exiting() = 0;                        // 47
    virtual void slot_GetAppContainerFilePaths() = 0;                       // 48
    virtual void slot_GetRuntimeVersion() = 0;                              // 49

    // Teilt dem Runtime mit, gegen welche SDK-Version der Client gebaut wurde. Der Runtime darf
    // neuere Features (und neue Standardverhalten) davon abhaengig machen.
    virtual vr::EVRInitError SetSDKVersion(uint32_t nVersionMajor, uint32_t nVersionMinor, uint32_t nVersionBuild) = 0; // 50
};

// Fragt das neue Interface vom bereits initialisierten OpenVR-Runtime an. nullptr = zu altes SteamVR.
inline IVRSystem_026* get_system_026(vr::EVRInitError* out_error = nullptr) {
    vr::EVRInitError err = vr::VRInitError_None;
    auto* p = (IVRSystem_026*)vr::VR_GetGenericInterface(IVRSystem_026_Version, &err);

    if (out_error != nullptr) {
        *out_error = err;
    }

    return err == vr::VRInitError_None ? p : nullptr;
}

} // namespace vr_ext
