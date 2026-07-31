#include <switch.h>
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cerrno>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "nlohmann/json.hpp"


#define CONFIG_PATH "/config/PhalkProfiles/config.txt"
#define EXPORT_PATH "/switch/PhalkProfiles/export.json"

#ifndef PHALK_SYSMODULE
#define PHALK_SYSMODULE_TID "00FF0000D4A9E73C"
#define SYS_CONTENT_DIR "/atmosphere/contents/" PHALK_SYSMODULE_TID
#define SYS_EXEFS_PATH SYS_CONTENT_DIR "/exefs.nsp"
#define SYS_TOOLBOX_PATH SYS_CONTENT_DIR "/toolbox.json"
#define SYS_BOOT2_FLAG SYS_CONTENT_DIR "/flags/boot2.flag"
#define SYSMODULE_SOURCE_PATH "romfs:/sysmodule/sysmodule.nsp"
#define TOOLBOX_SOURCE_PATH "romfs:/sysmodule/toolbox.json"
#endif


using json = nlohmann::json;

// =============================================================================
// BUILD MODES
// =============================================================================
// This file intentionally produces two different binaries from the same source:
//
//   make          -> PhalkProfiles.NRO (user interface)
//   make sysmodule -> sysmodule.nsp (background uploader)
//
// Everything before the APP section is shared by both builds. The two runtime
// entry points are separated near the end with PHALK_SYSMODULE.

#ifdef PHALK_SYSMODULE
static void sysLog(const char* format, ...);
#endif


// =============================================================================
// SHARED CODE: play-data export, HTTP upload and configuration
// =============================================================================

