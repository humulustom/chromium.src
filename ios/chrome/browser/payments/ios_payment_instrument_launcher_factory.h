// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_PAYMENTS_IOS_PAYMENT_INSTRUMENT_LAUNCHER_FACTORY_H_
#define IOS_CHROME_BROWSER_PAYMENTS_IOS_PAYMENT_INSTRUMENT_LAUNCHER_FACTORY_H_

#include <memory>

#include "base/macros.h"
#include "base/no_destructor.h"
#include "components/keyed_service/ios/browser_state_keyed_service_factory.h"

class ChromeBrowserState;

namespace payments {
class IOSPaymentInstrumentLauncher;

// Ensures that there's only one instance of
// payments::IOSPaymentInstrumentLauncher per browser state.
class IOSPaymentInstrumentLauncherFactory
    : public BrowserStateKeyedServiceFactory {
 public:
  static IOSPaymentInstrumentLauncher* GetForBrowserState(
      ChromeBrowserState* browser_state);
  static IOSPaymentInstrumentLauncherFactory* GetInstance();

 private:
  friend class base::NoDestructor<IOSPaymentInstrumentLauncherFactory>;

  IOSPaymentInstrumentLauncherFactory();
  ~IOSPaymentInstrumentLauncherFactory() override;

  // BrowserStateKeyedServiceFactory implementation.
  std::unique_ptr<KeyedService> BuildServiceInstanceFor(
      web::BrowserState* context) const override;

  DISALLOW_COPY_AND_ASSIGN(IOSPaymentInstrumentLauncherFactory);
};

}  // namespace payments

#endif  // IOS_CHROME_BROWSER_PAYMENTS_IOS_PAYMENT_INSTRUMENT_LAUNCHER_FACTORY_H_
