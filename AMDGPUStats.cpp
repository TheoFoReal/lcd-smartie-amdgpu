#include <windows.h>
#include <process.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DLL_EXPORT extern "C" __declspec(dllexport)

// ---------------------------------------------------------------------------
// ADL SDK structures and typedefs (minimal subset needed for this plugin)
// ---------------------------------------------------------------------------

#define ADL_OK 0
#define ADL_ERR_NOT_SUPPORTED -8

typedef void* (__stdcall *ADL_MAIN_MALLOC_CALLBACK)(int);
typedef void* ADL_CONTEXT_HANDLE;

typedef struct {
    int iAdapterIndex;
    char strAdapterName[256];
    char strDisplayName[256];
} AdapterInfo;

typedef struct {
    int iSize;
    int iTemperature; // in millidegrees Celsius
} ADLTemperature;

typedef struct {
    int iSize;
    int iEngineClock;      // in 10 kHz units
    int iMemoryClock;      // in 10 kHz units
    int iVddc;             // in mV (core voltage)
    int iActivityPercent;  // 0-100
} ADLPMActivity;

typedef struct {
    int iSize;
    int iSpeedType;
    int iFanSpeed;
    int iFlags;
} ADLFanSpeedValue;

#define ADL_DL_FANCTRL_SPEED_TYPE_RPM 1

// ---------------------------------------------------------------------------
// ADL function pointer typedefs
// ---------------------------------------------------------------------------

typedef int (*ADL_MAIN_CONTROL_CREATE)(ADL_MAIN_MALLOC_CALLBACK, int);
typedef int (*ADL_MAIN_CONTROL_DESTROY)();
typedef int (*ADL_ADAPTER_NUMBEROFADAPTERS_GET)(int*);
typedef int (*ADL_ADAPTER_ADAPTERINFO_GET)(AdapterInfo*, int);
typedef int (*ADL_OVERDRIVE5_TEMPERATURE_GET)(int, int, ADLTemperature*);
typedef int (*ADL_OVERDRIVE5_CURRENTACTIVITY_GET)(int, ADLPMActivity*);
typedef int (*ADL_OVERDRIVE5_FANSPEED_GET)(int, int, ADLFanSpeedValue*);

// Overdrive6 / Wattman function pointer typedefs
typedef int (*ADL2_MAIN_CONTROL_CREATE)(ADL_MAIN_MALLOC_CALLBACK, int, ADL_CONTEXT_HANDLE*);
typedef int (*ADL2_OVERDRIVE6_CURRENTPOWER_GET)(ADL_CONTEXT_HANDLE, int, int, int*);
typedef int (*ADL2_OVERDRIVE6_CAPABILITIES_GET)(ADL_CONTEXT_HANDLE, int, void*);

// VRAM usage function pointer typedef
typedef int (*ADL2_ADAPTER_DEDICATEDVRAMUSAGE_GET)(ADL_CONTEXT_HANDLE, int, int*);

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static HINSTANCE hADL = NULL;
static int gAdapterIndex = -1;
static volatile bool gInitStarted = false;
static volatile bool gInitComplete = false;
static volatile bool gInitSuccess = false;

static ADL_MAIN_CONTROL_CREATE              ADL_Main_Control_Create = NULL;
static ADL_MAIN_CONTROL_DESTROY             ADL_Main_Control_Destroy = NULL;
static ADL_ADAPTER_NUMBEROFADAPTERS_GET     ADL_Adapter_NumberOfAdapters_Get = NULL;
static ADL_ADAPTER_ADAPTERINFO_GET          ADL_Adapter_AdapterInfo_Get = NULL;
static ADL_OVERDRIVE5_TEMPERATURE_GET       ADL_Overdrive5_Temperature_Get = NULL;
static ADL_OVERDRIVE5_CURRENTACTIVITY_GET   ADL_Overdrive5_CurrentActivity_Get = NULL;
static ADL_OVERDRIVE5_FANSPEED_GET          ADL_Overdrive5_FanSpeed_Get = NULL;

// Overdrive6 / Wattman function pointers
static ADL2_MAIN_CONTROL_CREATE             ADL2_Main_Control_Create = NULL;
static ADL2_OVERDRIVE6_CURRENTPOWER_GET     ADL2_Overdrive6_CurrentPower_Get = NULL;
static ADL2_OVERDRIVE6_CAPABILITIES_GET     ADL2_Overdrive6_Capabilities_Get = NULL;

// VRAM usage function pointer
static ADL2_ADAPTER_DEDICATEDVRAMUSAGE_GET  ADL2_Adapter_DedicatedVRAMUsage_Get = NULL;

static ADL_CONTEXT_HANDLE g_adlContext = NULL;
static bool g_overdrive6Supported = false;