namespace PlayData
{

json exportPlayLog()
{
    json result;

    // 1. Global metadata.
    result["exportString"] = "Exported using PhalkProfiles";
    result["exportTimestamp"] = (u64)time(NULL);
    result["users"] = json::array();

    // Initialize system services.
    accountInitialize(AccountServiceType_Administrator);
    pdmqryInitialize();
    nsInitialize(); // Required to resolve application names.

    s32 userCount = 0;
    AccountUid userList[ACC_USER_LIST_SIZE]; 
    
    if (R_SUCCEEDED(accountListAllUsers(userList, ACC_USER_LIST_SIZE, &userCount))) 
    {
        // Iterate over every Switch user.
        for (s32 i = 0; i < userCount; i++) 
        {
            AccountUid userId = userList[i];
            AccountProfile prof;
            AccountProfileBase profBase;
            AccountUserData userData;
            char username[33] = {0}; 

            if (R_SUCCEEDED(accountGetProfile(&prof, userId))) {
                if (R_SUCCEEDED(accountProfileGet(&prof, &userData, &profBase))) {
                    snprintf(username, sizeof(username), "%s", profBase.nickname);
                }
                accountProfileClose(&prof);
            }

            json uJson;
            uJson["name"] = std::string(username);
            
            char userIdStr[40];
            snprintf(userIdStr, sizeof(userIdStr), "%016lx%016lx", (unsigned long)userId.uid[0], (unsigned long)userId.uid[1]);
            uJson["id"] = userIdStr;
            uJson["titles"] = json::array();

            // Get the valid global PDM event range for a fast title-ID scan.
            s32 totalEntries = 0;
            s32 startEntryIndex = 0;
            s32 endEntryIndex = 0;
            pdmqryGetAvailablePlayEventRange(&totalEntries, &startEntryIndex, &endEntryIndex);

            if (totalEntries > 0) 
            {
                std::vector<u64> foundTitles;

                const s32 batchSize = 100;
                std::vector<PdmPlayEvent> rawEvents(batchSize);
                s32 currentOffset = startEntryIndex;
                s32 remaining = totalEntries;

                // Collect only title IDs that have play history.
                while (remaining > 0) 
                {
                    s32 read = 0;
                    if (R_FAILED(pdmqryQueryPlayEvent(currentOffset, rawEvents.data(), batchSize, &read)) || read <= 0)
                        break;

                    for (s32 k = 0; k < read; k++) 
                    {
                        auto &ev = rawEvents[k];
                        if (ev.play_event_type == PdmPlayEventType_Applet) 
                        {
                            u64 evTitleId = ev.event_data.applet.program_id[0];
                            evTitleId = (evTitleId << 32) | ev.event_data.applet.program_id[1];
                            
                            if (std::find(foundTitles.begin(), foundTitles.end(), evTitleId) == foundTitles.end()) {
                                foundTitles.push_back(evTitleId);
                            }
                        }
                    }
                    currentOffset += read;
                    remaining -= read;
                }

                // Build a clean structure containing only name, ID and summary.
                for (u64 titleId : foundTitles) 
                {
                    PdmPlayStatistics tmpStats;
                    Result rcStats = pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(titleId, userId, false, &tmpStats);

                    // Process only titles that have actually been launched.
                    if (R_SUCCEEDED(rcStats) && tmpStats.total_launches > 0) 
                    {
                        json tJson;
                        char titleIdStr[20];
                        snprintf(titleIdStr, sizeof(titleIdStr), "%016lx", (unsigned long)titleId);
                        
                        tJson["id"] = titleIdStr;
                         // Resolve the application name from its control data.
                        std::string gameName = "Unknown Game (" + std::string(titleIdStr) + ")";
                        
                        NsApplicationControlData* controlData = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
                        if (controlData != NULL) 
                        {
                            u64 outSize = 0;
                            Result rcControl = nsGetApplicationControlData(NsApplicationControlSource_Storage, titleId, controlData, sizeof(NsApplicationControlData), &outSize);
                            
                            if (R_SUCCEEDED(rcControl)) 
                            {
                                u64 systemLanguage = 0;
                                NacpLanguageEntry* langEntry = NULL;

                                if (R_SUCCEEDED(setGetSystemLanguage(&systemLanguage))) 
                                {
                                    // Manual language mapping based on Horizon OS and libnx.
                                    s32 nacpLanguageIndex = -1;
                                    u64 langCodes[] = {
                                        0x000000000000616a, // ja    (0)
                                        0x53552d6e65000000, // en-US (1)
                                        0x0000000000007266, // fr    (2)
                                        0x0000000000006564, // de    (3)
                                        0x0000000000007469, // it    (4)
                                        0x0000000000007365, // es    (5)
                                        0x42472d6e65000000, // en-GB (6)
                                        0x0000000000006c6e, // nl    (7)
                                        0x0000000000007572, // ru    (8)
                                        0x53552d7365000000, // es-419(9)
                                        0x41432d7266000000, // fr-CA (10)
                                        0x0000000000006f6b, // ko    (11)
                                        0x4e432d687a000000, // zh-CN (12)
                                        0x57542d687a000000, // zh-TW (13)
                                        0x0000000000007074, // pt    (14)
                                        0x52422d7470000000  // pt-BR (15)
                                    };

                                    for (int l = 0; l < 16; l++) {
                                        if (systemLanguage == langCodes[l]) {
                                            nacpLanguageIndex = l;
                                            break;
                                        }
                                    }

                                    // Validate the system language against the title's language table.
                                    if (nacpLanguageIndex >= 0 && nacpLanguageIndex < 16) {
                                        if (controlData->nacp.lang[nacpLanguageIndex].name[0] != '\0') {
                                            langEntry = &controlData->nacp.lang[nacpLanguageIndex];
                                        }
                                    }
                                }

                                // Fallback 1: use US English when the system language is unavailable.
                                if (langEntry == NULL && controlData->nacp.lang[1].name[0] != '\0') {
                                    langEntry = &controlData->nacp.lang[1];
                                }

                                // Fallback 2: use the first populated language entry.
                                if (langEntry == NULL) {
                                    for (int lang = 0; lang < 16; lang++) {
                                        if (controlData->nacp.lang[lang].name[0] != '\0') {
                                            langEntry = &controlData->nacp.lang[lang];
                                            break;
                                        }
                                    }
                                }

                                // Use the first valid localized name.
                                if (langEntry != NULL && langEntry->name[0] != '\0') {
                                    gameName = std::string(langEntry->name);
                                }
                            }
                            free(controlData);
                        }
                        
                        tJson["name"] = gameName;
                        // ----------------------------------------

                        tJson["summary"]["firstPlayed"] = tmpStats.first_timestamp_user;
                        tJson["summary"]["lastPlayed"] = tmpStats.last_timestamp_user;
                        tJson["summary"]["playtime"] = tmpStats.playtime / 1000 / 1000 / 1000; // Seconds.
                        tJson["summary"]["launches"] = tmpStats.total_launches;

                        uJson["titles"].push_back(tJson);
                    }
                }
            }

            result["users"].push_back(uJson);
        }
    }

    nsExit();
    pdmqryExit();
    accountExit();

    return result;
}

}



