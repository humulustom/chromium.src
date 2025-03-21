// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ios/chrome/browser/crash_report/breadcrumbs/application_breadcrumbs_logger.h"

#include "base/bind.h"
#include "base/strings/stringprintf.h"
#include "ios/chrome/browser/crash_report/breadcrumbs/breadcrumb_manager.h"
#import "ios/chrome/browser/crash_report/crash_report_helper.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

ApplicationBreadcrumbsLogger::ApplicationBreadcrumbsLogger(
    BreadcrumbManager* breadcrumb_manager)
    : breadcrumb_manager_(breadcrumb_manager),
      user_action_callback_(
          base::BindRepeating(&ApplicationBreadcrumbsLogger::OnUserAction,
                              base::Unretained(this))),
      memory_pressure_listener_(std::make_unique<base::MemoryPressureListener>(
          base::BindRepeating(&ApplicationBreadcrumbsLogger::OnMemoryPressure,
                              base::Unretained(this)))) {
  base::AddActionCallback(user_action_callback_);
  breakpad::MonitorBreadcrumbManager(breadcrumb_manager_);
}

ApplicationBreadcrumbsLogger::~ApplicationBreadcrumbsLogger() {
  base::RemoveActionCallback(user_action_callback_);
  breakpad::StopMonitoringBreadcrumbManager(breadcrumb_manager_);
}

void ApplicationBreadcrumbsLogger::OnUserAction(const std::string& action) {
  // Filter out unwanted actions.
  if (action.find("InProductHelp.") == 0) {
    // InProductHelp actions are very noisy.
    return;
  }

  breadcrumb_manager_->AddEvent(action.c_str());
}

void ApplicationBreadcrumbsLogger::OnMemoryPressure(
    base::MemoryPressureListener::MemoryPressureLevel memory_pressure_level) {
  std::string pressure_string = "";
  switch (memory_pressure_level) {
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_NONE:
      pressure_string = "None";
      break;
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_MODERATE:
      pressure_string = "Moderate";
      break;
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL:
      pressure_string = "Critical";
      break;
  }

  std::string event =
      base::StringPrintf("Memory Pressure: %s", pressure_string.c_str());
  breadcrumb_manager_->AddEvent(event);
}