// ---------------------------------------------------------------------------
// Cached GPU values (written by the background thread, read by plugin functions)
// ---------------------------------------------------------------------------

static volatile bool gWorkerRunning = false;
static volatile bool gHaveNewData = false;

static char gCachedTemp[32]      = "N/A";
static char gCachedLoad[32]      = "N/A";
static char gCachedFan[32]       = "N/A";
static char gCachedPower[32]     = "N/A";
static char gCachedCoreClock[32] = "N/A";
static char gCachedMemClock[32]  = "N/A";
static char gCachedVRAM[32]      = "N/A";
static char gCachedVoltage[32]   = "N/A";
static char gCachedGpuCount[32]  = "N/A";

// Named result buffers (one per function).
static char resultBufferTemp[128];
static char resultBufferLoad[128];
static char resultBufferFan[128];
static char resultBufferPower[128];
static char resultBufferCoreClock[128];
static char resultBufferMemClock[128];
static char resultBufferVRAM[128];
static char resultBufferVoltage[128];
static char resultBufferGpuCount[128];

// ---------------------------------------------------------------------------
// Helper: allocate memory for ADL
// ---------------------------------------------------------------------------

static void* __stdcall ADL_Main_Memory_Alloc(int iSize) {
    return malloc(iSize);
}

// ---------------------------------------------------------------------------
// Worker thread: initializes ADL and polls GPU stats in the background.
// ---------------------------------------------------------------------------

