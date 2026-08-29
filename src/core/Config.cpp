#include "core/Config.h"

#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace nr {

RecordingConfig RecordingConfig::defaults() {
    RecordingConfig config;
#if defined(_WIN32)
    config.general.outputDir = "~/Videos/NotionRecorder";
#endif
    return config;
}

std::string RecordingConfig::defaultFileName() const {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << "recording_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "." << general.format;
    return os.str();
}

} // namespace nr
