// servicemanager for Windows: frameworks/native/cmds/servicemanager's
// ServiceManager and Access, with this main in place of main.cpp.
//
// It is built as vndservicemanager is (VENDORSERVICEMANAGER): there are no
// VINTF manifests and no perfetto on Windows. Access allows everything, as
// on any host (no SELinux). main.cpp waits on a Looper polling the binder fd
// and a timerfd for lazy services' client callbacks; binderd does not support
// polling, so here the main thread joins the binder thread pool instead, as
// servicemanager did before Android 11, and lazy-service callbacks are not
// sent. See docs/services.md and docs/decisions/0014-services-binder-ipc.md.
//
//   servicemanager [binder driver]     (default /dev/binder; the device is
//                                       $GESHER_BINDER's binderd)
#include <stdio.h>
#include <stdlib.h>

#include <android-base/logging.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/Status.h>

#include "Access.h"
#include "ServiceManager.h"

using ::android::Access;
using ::android::IPCThreadState;
using ::android::ProcessState;
using ::android::ServiceManager;
using ::android::sp;
using ::android::os::IServiceManager;

int main(int argc, char** argv) {
  android::base::InitLogging(argv, android::base::StderrLogger);

  if (argc > 2) {
    LOG(FATAL) << "usage: " << argv[0] << " [binder driver]";
  }

  const char* driver = argc == 2 ? argv[1] : "/dev/binder";

  LOG(INFO) << "Starting sm instance on " << driver;

  sp<ProcessState> ps = ProcessState::initWithDriver(driver);
  ps->setThreadPoolMaxThreadCount(0);
  ps->setCallRestriction(ProcessState::CallRestriction::FATAL_IF_NOT_ONEWAY);

  IPCThreadState::self()->disableBackgroundScheduling(true);

  sp<ServiceManager> manager = sp<ServiceManager>::make(std::make_unique<Access>());
  manager->setRequestingSid(true);
  if (!manager->addService("manager", manager, false /*allowIsolated*/,
                           IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT)
           .isOk()) {
    LOG(ERROR) << "Could not self register servicemanager";
  }

  IPCThreadState::self()->setTheContextObject(manager);
  if (!ps->becomeContextManager()) {
    LOG(FATAL) << "Could not become context manager";
  }

  // Ready: the context manager answers from here on.
  fprintf(stderr, "servicemanager: ready on %s\n", driver);
  fflush(stderr);

  IPCThreadState::self()->joinThreadPool(true /*isMain*/);

  // should not be reached
  return EXIT_FAILURE;
}
