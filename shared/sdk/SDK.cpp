#include "utility/ReleaseBuild.hpp" // [RELEASE SWITCH fix141] REVR_RELEASE, REVR_LOG
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "RETypes.hpp"
#include "REGlobals.hpp"
#include "ResourceManager.hpp"

#include "SDK.hpp"

namespace reframework {
void initialize_sdk() {
#if !REVR_RELEASE // [RELEASE SWITCH fix141] no log file in the release
    if (spdlog::default_logger() == nullptr) {
        spdlog::set_default_logger(spdlog::basic_logger_mt("REFrameworkSDK", "reframework_sdk_log.txt", true));
    }
#endif

    REVR_LOG(spdlog::info("Initializing REFramework SDK..."));
    REVR_LOG(spdlog::info("REFramework SDK: https://github.com/praydog/REFramework"));

    reframework::get_types();
    reframework::get_globals();

    utility::re_managed_object::detail::resolve_add_ref();
    utility::re_managed_object::detail::resolve_release();

    sdk::ResourceManager::update_pointers();
}
}