// -----------------------------------------------------------------------------
// Raw HTTP multipart upload
// -----------------------------------------------------------------------------

namespace Http
{


// Raw socket multipart upload.
// Raw sockets keep the uploader small and predictable on the Switch.
bool uploadFileRaw(
    const std::string& filePath,
    const std::string& username,
    const std::string& password
)
{
    const char* boundary = "----PhalkProfilesBoundary7MA4YWxkTrZu0gW";
    const char* serverIp = "147.79.94.55";

    struct stat fileStat;
    if (stat(filePath.c_str(), &fileStat) != 0)
        return false;

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
        return false;

    std::string start = std::string("--") + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"user_name\"\r\n\r\n" + username + "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"password\"\r\n\r\n" + password + "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"json_file\"; filename=\"export.json\"\r\n"
        "Content-Type: application/json\r\n\r\n";
    const std::string end = std::string("\r\n--") + boundary + "--\r\n";
    const unsigned long long length = start.size() + (unsigned long long)fileStat.st_size + end.size();

    char header[1024];
    int headerLength = snprintf(header, sizeof(header),
        "POST /profiles/json_curl.php HTTP/1.0\r\n"
        "Host: phalk.net\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %llu\r\n"
        "Connection: close\r\n\r\n", boundary, length);
    if (headerLength <= 0 || headerLength >= (int)sizeof(header))
        return false;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return false;

    timeval timeout = {15, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(80);
    address.sin_addr.s_addr = inet_addr(serverIp);
    if (connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(sock);
        return false;
    }

    auto sendAll = [sock](const void* data, size_t size) -> bool {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            ssize_t sent = send(sock, p, size, 0);
            if (sent <= 0) return false;
            p += sent;
            size -= static_cast<size_t>(sent);
        }
        return true;
    };

    bool ok = sendAll(header, headerLength) && sendAll(start.data(), start.size());
    char buffer[4096];
    while (ok && file.good()) {
        file.read(buffer, sizeof(buffer));
        std::streamsize count = file.gcount();
        if (count > 0) ok = sendAll(buffer, static_cast<size_t>(count));
    }
    if (ok) ok = sendAll(end.data(), end.size());

    char response[2048] = {0};
    ssize_t received = ok ? recv(sock, response, sizeof(response) - 1, 0) : -1;
    close(sock);

#ifdef PHALK_SYSMODULE
    sysLog("raw HTTP sent=%s response_bytes=%ld status200=%s", ok ? "yes" : "no",
           (long)received, (received >= 12 && strstr(response, " 200 ")) ? "yes" : "no");
    if (received > 0)
        sysLog("raw HTTP response: %.240s", response);
    if (received > 0) {
        const char* body = strstr(response, "\r\n\r\n");
        if (body)
            sysLog("raw HTTP body: %.240s", body + 4);
    }
#endif
    return ok && received >= 12 && strstr(response, " 200 ") != nullptr;
}


}



// -----------------------------------------------------------------------------
// Configuration and shared export/upload pipeline
// -----------------------------------------------------------------------------


struct Config
{
    std::string username;
    std::string password;
};