static unsigned int __stdcall WorkerThread(void* param) {
    gWorkerRunning = true;

    // --- Load ADL library ---
    hADL = LoadLibraryA("atiadlxx.dll");
    if (hADL == NULL) {
        hADL = LoadLibraryA("atiadlxy.dll");
    }
    if (hADL == NULL) {
        gInitComplete = true;
        gInitSuccess = false;
        gWorkerRunning = false;
        return 0;
    }

    // --- Resolve Overdrive5 function pointers ---
    ADL_Main_Control_Create =
        (ADL_MAIN_CONTROL_CREATE)GetProcAddress(hADL, "ADL_Main_Control_Create");
    ADL_Main_Control_Destroy =
        (ADL_MAIN_CONTROL_DESTROY)GetProcAddress(hADL, "ADL_Main_Control_Destroy");
    ADL_Adapter_NumberOfAdapters_Get =
        (ADL_ADAPTER_NUMBEROFADAPTERS_GET)GetProcAddress(hADL, "ADL_Adapter_NumberOfAdapters_Get");
    ADL_Adapter_AdapterInfo_Get =
        (ADL_ADAPTER_ADAPTERINFO_GET)GetProcAddress(hADL, "ADL_Adapter_AdapterInfo_Get");
    ADL_Overdrive5_Temperature_Get =
        (ADL_OVERDRIVE5_TEMPERATURE_GET)GetProcAddress(hADL, "ADL_Overdrive5_Temperature_Get");
    ADL_Overdrive5_CurrentActivity_Get =
        (ADL_OVERDRIVE5_CURRENTACTIVITY_GET)GetProcAddress(hADL, "ADL_Overdrive5_CurrentActivity_Get");
    ADL_Overdrive5_FanSpeed_Get =
        (ADL_OVERDRIVE5_FANSPEED_GET)GetProcAddress(hADL, "ADL_Overdrive5_FanSpeed_Get");

    // --- Resolve Overdrive6 / Wattman function pointers ---
    ADL2_Main_Control_Create =
        (ADL2_MAIN_CONTROL_CREATE)GetProcAddress(hADL, "ADL2_Main_Control_Create");
    ADL2_Overdrive6_CurrentPower_Get =
        (ADL2_OVERDRIVE6_CURRENTPOWER_GET)GetProcAddress(hADL, "ADL2_Overdrive6_CurrentPower_Get");
    ADL2_Overdrive6_Capabilities_Get =
        (ADL2_OVERDRIVE6_CAPABILITIES_GET)GetProcAddress(hADL, "ADL2_Overdrive6_Capabilities_Get");

    // --- Resolve VRAM usage function pointer ---
    ADL2_Adapter_DedicatedVRAMUsage_Get =
        (ADL2_ADAPTER_DEDICATEDVRAMUSAGE_GET)GetProcAddress(hADL, "ADL2_Adapter_DedicatedVRAMUsage_Get");

    if (ADL_Main_Control_Create == NULL) {
        gInitComplete = true;
        gInitSuccess = false;
        gWorkerRunning = false;
        return 0;
    }

    // --- Create Overdrive5 context ---
    if (ADL_Main_Control_Create(ADL_Main_Memory_Alloc, 1) != ADL_OK) {
        gInitComplete = true;
        gInitSuccess = false;
        gWorkerRunning = false;
        return 0;
    }

    // --- Create Overdrive6 context (separate from OD5) ---
    if (ADL2_Main_Control_Create != NULL) {
        if (ADL2_Main_Control_Create(ADL_Main_Memory_Alloc, 1, &g_adlContext) == ADL_OK) {
            if (ADL2_Overdrive6_Capabilities_Get != NULL) {
                char capsBuffer[256];
                memset(capsBuffer, 0, sizeof(capsBuffer));
                if (ADL2_Overdrive6_Capabilities_Get(g_adlContext, 0, capsBuffer) == ADL_OK) {
                    g_overdrive6Supported = true;
                }
            }
        }
    }

    // --- Find the first AMD adapter ---
    int numAdapters = 0;
    if (ADL_Adapter_NumberOfAdapters_Get(&numAdapters) != ADL_OK || numAdapters == 0) {
        gInitComplete = true;
        gInitSuccess = false;
        gWorkerRunning = false;
        return 0;
    }

    int bufferSize = numAdapters * 2048;
    AdapterInfo* adapterInfo = (AdapterInfo*)malloc(bufferSize);
    if (adapterInfo == NULL) {
        gInitComplete = true;
        gInitSuccess = false;
        gWorkerRunning = false;
        return 0;
    }

    if (ADL_Adapter_AdapterInfo_Get(adapterInfo, bufferSize) != ADL_OK) {
        free(adapterInfo);
        gInitComplete = true;
        gInitSuccess = false;
        gWorkerRunning = false;
        return 0;
    }

    gAdapterIndex = adapterInfo[0].iAdapterIndex;
    free(adapterInfo);

    sprintf(gCachedGpuCount, "%d", numAdapters);

    gInitSuccess = true;
    gInitComplete = true;

    // --- Poll loop: query stats every 1000ms ---
    while (gWorkerRunning) {
        // --- Temperature ---
        ADLTemperature temp;
        temp.iSize = sizeof(ADLTemperature);
        temp.iTemperature = 0;
        if (ADL_Overdrive5_Temperature_Get != NULL &&
            ADL_Overdrive5_Temperature_Get(gAdapterIndex, 0, &temp) == ADL_OK) {
            sprintf(gCachedTemp, "%.1f", temp.iTemperature / 1000.0);
        } else {
            strcpy(gCachedTemp, "N/A");
        }

        // --- Load, Core Clock, Memory Clock, Voltage ---
        ADLPMActivity activity;
        activity.iSize = sizeof(ADLPMActivity);
        memset(&activity, 0, sizeof(activity));
        if (ADL_Overdrive5_CurrentActivity_Get != NULL &&
            ADL_Overdrive5_CurrentActivity_Get(gAdapterIndex, &activity) == ADL_OK) {
            sprintf(gCachedLoad, "%d", activity.iActivityPercent);
            sprintf(gCachedCoreClock, "%d", activity.iEngineClock / 100);
            sprintf(gCachedMemClock, "%d", activity.iMemoryClock / 100);
            if (activity.iVddc > 0) {
                sprintf(gCachedVoltage, "%.3f", activity.iVddc / 1000.0);
            } else {
                strcpy(gCachedVoltage, "N/A");
            }
        } else {
            strcpy(gCachedLoad, "N/A");
            strcpy(gCachedCoreClock, "N/A");
            strcpy(gCachedMemClock, "N/A");
            strcpy(gCachedVoltage, "N/A");
        }

        // --- Fan Speed ---
        ADLFanSpeedValue fan;
        fan.iSize = sizeof(ADLFanSpeedValue);
        fan.iSpeedType = ADL_DL_FANCTRL_SPEED_TYPE_RPM;
        fan.iFanSpeed = 0;
        fan.iFlags = 0;
        if (ADL_Overdrive5_FanSpeed_Get != NULL &&
            ADL_Overdrive5_FanSpeed_Get(gAdapterIndex, 0, &fan) == ADL_OK) {
            sprintf(gCachedFan, "%d", fan.iFanSpeed);
        } else {
            strcpy(gCachedFan, "N/A");
        }

        // --- Power Consumption ---
        if (g_overdrive6Supported && ADL2_Overdrive6_CurrentPower_Get != NULL &&
            g_adlContext != NULL) {
            int power = 0;
            if (ADL2_Overdrive6_CurrentPower_Get(g_adlContext, gAdapterIndex, 1, &power) == ADL_OK) {
                sprintf(gCachedPower, "%d", power);
            } else {
                strcpy(gCachedPower, "N/A");
            }
        } else {
            strcpy(gCachedPower, "N/A");
        }

        // --- Dedicated VRAM Usage ---
        if (ADL2_Adapter_DedicatedVRAMUsage_Get != NULL && g_adlContext != NULL) {
            int vramUsageMB = 0;
            if (ADL2_Adapter_DedicatedVRAMUsage_Get(g_adlContext, gAdapterIndex, &vramUsageMB) == ADL_OK) {
                sprintf(gCachedVRAM, "%d", vramUsageMB);
            } else {
                strcpy(gCachedVRAM, "N/A");
            }
        } else {
            strcpy(gCachedVRAM, "N/A");
        }

        gHaveNewData = true;
        Sleep(1000);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Kick off the worker thread if not already started.
// ---------------------------------------------------------------------------

static void EnsureWorkerStarted() {
    if (!gInitStarted) {
        gInitStarted = true;
        _beginthreadex(NULL, 0, WorkerThread, NULL, 0, NULL);
    }
}

// ---------------------------------------------------------------------------
// Function 1: GPU Temperature (°C, one decimal)
// Usage: $dll(AMDGPUStats.dll,1,,)
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function1(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBufferTemp, "N/A");
    } else {
        strcpy(resultBufferTemp, gCachedTemp);
    }
    return resultBufferTemp;
}

