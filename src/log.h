#ifndef log_h_
#define log_h_

#include <spdlog/spdlog.h>

template <typename Arg, typename... Args>
void log_trace(Arg arg, Args... args) {
  auto console = spdlog::get("console");
  console->trace(arg, args...);
}

template <typename Arg, typename... Args>
void log_debug(Arg arg, Args... args) {
  auto console = spdlog::get("console");
  console->debug(arg, args...);
}

template <typename Arg, typename... Args>
void log_info(Arg arg, Args... args) {
  auto console = spdlog::get("console");
  console->info(arg, args...);
}

template <typename Arg, typename... Args>
void log_warn(Arg arg, Args... args) {
  auto console = spdlog::get("console");
  console->warn(arg, args...);
}

template <typename Arg, typename... Args>
void log_error(Arg arg, Args... args) {
  auto console = spdlog::get("err_logger");
  console->error(arg, args...);
}

template <typename Arg, typename... Args>
void log_critical(Arg arg, Args... args) {
  auto console = spdlog::get("err_logger");
  console->critical(arg, args...);
}

#endif /* log_h_ */