Config loadConfig()
{
    Config cfg;


    std::ifstream file(CONFIG_PATH);


    if(file.is_open())
    {
        getline(file,cfg.username);
        getline(file,cfg.password);

        // Remove Windows CR characters and an optional UTF-8 BOM from the first line.
        if (!cfg.username.empty() &&
            static_cast<unsigned char>(cfg.username[0]) == 0xEF &&
            cfg.username.size() >= 3)
        {
            cfg.username.erase(0, 3);
        }

        if (!cfg.username.empty() && cfg.username.back() == '\r')
            cfg.username.pop_back();
        if (!cfg.password.empty() && cfg.password.back() == '\r')
            cfg.password.pop_back();

        file.close();
    }


    return cfg;
}



void saveConfig(
    const Config& cfg
)
{

    mkdir(
        "/config/PhalkProfiles",
        0777
    );


    std::ofstream file(
        CONFIG_PATH,
        std::ios::trunc
    );


    if(file.is_open())
    {
        file << cfg.username << "\n";
        file << cfg.password << "\n";

        file.close();
    }
}



bool exportPlayData()
{
    mkdir("/switch/PhalkProfiles", 0777);

    auto data = PlayData::exportPlayLog();
    std::ofstream file(EXPORT_PATH);

    if (!file.is_open())
        return false;

    file << data.dump(4);
    file.close();

    return true;
}



bool uploadExport(
    const Config& cfg
)
{
    return Http::uploadFileRaw(EXPORT_PATH, cfg.username, cfg.password);
}



bool exportAndUpload(
    const Config& cfg
)
{
    if (!exportPlayData())
        return false;

    return uploadExport(cfg);
}



#ifndef PHALK_SYSMODULE

// =============================================================================
// APP ONLY: PhalkProfiles.NRO
// =============================================================================
// This section owns the screen, keyboard, credentials and sysmodule installer.
// It is not compiled into the background sysmodule.

static bool fileExists(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool readableFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

static bool sameFile(const char* first, const char* second)
{
    std::ifstream a(first, std::ios::binary);
    std::ifstream b(second, std::ios::binary);
    if (!a || !b)
        return false;

    char abuf[4096], bbuf[4096];
    while (true) {
        a.read(abuf, sizeof(abuf));
        b.read(bbuf, sizeof(bbuf));
        const std::streamsize ac = a.gcount();
        const std::streamsize bc = b.gcount();
        if (ac != bc)
            return false;
        if (ac == 0)
            return true;
        if (!std::equal(abuf, abuf + ac, bbuf))
            return false;
    }
}

static bool copyFile(const char* source, const char* destination)
{
    std::ifstream in(source, std::ios::binary);
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!in || !out)
        return false;

    char buffer[4096];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const std::streamsize count = in.gcount();
        if (count > 0)
            out.write(buffer, count);
    }
    // Some devoptab-backed streams do not expose eof() consistently after the
    // final read. The destination stream is the authoritative result here.
    return out.good();
}

static bool prepareSysmoduleDirectory()
{
    // Only create the exact directories we own. Never remove or overwrite a
    // neighboring Atmosphere content directory.
    mkdir("/atmosphere", 0777);
    mkdir("/atmosphere/contents", 0777);
    mkdir(SYS_CONTENT_DIR, 0777);
    mkdir(SYS_CONTENT_DIR "/flags", 0777);
    return true;
}

static bool sysmoduleInstalled()
{
    return fileExists(SYS_EXEFS_PATH) && readableFile(SYSMODULE_SOURCE_PATH)
        && sameFile(SYSMODULE_SOURCE_PATH, SYS_EXEFS_PATH);
}

static bool installSysmodule()
{
    // RomFS files are accessed through libnx's devoptab. Use stream reads for
    // RomFS instead of stat(), which is not reliable for every devoptab mount.
    if (!readableFile(SYSMODULE_SOURCE_PATH) || !readableFile(TOOLBOX_SOURCE_PATH))
        return false;

    prepareSysmoduleDirectory();

    // A different binary in our chosen TID is treated as a collision. This
    // prevents the app from silently overwriting another sysmodule.
    if (fileExists(SYS_EXEFS_PATH) && !sameFile(SYSMODULE_SOURCE_PATH, SYS_EXEFS_PATH))
        return false;

    if (!fileExists(SYS_EXEFS_PATH) && !copyFile(SYSMODULE_SOURCE_PATH, SYS_EXEFS_PATH))
        return false;

    if (!copyFile(TOOLBOX_SOURCE_PATH, SYS_TOOLBOX_PATH))
        return false;

    return sysmoduleInstalled() && fileExists(SYS_TOOLBOX_PATH);
}