// ---------------------------------------------------------------------------
// Function 2: GPU Load (%, 0-100)
// Usage: $dll(AMDGPUStats.dll,2,,)
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function2(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBufferLoad, "N/A");
    } else {
        strcpy(resultBufferLoad, gCachedLoad);
    }
    return resultBufferLoad;
}

// ---------------------------------------------------------------------------
// Function 3: Fan Speed (RPM)
// Usage: $dll(AMDGPUStats.dll,3,,)
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function3(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBufferFan, "N/A");
    } else {
        strcpy(resultBufferFan, gCachedFan);
    }
    return resultBufferFan;
}

// ---------------------------------------------------------------------------
// Function 4: GPU Power Consumption (watts)
// Usage: $dll(AMDGPUStats.dll,4,,)
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function4(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBufferPower, "N/A");
    } else {
        strcpy(resultBufferPower, gCachedPower);
    }
    return resultBufferPower;
}

// ---------------------------------------------------------------------------
// Function 5: Core Clock (MHz)
// Usage: $dll(AMDGPUStats.dll,5,,)
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function5(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBufferCoreClock, "N/A");
    } else {
        strcpy(resultBufferCoreClock, gCachedCoreClock);
    }
    return resultBufferCoreClock;
}

// ---------------------------------------------------------------------------
// Function 6: Memory Clock (MHz)
// Usage: $dll(AMDGPUStats.dll,6,,)
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function6(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBufferMemClock, "N/A");
    } else {
        strcpy(resultBufferMemClock, gCachedMemClock);
    }
    return resultBufferMemClock;
}

// ---------------------------------------------------------------------------
// Function 7: Dedicated VRAM Usage (MB)
// Usage: $dll(AMDGPUStats.dll,7,,)
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function7(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBufferVRAM, "N/A");
    } else {
        strcpy(resultBufferVRAM, gCachedVRAM);
    }
    return resultBufferVRAM;
}

// ---------------------------------------------------------------------------
// Function 8: GPU Core Voltage (volts)
// Usage: $dll(AMDGPUStats.dll,8,,)
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function8(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBufferVoltage, "N/A");
    } else {
        strcpy(resultBufferVoltage, gCachedVoltage);
    }
    return resultBufferVoltage;
}

// ---------------------------------------------------------------------------
// Function 9: Number of AMD GPUs
// Usage: $dll(AMDGPUStats.dll,9,,)
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function9(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBufferGpuCount, "0");
    } else {
        strcpy(resultBufferGpuCount, gCachedGpuCount);
    }
    return resultBufferGpuCount;
}

// ---------------------------------------------------------------------------
// LCD Smartie entry points
// ---------------------------------------------------------------------------

DLL_EXPORT void __stdcall SmartieInit() {
    gInitStarted = false;
    gInitComplete = false;
    gInitSuccess = false;
    gAdapterIndex = -1;
    hADL = NULL;
    gWorkerRunning = false;
    gHaveNewData = false;
    g_adlContext = NULL;
    g_overdrive6Supported = false;
}

DLL_EXPORT void __stdcall SmartieFini() {
    gWorkerRunning = false;
    Sleep(100);

    if (gInitSuccess && ADL_Main_Control_Destroy != NULL) {
        ADL_Main_Control_Destroy();
    }
    if (g_adlContext != NULL) {
        g_adlContext = NULL;
    }
    if (hADL != NULL) {
        FreeLibrary(hADL);
        hADL = NULL;
    }
    gInitStarted = false;
    gInitComplete = false;
    gInitSuccess = false;
}
