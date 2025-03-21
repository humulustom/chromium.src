// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WEBLAYER_BROWSER_SYSTEM_NETWORK_CONTEXT_MANAGER_H_
#define WEBLAYER_BROWSER_SYSTEM_NETWORK_CONTEXT_MANAGER_H_

#include "base/memory/scoped_refptr.h"
#include "content/public/browser/cors_exempt_headers.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/network_service.mojom.h"

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace weblayer {

// Manages a system-wide network context that's not tied to a profile.
class SystemNetworkContextManager {
 public:
  // Creates the global instance of SystemNetworkContextManager.
  static SystemNetworkContextManager* CreateInstance(
      const std::string& user_agent);

  // Checks if the global SystemNetworkContextManager has been created.
  static bool HasInstance();

  // Gets the global SystemNetworkContextManager instance. If it has not been
  // created yet, NetworkService is called, which will cause the
  // SystemNetworkContextManager to be created.
  static SystemNetworkContextManager* GetInstance();

  // Destroys the global SystemNetworkContextManager instance.
  static void DeleteInstance();

  ~SystemNetworkContextManager();

  // Returns the System NetworkContext. Does any initialization of the
  // NetworkService that may be needed when first called.
  network::mojom::NetworkContext* GetSystemNetworkContext();

  // Called when content creates a NetworkService. Creates the
  // system NetworkContext, if the network service is enabled.
  void OnNetworkServiceCreated(network::mojom::NetworkService* network_service);

  // Returns a SharedURLLoaderFactory owned by the SystemNetworkContextManager
  // that is backed by the SystemNetworkContext.
  scoped_refptr<network::SharedURLLoaderFactory> GetSharedURLLoaderFactory();

 private:
  explicit SystemNetworkContextManager(const std::string& user_agent);

  network::mojom::NetworkContextParamsPtr
  CreateSystemNetworkContextManagerParams();

  std::string user_agent_;

  mojo::Remote<network::mojom::URLLoaderFactory> url_loader_factory_;
  scoped_refptr<network::WeakWrapperSharedURLLoaderFactory>
      shared_url_loader_factory_;

  mojo::Remote<network::mojom::NetworkContext> system_network_context_;

  DISALLOW_COPY_AND_ASSIGN(SystemNetworkContextManager);
};

}  // namespace weblayer

#endif  // WEBLAYER_BROWSER_SYSTEM_NETWORK_CONTEXT_MANAGER_H_
