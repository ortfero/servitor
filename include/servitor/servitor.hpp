/* This file is part of servitor library
 * Copyright 2020 Andrei Ilin <ortfero@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once


#include <cstdint>
#include <cstdio>
#include <system_error>
#include <string>
#include <memory>
#include <exception>
#include <chrono>
#include <filesystem>


#if defined (_WIN32)

#if !defined(_X86_) && !defined(_AMD64_) && !defined(_ARM_) && !defined(_ARM64_)
#if defined(_M_IX86)
#define _X86_
#elif defined(_M_AMD64)
#define _AMD64_
#elif defined(_M_ARM)
#define _ARM_
#elif defined(_M_ARM64)
#define _ARM64_
#endif
#endif


#include <minwindef.h>
#include <errhandlingapi.h>
#include <winsvc.h>
#include <synchapi.h>
#include <handleapi.h>
#include <libloaderapi.h>

#else

#error Unsupported system

#endif



namespace servitor {


  class application {
  public:

    virtual ~application() { }
    virtual std::string const& title() const noexcept = 0;
    virtual std::string const& description() const noexcept = 0;
    virtual std::string const& command_line() const noexcept = 0;
    virtual std::chrono::milliseconds starting_timeout() const noexcept = 0;
    virtual std::chrono::milliseconds stopping_timeout() const noexcept = 0;
    virtual bool initialize() noexcept = 0;
    virtual bool run() noexcept = 0;
    virtual void stop() noexcept = 0;

  }; // application


  class launcher {
  public:

    launcher(launcher const&) = delete;
    launcher& operator = (launcher const&) = delete;


    static std::error_code last_error() {
      return {int(GetLastError()), std::system_category()};
    }


    static bool install(application const& app) {
      SC_HANDLE const manager{OpenSCManager(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE)};
      if(!manager)
        return false;

      std::string command_line; command_line.resize(MAX_PATH, '\0');
      auto const path_length = GetModuleFileNameA(nullptr, command_line.data(), MAX_PATH);
      command_line.resize(path_length);
      command_line.push_back(' ');
      command_line.append(app.command_line());

      SC_HANDLE const service{CreateServiceA(manager, app.title().data(), app.title().data(), SERVICE_ALL_ACCESS,
                                               SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS,
                                               SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, command_line.data(),
                                               nullptr, nullptr, nullptr, nullptr, nullptr)};
      if(!service) {
        CloseHandle(manager);
        return false;
      }

      SERVICE_DESCRIPTIONA service_description;
      // so nasty
      service_description.lpDescription = const_cast<char*>(app.description().data());
      ChangeServiceConfig2A(service, SERVICE_CONFIG_DESCRIPTION, reinterpret_cast<void*>(&service_description));

      CloseHandle(service);
      CloseHandle(manager);

      return true;
    }


    static bool uninstall(application const& app) {

      SC_HANDLE const manager{OpenSCManager(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE)};
      if(!manager)
        return false;

      SC_HANDLE const service{OpenServiceA(manager, app.title().data(), SERVICE_QUERY_STATUS | DELETE)};
      if(!service) {
        CloseHandle(manager);
        return false;
      }

      SERVICE_STATUS status;
      if(!QueryServiceStatus(service, &status) || status.dwCurrentState != SERVICE_STOPPED) {
        CloseHandle(service);
        CloseHandle(manager);
        return false;
      }

      if(!DeleteService(service))
        return false;

      CloseHandle(service);
      CloseHandle(manager);

      return true;

    }


    static bool run(std::unique_ptr<application>&& app) {
      if(!app)
        return false;

      char buffer[MAX_PATH];
      GetModuleFileNameA(nullptr, buffer, MAX_PATH);
      namespace fs = std::filesystem;
      fs::path executable_path{buffer};
      std::error_code failed;
      fs::current_path(executable_path.parent_path(), failed);
      if(!!failed)
        return false;

      app_ = std::move(app);

      SERVICE_TABLE_ENTRYA const service_table[] = {
        { const_cast<char*>(app_->title().data()), LPSERVICE_MAIN_FUNCTIONA(&launcher::service_entry) },
        { nullptr, nullptr}
      };

      if(!StartServiceCtrlDispatcherA(service_table))
        return false;

      return true;
    }


  private:

    static std::unique_ptr<application> app_;
    static SERVICE_STATUS status_;
    static SERVICE_STATUS_HANDLE status_handle_;


    launcher() noexcept = default;


    static void __stdcall service_entry(uint32_t, char**) {

      status_ = SERVICE_STATUS{SERVICE_WIN32_OWN_PROCESS, 0, 0, 0, 0, 0, 0};

      status_handle_ = RegisterServiceCtrlHandlerA(app_->title().data(),
                                            &launcher::service_control);
      if(status_handle_ == nullptr)
        return;

      using namespace std::chrono;
      try {

        if(!report_starting(uint32_t(app_->starting_timeout().count()))) {
          report_stopped();
          return;
        }

        if(!app_->initialize()) {
          report_stopped();
          return;
        }

        report_running();
        app_->run();
        report_stopped();

      } catch(...) {
        report_stopped();
      }
    }


    static void WINAPI service_control(DWORD command) {

      if(!app_)
        return;
      try {

        switch(command) {

          case SERVICE_CONTROL_STOP:
            report_stopping(uint32_t(app_->stopping_timeout().count()));
            app_->stop();
            return;

          case SERVICE_CONTROL_INTERROGATE:
            if(status_.dwCurrentState == SERVICE_START_PENDING || status_.dwCurrentState == SERVICE_STOP_PENDING)
              ++status_.dwCheckPoint;
            SetServiceStatus(status_handle_, &status_);
            return;

          default:
            return;
        }

      } catch(...)
      { }
    }


    static bool report_starting(uint32_t millis) {
      status_.dwCurrentState = SERVICE_START_PENDING;
      status_.dwControlsAccepted = 0;
      status_.dwWaitHint = millis;
      ++status_.dwCheckPoint;
      return !!SetServiceStatus(status_handle_, &status_);
    }


    static bool report_running() {
      status_.dwCurrentState = SERVICE_RUNNING;
      status_.dwControlsAccepted = SERVICE_ACCEPT_STOP;
      status_.dwCheckPoint = 0;
      return !!SetServiceStatus(status_handle_, &status_);
    }


    static bool report_stopping(uint32_t millis) {
      status_.dwCurrentState = SERVICE_STOP_PENDING;
      status_.dwControlsAccepted = 0;
      status_.dwWaitHint = millis;
      ++status_.dwCheckPoint;
      return !!SetServiceStatus(status_handle_, &status_);
    }


    static bool report_stopped() {
      app_.reset();
      status_.dwCurrentState = SERVICE_STOPPED;
      status_.dwControlsAccepted = 0;
      status_.dwCheckPoint = 0;
      return !!SetServiceStatus(status_handle_, &status_);
    }

  }; // launcher


  inline std::unique_ptr<application> launcher::app_;
  inline SERVICE_STATUS launcher::status_;
  inline SERVICE_STATUS_HANDLE launcher::status_handle_;


}; // servitor