static bool sysmoduleEnabled()
{
    return fileExists(SYS_BOOT2_FLAG);
}

static bool setSysmoduleEnabled(bool enabled)
{
    if (enabled) {
        if (!installSysmodule())
            return false;
        std::ofstream flag(SYS_BOOT2_FLAG, std::ios::binary | std::ios::trunc);
        return flag.good();
    }

    // Removing only our exact flag disables boot-time loading. The binary is
    // intentionally kept, so a later enable is cheap and reversible.
    return std::remove(SYS_BOOT2_FLAG) == 0 || errno == ENOENT;
}

static bool removeInstalledSysmodule()
{
    if (sysmoduleEnabled() || !sysmoduleInstalled())
        return false;

    if (std::remove(SYS_EXEFS_PATH) != 0 && errno != ENOENT)
        return false;
    std::remove(SYS_CONTENT_DIR "/flags/boot2.flag");
    std::remove(SYS_CONTENT_DIR "/toolbox.json");
    rmdir(SYS_CONTENT_DIR "/flags");
    rmdir(SYS_CONTENT_DIR);
    return true;
}

// -----------------------------------------------------------------------------
// On-screen keyboard
// -----------------------------------------------------------------------------


std::string askText(
    const char* guide,
    bool password=false
)
{

    SwkbdConfig kbd;

    char buffer[513]={0};



    if(R_FAILED(
        swkbdCreate(&kbd,0)
    ))
        return "";



    swkbdConfigMakePresetDefault(
        &kbd
    );


    swkbdConfigSetGuideText(
        &kbd,
        guide
    );



    if(password)
    {
        swkbdConfigSetPasswordFlag(
            &kbd,
            true
        );
    }



    Result rc =
        swkbdShow(
            &kbd,
            buffer,
            sizeof(buffer)
        );


    swkbdClose(&kbd);



    if(R_FAILED(rc))
        return "";



    return std::string(buffer);
}



void drawMainScreen(
    const Config& cfg,
    const std::string& status,
    int selected,
    bool restartRequired
)
{
    const char* reset = "\x1b[0m";
    const char* green = "\x1b[32m";
    const char* red = "\x1b[31m";
    const char* yellow = "\x1b[33m";

    consoleClear();

    printf("+--------------------------------------+\n");
    printf("|            PHALK PROFILES            |\n");
    printf("+--------------------------------------+\n\n");

    const bool uploadFailed = status == "Upload failed...";
    const bool uploading = status == "Uploading";
    const char* statusColor = uploadFailed ? red : uploading ? yellow : green;
    printf("[ Status: %s%s%s ]\n\n", statusColor, status.c_str(), reset);
    printf("  User:              %s\n", cfg.username.empty() ? "(empty)" : cfg.username.c_str());
    printf("  Password:          %s\n\n", cfg.password.empty() ? "(empty)" : "******");

    const char* appItems[] = {
        "Export & upload",
        "Set username",
        "Set password"
    };
    for (int i = 0; i < 3; i++)
        printf("  %c %s\n", selected == i ? '>' : ' ', appItems[i]);

    const bool installed = sysmoduleInstalled();
    const char* sysmoduleStatus = !installed
        ? (restartRequired ? "SysModule: not installed (requires reboot)" : "SysModule: not installed")
        : (restartRequired ? "SysModule: installed (requires reboot)" : "SysModule: installed");
    printf("\n[ %s ]\n\n", sysmoduleStatus);

    const bool enabled = sysmoduleEnabled();
    const char* sysmoduleAction = !installed
        ? "Install & enable sysmodule"
        : enabled
            ? "Disable & remove sysmodule"
            : "Enable sysmodule";
    printf("  %c %s\n", selected == 3 ? '>' : ' ', sysmoduleAction);

    printf("\n  %c Quit\n", selected == 4 ? '>' : ' ');
    consoleUpdate(NULL);
}



