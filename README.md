# servitor

C++ header-only library to make windows services


## Snippet

```cpp
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <cstdio>
#include <cstdarg>

#include <servitor/servitor.hpp>


class application : public servitor::application {
public:

  std::string const& title() const noexcept override {
    return title_;
  }


  std::string const& description() const noexcept override {
    return description_;
  }


  std::string const& command_line() const noexcept override {
    return command_line_;
  }


  std::chrono::milliseconds starting_timeout() const noexcept override {
    return std::chrono::milliseconds{1000};
  }


  std::chrono::milliseconds stopping_timeout() const noexcept override {
    return std::chrono::milliseconds{1000};
  }


  bool initialize() noexcept override {
    return true;
  }


  bool run() noexcept override {
    request_to_stop_ = false;
    std::unique_lock g{sync_};
    stop_cv_.wait(g, [this]{ return request_to_stop_.load(); });
    return true;
  }


  void stop() noexcept override {
    request_to_stop_ = true;
    stop_cv_.notify_one();
  }


private:

  std::string title_{"Servitor test"};
  std::string description_{"Service to test servitor library"};
  std::string command_line_{"--service"};
  std::atomic_bool request_to_stop_{false};
  std::mutex sync_;
  std::condition_variable stop_cv_;
};


auto constexpr invalid_usage_text = "Invalid usage, try with --install or --uninstall";


int error(int code, char const* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  fputs("[Error] ", stderr);
  vfprintf(stderr, fmt, args);
  va_end(args);
  return code;
}


int main(int argc, char* argv[]) {

  std::unique_ptr<application> app = std::make_unique<application>();
  std::thread console_worker;
  switch(argc) {
    case 1:
      // Running as console app
      if(!app->initialize())
        return 1;
      std::puts("Running from console, press ENTER to stop");
      console_worker = std::thread{[&]{ app->run(); }};
      std::getchar();
      app->stop();
      console_worker.join();
      break;
    case 2:
      if(strcmp(argv[1], "--install") == 0) {
        std::printf("Service '%s' is installing\n", app->title().data());
        if(!servitor::launcher::install(*app))
          return error(1, "Unable to install service '%s' (%s)\n",
                       app->title().data(),
                       servitor::launcher::last_error().message().data());

      } else if(strcmp(argv[1], "--uninstall") == 0) {
        std::printf("Service '%s' is uninstalling\n", app->title().data());
        if(!servitor::launcher::uninstall(*app))
          return error(1, "Unable to uninstall service '%s' (%s)\n",
                      app->title().data(),
                      servitor::launcher::last_error().message().data());
      } else if(strcmp(argv[1], "--service") == 0) {
        // Running as service
        servitor::launcher::run(std::move(app));
      } else {
        return error(1, invalid_usage_text);
      }
      break;
    default:
      return error(1, invalid_usage_text);
  }

  std::puts("Ok");
  return 0;
}
```
