#ifndef __LOG_MANAGER_H__
#define __LOG_MANAGER_H__

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace spdlog {
   class logger;
   namespace sinks {
      class sink;
   }
}


namespace bs {
   enum class LogLevel
   {
      off = 0,
      debug = 1,
      info = 2,
      warn = 3,
      err = 4,
      crit = 5
   };

   struct LogConfig
   {
      std::string fileName;
      std::string pattern;
      std::string category;
      LogLevel    level;
      bool        truncate;

      explicit LogConfig()
         : pattern("%C/%m/%d %H:%M:%S.%e [%L](%t)%n: %v"), level(LogLevel::debug), truncate(false) {}
   };

   class LogManager
   {
   public:
      using OnErrorCallback = std::function<void(void)>;

      LogManager(const OnErrorCallback &cb = nullptr);

      bool add(const LogConfig &);
      void add(const std::vector<LogConfig> &);
      bool add(const std::shared_ptr<spdlog::logger> &, const std::string &category = {});

      std::shared_ptr<spdlog::logger> logger(const std::string &category = {});

   private:
      std::shared_ptr<spdlog::logger> create(const LogConfig &);
      std::shared_ptr<spdlog::logger> createOrAppend(const std::shared_ptr<spdlog::logger> &, const LogConfig &);
      std::shared_ptr<spdlog::logger> copy(const std::shared_ptr<spdlog::logger> &, const std::string &srcCat, const std::string &category);

   private:
      const OnErrorCallback   cb_;
      std::unordered_map<std::string, std::shared_ptr<spdlog::logger>>        loggers_;
      std::unordered_map<std::string, std::shared_ptr<spdlog::sinks::sink>>   sinks_;
      std::unordered_map<std::string, std::string> patterns_;
      std::shared_ptr<spdlog::logger>              defaultLogger_;
   };

}  // namespace bs

#endif // __LOG_MANAGER_H__