// -----------------------------------------------------------------------------
// NRO entry point and user interface
// -----------------------------------------------------------------------------


int main()
{

    consoleInit(NULL);
    romfsInit();
    printf("console ok\n");
    consoleUpdate(NULL);
    socketInitializeDefault();
    printf("socket ok\n");
    consoleUpdate(NULL);
    nifmInitialize(NifmServiceType_User);
    printf("nifm ok\n");
    consoleUpdate(NULL);
    sslInitialize(1);
    printf("ssl ok\n");
    consoleUpdate(NULL);
    Config cfg =
        loadConfig();

    PadState pad;


    padConfigureInput(
        1,
        HidNpadStyleSet_NpadStandard
    );


    padInitializeDefault(
        &pad
    );



    std::string status =
        "Ready";

    int selected = 0;
    bool restartRequired = false;
    bool screenDirty = true;



    while(appletMainLoop())
    {

        padUpdate(&pad);


        u64 down =
            padGetButtonsDown(&pad);



        if(down & HidNpadButton_Plus)
            break;

        if(down & HidNpadButton_Up) {
            selected = (selected + 4) % 5;
            screenDirty = true;
        }

        if(down & HidNpadButton_Down) {
            selected = (selected + 1) % 5;
            screenDirty = true;
        }



        if(down & HidNpadButton_A && selected == 3)
        {
            if (!sysmoduleInstalled()) {
                if (installSysmodule() && setSysmoduleEnabled(true)) {
                    status = "Ready";
                    restartRequired = true;
                } else {
                    status = "Ready";
                }
            } else if (!sysmoduleEnabled()) {
                if (setSysmoduleEnabled(true)) {
                    status = "Ready";
                    restartRequired = true;
                } else {
                    status = "Ready";
                }
            } else if (setSysmoduleEnabled(false) && removeInstalledSysmodule()) {
                status = "Ready";
                restartRequired = true;
            } else {
                status = "Ready";
            }
            screenDirty = true;
        }

        if(down & HidNpadButton_A && selected == 1)
        {

            auto value =
                askText(
                    "Username"
                );


            if(!value.empty())
            {
                cfg.username=value;
                saveConfig(cfg);

                status="Ready";
                screenDirty = true;
            }

        }



        if(down & HidNpadButton_A && selected == 2)
        {

            auto value =
                askText(
                    "Password",
                    true
                );


            if(!value.empty())
            {
                cfg.password=value;
                saveConfig(cfg);

                status="Ready";
                screenDirty = true;
            }

        }




        if(down & HidNpadButton_A && selected == 4)
            break;



        if(down & HidNpadButton_A && selected == 0)
        {
            // Show immediate feedback before the blocking upload operation.
            status = "Uploading...";
            drawMainScreen(cfg, status, selected, restartRequired);

            status = exportAndUpload(cfg)
                ? "Success!"
                : "Upload failed...";
            screenDirty = true;

        }



        if (screenDirty) {
            drawMainScreen(cfg, status, selected, restartRequired);
            screenDirty = false;
        }

        svcSleepThread(16'000'000ULL);

    }



    nifmExit();

    sslExit();

    socketExit();



    consoleExit(NULL);
    romfsExit();


    return 0;
}

#else

// =============================================================================
// SYSMODULE ONLY: background uploader
// =============================================================================
// This section has no UI. It starts with Atmosphere, watches the application
// process through pgl, and uploads play data after the application exits.

static void sysLog(const char* format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    std::ofstream log("/switch/PhalkProfiles/sysmodule.log", std::ios::app);
    if (!log)
        return;

    log << "[" << (u64)time(NULL) << "] " << message << "\n";
    log.flush();
}

