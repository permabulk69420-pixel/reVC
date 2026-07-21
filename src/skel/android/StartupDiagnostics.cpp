#if defined(ANDROID)

#include <android/log.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "skeleton.h"
#include "platform.h"

namespace {

const char* EventName(RsEvent event) {
    switch (event) {
    case rsINITIALIZE: return "rsINITIALIZE";
    case rsRWINITIALIZE: return "rsRWINITIALIZE";
    case rsCAMERASIZE: return "rsCAMERASIZE";
    case rsRWTERMINATE: return "rsRWTERMINATE";
    case rsTERMINATE: return "rsTERMINATE";
    default: return NULL;
    }
}

uint64_t MonotonicMilliseconds() {
    timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<uint64_t>(value.tv_sec) * 1000ULL +
           static_cast<uint64_t>(value.tv_nsec) / 1000000ULL;
}

void DiagnosticLog(const char* format, ...) {
    char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char line[2304];
    snprintf(line, sizeof(line),
             "DIAG t=%llu tid=%d %s",
             static_cast<unsigned long long>(MonotonicMilliseconds()),
             static_cast<int>(syscall(SYS_gettid)), message);
    __android_log_write(ANDROID_LOG_INFO, "reVC-XR", line);

    const char* root = getenv("STORAGE_ROOT");
    if (root == NULL || root[0] == '\0')
        return;

    std::string gameRoot(root);
    while (!gameRoot.empty() && gameRoot[gameRoot.size() - 1] == '/')
        gameRoot.erase(gameRoot.size() - 1);

    const std::string userFiles = gameRoot + "/userfiles";
    if (mkdir(userFiles.c_str(), 0775) != 0 && errno != EEXIST)
        return;

    FILE* file = fopen((userFiles + "/xr_log.txt").c_str(), "a");
    if (file == NULL)
        return;
    fprintf(file, "%s\n", line);
    fflush(file);
    fclose(file);
}

} // namespace

extern "C" RsEventStatus __real_RsEventHandler(RsEvent event, void* param);
extern "C" RwBool __real_psInitialize(void);
extern "C" RwBool __real_psSelectDevice(void);

extern "C" RsEventStatus __wrap_RsEventHandler(RsEvent event, void* param) {
    const char* name = EventName(event);
    if (name != NULL)
        DiagnosticLog("%s BEGIN event=%d param=%p", name,
                      static_cast<int>(event), param);

    const RsEventStatus result = __real_RsEventHandler(event, param);

    if (name != NULL)
        DiagnosticLog("%s END result=%d", name, static_cast<int>(result));
    return result;
}

extern "C" RwBool __wrap_psInitialize(void) {
    DiagnosticLog("psInitialize BEGIN");
    const RwBool result = __real_psInitialize();
    DiagnosticLog("psInitialize END result=%d", static_cast<int>(result));
    return result;
}

extern "C" RwBool __wrap_psSelectDevice(void) {
    DiagnosticLog("psSelectDevice BEGIN");
    const RwBool result = __real_psSelectDevice();
    DiagnosticLog("psSelectDevice END result=%d", static_cast<int>(result));
    return result;
}

#endif
