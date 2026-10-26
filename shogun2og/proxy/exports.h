// The system dinput8.dll's exports with their ordinals (SysWOW64, Windows 10.0.22631); build.sh generates dinput8.def from
// this list.
#define DINPUT8_EXPORTS(STUB) \
    STUB(DirectInput8Create, 1) \
    STUB(DllCanUnloadNow, 2) \
    STUB(DllGetClassObject, 3) \
    STUB(DllRegisterServer, 4) \
    STUB(DllUnregisterServer, 5) \
    STUB(GetdfDIJoystick, 6)
