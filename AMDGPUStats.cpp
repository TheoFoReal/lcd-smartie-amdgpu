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

// ADL adapter info structure (partial).
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
    int iVddc;             // in mV
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

// ---------------------------------------------------------------------------
// Cached GPU values (written by the background thread, read by plugin functions)
// ---------------------------------------------------------------------------

static volatile bool gWorkerRunning = false;
static volatile bool gHaveNewData = false;

static char gCachedTemp[32] = "N/A";
static char gCachedLoad[32] = "N/A";
static char gCachedFan[32] = "N/A";
static char gCachedCoreClock[32] = "N/A";
static char gCachedMemClock[32] = "N/A";
static char gCachedGpuCount[32] = "N/A";

// Per-function result buffers.
static char resultBuffer1[128];
static char resultBuffer2[128];
static char resultBuffer3[128];
static char resultBuffer4[128];
static char resultBuffer5[128];
static char resultBuffer6[128];

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

    // --- Initialize ADL ---
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

    if (ADL_Main_Control_Create == NULL) {
        gInitComplete = true;
        gInitSuccess = false;
        gWorkerRunning = false;
        return 0;
    }

    if (ADL_Main_Control_Create(ADL_Main_Memory_Alloc, 1) != ADL_OK) {
        gInitComplete = true;
        gInitSuccess = false;
        gWorkerRunning = false;
        return 0;
    }

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

    // Cache the GPU count once.
    sprintf(gCachedGpuCount, "%d", numAdapters);

    gInitSuccess = true;
    gInitComplete = true;

    // --- Poll loop: query stats every 1000ms ---
    while (gWorkerRunning) {
        ADLTemperature temp;
        temp.iSize = sizeof(ADLTemperature);
        temp.iTemperature = 0;
        if (ADL_Overdrive5_Temperature_Get != NULL &&
            ADL_Overdrive5_Temperature_Get(gAdapterIndex, 0, &temp) == ADL_OK) {
            sprintf(gCachedTemp, "%.1f", temp.iTemperature / 1000.0);
        } else {
            strcpy(gCachedTemp, "N/A");
        }

        ADLPMActivity activity;
        activity.iSize = sizeof(ADLPMActivity);
        memset(&activity, 0, sizeof(activity));
        if (ADL_Overdrive5_CurrentActivity_Get != NULL &&
            ADL_Overdrive5_CurrentActivity_Get(gAdapterIndex, &activity) == ADL_OK) {
            sprintf(gCachedLoad, "%d", activity.iActivityPercent);
            sprintf(gCachedCoreClock, "%d", activity.iEngineClock / 100);
            sprintf(gCachedMemClock, "%d", activity.iMemoryClock / 100);
        } else {
            strcpy(gCachedLoad, "N/A");
            strcpy(gCachedCoreClock, "N/A");
            strcpy(gCachedMemClock, "N/A");
        }

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

        gHaveNewData = true;

        // Sleep for 1000ms (1 second) before the next poll.
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
// Function 1: GPU Temperature
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function1(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBuffer1, "N/A");
    } else {
        strcpy(resultBuffer1, gCachedTemp);
    }
    return resultBuffer1;
}

// ---------------------------------------------------------------------------
// Function 2: GPU Load
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function2(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBuffer2, "N/A");
    } else {
        strcpy(resultBuffer2, gCachedLoad);
    }
    return resultBuffer2;
}

// ---------------------------------------------------------------------------
// Function 3: Fan Speed
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function3(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBuffer3, "N/A");
    } else {
        strcpy(resultBuffer3, gCachedFan);
    }
    return resultBuffer3;
}

// ---------------------------------------------------------------------------
// Function 4: Core Clock
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function4(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBuffer4, "N/A");
    } else {
        strcpy(resultBuffer4, gCachedCoreClock);
    }
    return resultBuffer4;
}

// ---------------------------------------------------------------------------
// Function 5: Memory Clock
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function5(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBuffer5, "N/A");
    } else {
        strcpy(resultBuffer5, gCachedMemClock);
    }
    return resultBuffer5;
}

// ---------------------------------------------------------------------------
// Function 6: Number of AMD GPUs
// ---------------------------------------------------------------------------

DLL_EXPORT char* __stdcall function6(char* param1, char* param2) {
    EnsureWorkerStarted();
    if (gInitComplete && !gInitSuccess) {
        strcpy(resultBuffer6, "0");
    } else {
        strcpy(resultBuffer6, gCachedGpuCount);
    }
    return resultBuffer6;
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
}

DLL_EXPORT void __stdcall SmartieFini() {
    gWorkerRunning = false;
    Sleep(100); // give the worker thread a moment to exit

    if (gInitSuccess && ADL_Main_Control_Destroy != NULL) {
        ADL_Main_Control_Destroy();
    }
    if (hADL != NULL) {
        FreeLibrary(hADL);
        hADL = NULL;
    }
    gInitStarted = false;
    gInitComplete = false;
    gInitSuccess = false;
}
