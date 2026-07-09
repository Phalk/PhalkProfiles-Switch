#include <switch.h>
#include <curl/curl.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "nlohmann/json.hpp"


#define CONFIG_PATH "/config/PhalkProfiles/config.txt"
#define EXPORT_PATH "/switch/PhalkProfiles/export.json"


using json = nlohmann::json;


// ============================================================================
// PLAYDATA
// ============================================================================

namespace PlayData
{

json exportPlayLog()
{
    json result;

    // 1. Metadados Globais
    result["exportString"] = "Exportado via PhalkProfiles";
    result["exportTimestamp"] = (u64)time(NULL);
    result["users"] = json::array();

    // Inicializa os serviços do sistema
    accountInitialize(AccountServiceType_Administrator);
    pdmqryInitialize();
    nsInitialize(); // Inicializa o serviço necessário para obter os nomes dos jogos

    s32 userCount = 0;
    AccountUid userList[ACC_USER_LIST_SIZE]; 
    
    if (R_SUCCEEDED(accountListAllUsers(userList, ACC_USER_LIST_SIZE, &userCount))) 
    {
        // Iterar sobre cada Usuário do Switch
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

            // Pegar o range válido de eventos do PDM global para varredura rápida de IDs existentes
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

                // Varredura ultra rápida apenas para coletar quais IDs de jogos possuem histórico
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

                // Agora construímos a estrutura limpa contendo apenas Nome, ID e Summary
                for (u64 titleId : foundTitles) 
                {
                    PdmPlayStatistics tmpStats;
                    Result rcStats = pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(titleId, userId, false, &tmpStats);

                    // Só processa se o usuário de fato tiver aberto o jogo alguma vez
                    if (R_SUCCEEDED(rcStats) && tmpStats.total_launches > 0) 
                    {
                        json tJson;
                        char titleIdStr[20];
                        snprintf(titleIdStr, sizeof(titleIdStr), "%016lx", (unsigned long)titleId);
                        
                        tJson["id"] = titleIdStr;
                         // --- MÁGICA PARA PEGAR O NOME DO JOGO ---
                        std::string gameName = "Jogo Desconhecido (" + std::string(titleIdStr) + ")";
                        
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
                                    // Mapeamento manual e definitivo baseado no Horizon OS / libnx
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

                                    // Se encontrou o idioma do console, valida na tabela do jogo
                                    if (nacpLanguageIndex >= 0 && nacpLanguageIndex < 16) {
                                        if (controlData->nacp.lang[nacpLanguageIndex].name[0] != '\0') {
                                            langEntry = &controlData->nacp.lang[nacpLanguageIndex];
                                        }
                                    }
                                }

                                // FALLBACK 1: Se o idioma do console não tiver tradução no jogo, tenta o Inglês Americano (Índice 1)
                                if (langEntry == NULL && controlData->nacp.lang[1].name[0] != '\0') {
                                    langEntry = &controlData->nacp.lang[1];
                                }

                                // FALLBACK 2: Se nem o Inglês estiver disponível, varre a tabela e pega o primeiro idioma populado
                                if (langEntry == NULL) {
                                    for (int lang = 0; lang < 16; lang++) {
                                        if (controlData->nacp.lang[lang].name[0] != '\0') {
                                            langEntry = &controlData->nacp.lang[lang];
                                            break;
                                        }
                                    }
                                }

                                // Se encontramos uma entrada válida com texto, define o nome do jogo
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
                        tJson["summary"]["playtime"] = tmpStats.playtime / 1000 / 1000 / 1000; // Segundos
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



// ============================================================================
// CURL
// ============================================================================

namespace Curl
{


void init()
{
    curl_global_init(
        CURL_GLOBAL_DEFAULT
    );
}



void exit()
{
    curl_global_cleanup();
}



bool uploadFile(
    const std::string& url,
    const std::string& filePath,
    const std::string& username,
    const std::string& password
)
{

    CURL *curl = curl_easy_init();


    if(!curl)
        return false;



    curl_mime *mime = curl_mime_init(curl);


    if(!mime)
    {
        curl_easy_cleanup(curl);
        return false;
    }



    curl_mimepart *part;



    // user_name

    part = curl_mime_addpart(mime);

    curl_mime_name(
        part,
        "user_name"
    );

    curl_mime_data(
        part,
        username.c_str(),
        CURL_ZERO_TERMINATED
    );



    // password

    part = curl_mime_addpart(mime);

    curl_mime_name(
        part,
        "password"
    );

    curl_mime_data(
        part,
        password.c_str(),
        CURL_ZERO_TERMINATED
    );



    // arquivo JSON

    part = curl_mime_addpart(mime);

    curl_mime_name(
        part,
        "json_file"
    );

    curl_mime_filedata(
        part,
        filePath.c_str()
    );



    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        url.c_str()
    );


    curl_easy_setopt(
        curl,
        CURLOPT_MIMEPOST,
        mime
    );



    CURLcode res =
        curl_easy_perform(curl);



    curl_mime_free(mime);

    curl_easy_cleanup(curl);



    return res == CURLE_OK;
}


}



// ============================================================================
// CONFIG
// ============================================================================


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



// ============================================================================
// TECLADO
// ============================================================================


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



// ============================================================================
// MAIN
// ============================================================================


int main()
{

    consoleInit(NULL);
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
    Curl::init();

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



    while(appletMainLoop())
    {

        padUpdate(&pad);


        u64 down =
            padGetButtonsDown(&pad);



        if(down & HidNpadButton_Plus)
            break;



        if(down & HidNpadButton_A)
        {

            auto value =
                askText(
                    "Username"
                );


            if(!value.empty())
            {
                cfg.username=value;
                saveConfig(cfg);

                status="Username saved";
            }

        }



        if(down & HidNpadButton_X)
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

                status="Password saved";
            }

        }




        if(down & HidNpadButton_R)
        {

            mkdir(
                "/switch/PhalkProfiles",
                0777
            );


            auto data =
                PlayData::exportPlayLog();



            std::ofstream file(
                EXPORT_PATH
            );


            if(file.is_open())
            {

                file
                    << data.dump(4);


                file.close();



                bool ok =
                    Curl::uploadFile(
                        "https://www.phalk.net/profiles/json_curl.php",
                        EXPORT_PATH,
                        cfg.username,
                        cfg.password
                    );


                status =
                    ok ?
                    "Data uploaded successfully!" :
                    "Data upload failed.";

            }
            else
            {
                status =
                    "Error creating JSON file";
            }

        }



        consoleClear();


        printf(
            "=== Phalk Profiles ===\n\n"
        );


        printf(
            "User: %s\n",
            cfg.username.empty()
            ? "(empty)"
            : cfg.username.c_str()
        );


        printf(
            "Password: %s\n\n",
            cfg.password.empty()
            ? "(empty)"
            : "******"
        );


        printf(
            "Status: %s\n\n",
            status.c_str()
        );


        printf(
            "[A] Set username\n"
        );


        printf(
            "[X] Set password\n"
        );


        printf(
            "[R] Export & upload\n"
        );


        printf(
            "[+] Quit\n"
        );



        consoleUpdate(NULL);

    }



    Curl::exit();



    nifmExit();

    sslExit();

    socketExit();



    consoleExit(NULL);


    return 0;
}