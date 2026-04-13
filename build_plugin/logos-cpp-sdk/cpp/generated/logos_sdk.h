#pragma once
#include "logos_api.h"
#include "logos_api_client.h"

#include "core_manager_api.h"

struct LogosModules {
    explicit LogosModules(LogosAPI* api) : api(api), 
        core_manager(api) {}
    LogosAPI* api;
    CoreManager core_manager;
};