extern "C" {

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void)
{
    static u8 inner_heap[0x200000];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void)
{
    if (R_FAILED(smInitialize()))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    // Sysmodules do not get the firmware version initialized by an applet.
    // libnx uses this value to select the correct IPC command versions.
    Result setRc = setsysInitialize();
    if (R_SUCCEEDED(setRc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            sysLog("firmware detected: %u.%u.%u", fw.major, fw.minor, fw.micro);
        } else {
            sysLog("setsysGetFirmwareVersion failed");
        }
        setsysExit();
    } else {
        sysLog("setsysInitialize failed: 0x%08x", setRc);
    }

    if (R_FAILED(fsInitialize()))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    fsdevMountSdmc();
    sysLog("__appInit complete");
}

void __appExit(void)
{
    fsdevUnmountAll();
    fsExit();
    smExit();
}

}


static void uploadCurrentProfiles()
{
    sysLog("upload requested");
    Config cfg = loadConfig();
    sysLog("config username=%s password=%s", cfg.username.empty() ? "empty" : "set",
           cfg.password.empty() ? "empty" : "set");
    if (cfg.username.empty() || cfg.password.empty()) {
        sysLog("upload skipped: credentials missing");
        return;
    }

    Result rc = nifmInitialize(NifmServiceType_User);
    sysLog("nifmInitialize: 0x%08x", rc);

    for (int attempt = 0; attempt < 20; attempt++) {
        NifmInternetConnectionType connectionType;
        u32 wifiStrength;
        NifmInternetConnectionStatus connectionStatus = NifmInternetConnectionStatus_ConnectingUnknown1;
        rc = nifmGetInternetConnectionStatus(&connectionType, &wifiStrength, &connectionStatus);
        sysLog("network status rc=0x%08x status=%u", rc, connectionStatus);
        if (R_SUCCEEDED(rc) && connectionStatus == NifmInternetConnectionStatus_Connected)
            break;
        svcSleepThread(500'000'000ULL);
    }

    SocketInitConfig socketConfig = *socketGetDefaultInitConfig();
    socketConfig.tcp_tx_buf_size = 0x4000;
    socketConfig.tcp_rx_buf_size = 0x4000;
    socketConfig.tcp_tx_buf_max_size = 0x4000;
    socketConfig.tcp_rx_buf_max_size = 0x4000;
    socketConfig.udp_tx_buf_size = 0x2400;
    socketConfig.udp_rx_buf_size = 0x4000;
    socketConfig.sb_efficiency = 1;
    socketConfig.num_bsd_sessions = 1;
    rc = socketInitialize(&socketConfig);
    sysLog("socketInitializeDefault: 0x%08x", rc);
    if (!exportPlayData()) {
        sysLog("exportPlayData: failed");
    } else {
        struct stat exportStat;
        if (stat(EXPORT_PATH, &exportStat) == 0)
            sysLog("exportPlayData: success bytes=%ld", (long)exportStat.st_size);
        else
            sysLog("exportPlayData: success but stat failed");
        const bool uploadSuccess = uploadExport(cfg);
        sysLog("uploadExport: %s", uploadSuccess ? "success" : "failed");
    }

    nifmExit();
    socketExit();
}


int main()
{
    sysLog("main entered");

    Result rc = pglInitialize();
    sysLog("pglInitialize: 0x%08x", rc);
    if (R_FAILED(rc))
        return 1;

    u64 currentApplicationPid = 0;

    while (true)
    {
        u64 applicationPid = 0;
        rc = pglGetApplicationProcessId(&applicationPid);
        if (R_FAILED(rc))
            applicationPid = 0;

        if (applicationPid != 0 && applicationPid != currentApplicationPid) {
            currentApplicationPid = applicationPid;
            sysLog("application start captured pid=%016lx; waiting for exit", currentApplicationPid);
        } else if (applicationPid == 0 && currentApplicationPid != 0) {
            sysLog("application exit captured; waiting for playdata");
            svcSleepThread(3'000'000'000ULL);
            uploadCurrentProfiles();
            currentApplicationPid = 0;
        }

        svcSleepThread(1'000'000'000ULL);
    }

    pglExit();
    return 0;
}

#endif
