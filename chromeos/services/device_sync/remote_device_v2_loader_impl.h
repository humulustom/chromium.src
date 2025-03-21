// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_SERVICES_DEVICE_SYNC_REMOTE_DEVICE_V2_LOADER_IMPL_H_
#define CHROMEOS_SERVICES_DEVICE_SYNC_REMOTE_DEVICE_V2_LOADER_IMPL_H_

#include <memory>
#include <string>

#include "base/callback.h"
#include "base/containers/flat_set.h"
#include "chromeos/components/multidevice/remote_device.h"
#include "chromeos/services/device_sync/cryptauth_device.h"
#include "chromeos/services/device_sync/cryptauth_device_registry.h"
#include "chromeos/services/device_sync/remote_device_v2_loader.h"

namespace chromeos {

namespace multidevice {
class SecureMessageDelegate;
}  // namespace multidevice

namespace device_sync {

// Converts the given CryptAuthDevices into RemoteDevices. Some RemoteDevice
// fields are left empty if the CryptAuthDevice does not have
// CryptAuthBetterTogetherDeviceMetadata, for instance, if the metadata cannot
// be decrypted. If the public key is available for a device, a persistent
// symmetric key (PSK) is derived and added to the RemoteDevice; otherwise, the
// PSK is set to an empty string.
//
// A RemoteDeviceV2LoaderImpl object is designed to be used for only one Load()
// call. For a new attempt, a new object should be created. Note: The async
// calls to SecureMessage are guarded by the default DBus timeout (currently
// 25s).
class RemoteDeviceV2LoaderImpl : public RemoteDeviceV2Loader {
 public:
  class Factory {
   public:
    static Factory* Get();
    static void SetFactoryForTesting(Factory* test_factory);
    virtual ~Factory();
    virtual std::unique_ptr<RemoteDeviceV2Loader> BuildInstance();

   private:
    static Factory* test_factory_;
  };

  ~RemoteDeviceV2LoaderImpl() override;

 private:
  RemoteDeviceV2LoaderImpl();

  // Disallow copy and assign.
  explicit RemoteDeviceV2LoaderImpl(const RemoteDeviceV2Loader&) = delete;
  RemoteDeviceV2LoaderImpl& operator=(const RemoteDeviceV2LoaderImpl&) = delete;

  // RemoteDeviceV2Loader:
  void Load(
      const CryptAuthDeviceRegistry::InstanceIdToDeviceMap& id_to_device_map,
      const std::string& user_email,
      const std::string& user_private_key,
      LoadCallback callback) override;

  void OnPskDerived(const CryptAuthDevice& device,
                    const std::string& user_email,
                    const std::string& psk);
  void AddRemoteDevice(const CryptAuthDevice& device,
                       const std::string& user_email,
                       const std::string& psk);

  LoadCallback callback_;
  CryptAuthDeviceRegistry::InstanceIdToDeviceMap id_to_device_map_;
  base::flat_set<std::string> remaining_ids_to_process_;
  multidevice::RemoteDeviceList remote_devices_;
  std::unique_ptr<multidevice::SecureMessageDelegate> secure_message_delegate_;
};

}  // namespace device_sync

}  // namespace chromeos

#endif  // CHROMEOS_SERVICES_DEVICE_SYNC_REMOTE_DEVICE_V2_LOADER_IMPL_H_
