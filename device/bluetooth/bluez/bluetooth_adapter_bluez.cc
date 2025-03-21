// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/bluetooth/bluez/bluetooth_adapter_bluez.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_macros.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/stringprintf.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "components/device_event_log/device_event_log.h"
#include "device/bluetooth/bluetooth_common.h"
#include "device/bluetooth/bluetooth_device.h"
#include "device/bluetooth/bluetooth_discovery_session_outcome.h"
#include "device/bluetooth/bluetooth_socket_thread.h"
#include "device/bluetooth/bluez/bluetooth_adapter_profile_bluez.h"
#include "device/bluetooth/bluez/bluetooth_advertisement_bluez.h"
#include "device/bluetooth/bluez/bluetooth_device_bluez.h"
#include "device/bluetooth/bluez/bluetooth_gatt_service_bluez.h"
#include "device/bluetooth/bluez/bluetooth_local_gatt_characteristic_bluez.h"
#include "device/bluetooth/bluez/bluetooth_local_gatt_service_bluez.h"
#include "device/bluetooth/bluez/bluetooth_pairing_bluez.h"
#include "device/bluetooth/bluez/bluetooth_socket_bluez.h"
#include "device/bluetooth/dbus/bluetooth_adapter_client.h"
#include "device/bluetooth/dbus/bluetooth_agent_manager_client.h"
#include "device/bluetooth/dbus/bluetooth_agent_service_provider.h"
#include "device/bluetooth/dbus/bluetooth_device_client.h"
#include "device/bluetooth/dbus/bluetooth_gatt_application_service_provider.h"
#include "device/bluetooth/dbus/bluetooth_gatt_manager_client.h"
#include "device/bluetooth/dbus/bluetooth_input_client.h"
#include "device/bluetooth/dbus/bluetooth_le_advertising_manager_client.h"
#include "device/bluetooth/dbus/bluez_dbus_manager.h"
#include "device/bluetooth/public/cpp/bluetooth_uuid.h"
#include "third_party/cros_system_api/dbus/service_constants.h"

#if defined(OS_CHROMEOS)
#include "chromeos/constants/devicetype.h"
#include "device/bluetooth/bluetooth_adapter_factory.h"
#include "device/bluetooth/chromeos/bluetooth_utils.h"
#endif

using device::BluetoothAdapter;
using device::BluetoothDevice;
using UUIDSet = device::BluetoothDevice::UUIDSet;
using device::BluetoothDiscoveryFilter;
using device::BluetoothSocket;
using device::BluetoothUUID;
using device::UMABluetoothDiscoverySessionOutcome;

namespace {

// The agent path is relatively meaningless since BlueZ only permits one to
// exist per D-Bus connection, it just has to be unique within Chromium.
const char kAgentPath[] = "/org/chromium/bluetooth_agent";
const char kGattApplicationObjectPath[] = "/gatt_application";

#if defined(OS_CHROMEOS)
// Twice the period that field trial configs are fetched at.
constexpr base::TimeDelta kSetLongTermKeysAfterFirstTimeInstallDuration =
    base::TimeDelta::FromMinutes(60);
#endif

void OnUnregisterAgentError(const std::string& error_name,
                            const std::string& error_message) {
  // It's okay if the agent didn't exist, it means we never saw an adapter.
  if (error_name == bluetooth_agent_manager::kErrorDoesNotExist)
    return;

  BLUETOOTH_LOG(ERROR) << "Failed to unregister pairing agent: " << error_name
                       << ": " << error_message;
}

UMABluetoothDiscoverySessionOutcome TranslateDiscoveryErrorToUMA(
    const std::string& error_name) {
  if (error_name == bluez::BluetoothAdapterClient::kUnknownAdapterError) {
    return UMABluetoothDiscoverySessionOutcome::BLUEZ_DBUS_UNKNOWN_ADAPTER;
  } else if (error_name == bluez::BluetoothAdapterClient::kNoResponseError) {
    return UMABluetoothDiscoverySessionOutcome::BLUEZ_DBUS_NO_RESPONSE;
  } else if (error_name == bluetooth_device::kErrorInProgress) {
    return UMABluetoothDiscoverySessionOutcome::BLUEZ_DBUS_IN_PROGRESS;
  } else if (error_name == bluetooth_device::kErrorNotReady) {
    return UMABluetoothDiscoverySessionOutcome::BLUEZ_DBUS_NOT_READY;
  } else if (error_name == bluetooth_device::kErrorNotSupported) {
    return UMABluetoothDiscoverySessionOutcome::BLUEZ_DBUS_UNSUPPORTED_DEVICE;
  } else if (error_name == bluetooth_device::kErrorFailed) {
    return UMABluetoothDiscoverySessionOutcome::FAILED;
  } else {
    BLUETOOTH_LOG(ERROR) << "Unrecognized DBus error " << error_name;
    return UMABluetoothDiscoverySessionOutcome::UNKNOWN;
  }
}

#if defined(OS_CHROMEOS)
device::BluetoothDevice::ServiceDataMap ConvertServiceDataMap(
    const base::flat_map<std::string, std::vector<uint8_t>>& input) {
  device::BluetoothDevice::ServiceDataMap output;
  for (auto& i : input) {
    output[BluetoothUUID(i.first)] = i.second;
  }

  return output;
}

device::BluetoothDevice::ManufacturerDataMap ConvertManufacturerDataMap(
    const base::flat_map<uint16_t, std::vector<uint8_t>>& input) {
  return device::BluetoothDevice::ManufacturerDataMap(input.begin(),
                                                      input.end());
}
#endif  // defined(OS_CHROMEOS)

}  // namespace

namespace device {

// static
base::WeakPtr<BluetoothAdapter> BluetoothAdapter::CreateAdapter(
    InitCallback init_callback) {
  return bluez::BluetoothAdapterBlueZ::CreateAdapter(std::move(init_callback));
}

}  // namespace device

namespace bluez {

namespace {

void OnRegistrationErrorCallback(
    device::BluetoothGattService::ErrorCallback error_callback,
    bool is_register_callback,
    const std::string& error_name,
    const std::string& error_message) {
  if (is_register_callback) {
    BLUETOOTH_LOG(ERROR) << "Failed to Register service: " << error_name << ", "
                         << error_message;
  } else {
    BLUETOOTH_LOG(ERROR) << "Failed to Unregister service: " << error_name
                         << ", " << error_message;
  }
  std::move(error_callback)
      .Run(BluetoothGattServiceBlueZ::DBusErrorToServiceError(error_name));
}

void SetIntervalErrorCallbackConnector(
    const device::BluetoothAdapter::AdvertisementErrorCallback& error_callback,
    const std::string& error_name,
    const std::string& error_message) {
  BLUETOOTH_LOG(ERROR) << "Error while registering advertisement. error_name = "
                       << error_name << ", error_message = " << error_message;

  device::BluetoothAdvertisement::ErrorCode code = device::
      BluetoothAdvertisement::ErrorCode::INVALID_ADVERTISEMENT_ERROR_CODE;
  if (error_name == bluetooth_advertising_manager::kErrorInvalidArguments) {
    code = device::BluetoothAdvertisement::ErrorCode::
        ERROR_INVALID_ADVERTISEMENT_INTERVAL;
  }
  error_callback.Run(code);
}

void ResetAdvertisingErrorCallbackConnector(
    const device::BluetoothAdapter::AdvertisementErrorCallback& error_callback,
    const std::string& error_name,
    const std::string& error_message) {
  BLUETOOTH_LOG(ERROR) << "Error while resetting advertising. error_name = "
                       << error_name << ", error_message = " << error_message;

  error_callback.Run(
      device::BluetoothAdvertisement::ErrorCode::ERROR_RESET_ADVERTISING);
}

}  // namespace

// static
base::WeakPtr<BluetoothAdapter> BluetoothAdapterBlueZ::CreateAdapter(
    InitCallback init_callback) {
  BluetoothAdapterBlueZ* adapter =
      new BluetoothAdapterBlueZ(std::move(init_callback));
  return adapter->weak_ptr_factory_.GetWeakPtr();
}

void BluetoothAdapterBlueZ::Shutdown() {
  if (dbus_is_shutdown_)
    return;

  BLUETOOTH_LOG(EVENT) << "BluetoothAdapterBlueZ::Shutdown";

  DCHECK(bluez::BluezDBusManager::IsInitialized())
      << "Call BluetoothAdapterFactory::Shutdown() before "
         "BluezDBusManager::Shutdown().";

  // Since we don't initialize anything if Object Manager is not supported,
  // no need to do any clean up.
  if (!bluez::BluezDBusManager::Get()->IsObjectManagerSupported()) {
    dbus_is_shutdown_ = true;
    return;
  }

  if (IsPresent())
    RemoveAdapter();  // Also deletes devices_.
  DCHECK(devices_.empty());

  // profiles_ must be empty because all BluetoothSockets have been notified
  // that this adapter is disappearing.
  DCHECK(profiles_.empty());

  // Some profiles may have been released but not yet removed; it is safe to
  // delete them.
  for (auto& it : released_profiles_)
    delete it.second;
  released_profiles_.clear();

  for (auto& it : profile_queues_)
    delete it.second;
  profile_queues_.clear();

  // This may call unregister on advertisements that have already been
  // unregistered but that's fine. The advertisement object keeps a track of
  // the fact that it has been already unregistered and will call our empty
  // error callback with an "Already unregistered" error, which we'll ignore.
  for (auto& it : advertisements_) {
    it->Unregister(base::DoNothing(), base::DoNothing());
  }
  advertisements_.clear();

  bluez::BluezDBusManager::Get()->GetBluetoothAdapterClient()->RemoveObserver(
      this);
  bluez::BluezDBusManager::Get()->GetBluetoothDeviceClient()->RemoveObserver(
      this);
  bluez::BluezDBusManager::Get()->GetBluetoothInputClient()->RemoveObserver(
      this);
  bluez::BluezDBusManager::Get()
      ->GetBluetoothAgentManagerClient()
      ->RemoveObserver(this);

  BLUETOOTH_LOG(EVENT) << "Unregistering pairing agent";
  bluez::BluezDBusManager::Get()
      ->GetBluetoothAgentManagerClient()
      ->UnregisterAgent(dbus::ObjectPath(kAgentPath), base::DoNothing(),
                        base::BindOnce(&OnUnregisterAgentError));

  agent_.reset();

  dbus_is_shutdown_ = true;
}

BluetoothAdapterBlueZ::BluetoothAdapterBlueZ(InitCallback init_callback)
    : init_callback_(std::move(init_callback)),
      initialized_(false),
      dbus_is_shutdown_(false) {
  ui_task_runner_ = base::ThreadTaskRunnerHandle::Get();
  socket_thread_ = device::BluetoothSocketThread::Get();

  // Can't initialize the adapter until DBus clients are ready.
  if (bluez::BluezDBusManager::Get()->IsObjectManagerSupportKnown()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(&BluetoothAdapterBlueZ::Init,
                                  weak_ptr_factory_.GetWeakPtr()));
    return;
  }
  bluez::BluezDBusManager::Get()->CallWhenObjectManagerSupportIsKnown(
      base::Bind(&BluetoothAdapterBlueZ::Init, weak_ptr_factory_.GetWeakPtr()));
}

void BluetoothAdapterBlueZ::Init() {
  // We may have been shutdown already, in which case do nothing. If the
  // platform doesn't support Object Manager then Bluez 5 is probably not
  // present. In this case we just return without initializing anything.
  if (dbus_is_shutdown_ ||
      !bluez::BluezDBusManager::Get()->IsObjectManagerSupported()) {
    initialized_ = true;
    std::move(init_callback_).Run();
    return;
  }

  bluez::BluezDBusManager::Get()->GetBluetoothAdapterClient()->AddObserver(
      this);
  bluez::BluezDBusManager::Get()->GetBluetoothDeviceClient()->AddObserver(this);
  bluez::BluezDBusManager::Get()->GetBluetoothInputClient()->AddObserver(this);
  bluez::BluezDBusManager::Get()->GetBluetoothAgentManagerClient()->AddObserver(
      this);

  // Register the pairing agent.
  dbus::Bus* system_bus = bluez::BluezDBusManager::Get()->GetSystemBus();
  agent_.reset(bluez::BluetoothAgentServiceProvider::Create(
      system_bus, dbus::ObjectPath(kAgentPath), this));
  DCHECK(agent_.get());

  std::vector<dbus::ObjectPath> object_paths = bluez::BluezDBusManager::Get()
                                                   ->GetBluetoothAdapterClient()
                                                   ->GetAdapters();

  BLUETOOTH_LOG(EVENT) << "BlueZ Adapter Initialized.";
  if (!object_paths.empty()) {
    BLUETOOTH_LOG(EVENT) << "BlueZ Adapters available: " << object_paths.size();
    SetAdapter(object_paths[0]);
  }
  initialized_ = true;
  std::move(init_callback_).Run();
}

BluetoothAdapterBlueZ::~BluetoothAdapterBlueZ() {
  Shutdown();
}

std::string BluetoothAdapterBlueZ::GetAddress() const {
  if (!IsPresent())
    return std::string();

  bluez::BluetoothAdapterClient::Properties* properties =
      bluez::BluezDBusManager::Get()
          ->GetBluetoothAdapterClient()
          ->GetProperties(object_path_);
  DCHECK(properties);

  return BluetoothDevice::CanonicalizeAddress(properties->address.value());
}

std::string BluetoothAdapterBlueZ::GetName() const {
  if (!IsPresent())
    return std::string();

  bluez::BluetoothAdapterClient::Properties* properties =
      bluez::BluezDBusManager::Get()
          ->GetBluetoothAdapterClient()
          ->GetProperties(object_path_);
  DCHECK(properties);

  return properties->alias.value();
}

void BluetoothAdapterBlueZ::SetName(const std::string& name,
                                    const base::Closure& callback,
                                    const ErrorCallback& error_callback) {
  if (!IsPresent()) {
    error_callback.Run();
    return;
  }

  bluez::BluezDBusManager::Get()
      ->GetBluetoothAdapterClient()
      ->GetProperties(object_path_)
      ->alias.Set(
          name,
          base::Bind(&BluetoothAdapterBlueZ::OnPropertyChangeCompleted,
                     weak_ptr_factory_.GetWeakPtr(), callback, error_callback));
}

bool BluetoothAdapterBlueZ::IsInitialized() const {
  return initialized_;
}

bool BluetoothAdapterBlueZ::IsPresent() const {
  return !dbus_is_shutdown_ && !object_path_.value().empty();
}

bool BluetoothAdapterBlueZ::IsPowered() const {
  if (!IsPresent())
    return false;

  bluez::BluetoothAdapterClient::Properties* properties =
      bluez::BluezDBusManager::Get()
          ->GetBluetoothAdapterClient()
          ->GetProperties(object_path_);

  return properties->powered.value();
}

void BluetoothAdapterBlueZ::SetPowered(bool powered,
                                       const base::Closure& callback,
                                       const ErrorCallback& error_callback) {
  if (!IsPresent()) {
    BLUETOOTH_LOG(ERROR) << "SetPowered: " << powered << ". Not Present!";
    error_callback.Run();
    return;
  }

  BLUETOOTH_LOG(EVENT) << "SetPowered: " << powered;

  bluez::BluezDBusManager::Get()
      ->GetBluetoothAdapterClient()
      ->GetProperties(object_path_)
      ->powered.Set(
          powered,
          base::Bind(&BluetoothAdapterBlueZ::OnPropertyChangeCompleted,
                     weak_ptr_factory_.GetWeakPtr(), callback, error_callback));
}

bool BluetoothAdapterBlueZ::IsDiscoverable() const {
  if (!IsPresent())
    return false;

  bluez::BluetoothAdapterClient::Properties* properties =
      bluez::BluezDBusManager::Get()
          ->GetBluetoothAdapterClient()
          ->GetProperties(object_path_);

  return properties->discoverable.value();
}

void BluetoothAdapterBlueZ::SetDiscoverable(
    bool discoverable,
    const base::Closure& callback,
    const ErrorCallback& error_callback) {
  if (!IsPresent()) {
    error_callback.Run();
    return;
  }

  BLUETOOTH_LOG(EVENT) << "SetDiscoverable: " << discoverable;

  bluez::BluezDBusManager::Get()
      ->GetBluetoothAdapterClient()
      ->GetProperties(object_path_)
      ->discoverable.Set(
          discoverable,
          base::Bind(&BluetoothAdapterBlueZ::OnSetDiscoverable,
                     weak_ptr_factory_.GetWeakPtr(), callback, error_callback));
}

uint32_t BluetoothAdapterBlueZ::GetDiscoverableTimeout() const {
  if (!IsPresent())
    return 0;

  bluez::BluetoothAdapterClient::Properties* properties =
      bluez::BluezDBusManager::Get()
          ->GetBluetoothAdapterClient()
          ->GetProperties(object_path_);

  return properties->discoverable_timeout.value();
}

bool BluetoothAdapterBlueZ::IsDiscovering() const {
  if (!IsPresent())
    return false;

  return NumScanningDiscoverySessions() > 0;
}

bool BluetoothAdapterBlueZ::IsDiscoveringForTesting() const {
  if (!IsPresent())
    return false;

  bluez::BluetoothAdapterClient::Properties* properties =
      bluez::BluezDBusManager::Get()
          ->GetBluetoothAdapterClient()
          ->GetProperties(object_path_);
  return properties->discovering.value();
}

std::unordered_map<BluetoothDevice*, UUIDSet>
BluetoothAdapterBlueZ::RetrieveGattConnectedDevicesWithDiscoveryFilter(
    const BluetoothDiscoveryFilter& discovery_filter) {
  std::unordered_map<BluetoothDevice*, UUIDSet> connected_devices;

  std::set<BluetoothUUID> filter_uuids;
  discovery_filter.GetUUIDs(filter_uuids);

  for (BluetoothDevice* device : GetDevices()) {
    if (device->IsGattConnected() &&
        (device->GetType() & device::BLUETOOTH_TRANSPORT_LE)) {
      UUIDSet device_uuids = device->GetUUIDs();

      UUIDSet intersection;
      for (const BluetoothUUID& uuid : filter_uuids) {
        if (base::Contains(device_uuids, uuid)) {
          intersection.insert(uuid);
        }
      }

      if (filter_uuids.empty() || !intersection.empty()) {
        connected_devices[device] = std::move(intersection);
      }
    }
  }

  return connected_devices;
}

BluetoothAdapterBlueZ::UUIDList BluetoothAdapterBlueZ::GetUUIDs() const {
  if (!IsPresent())
    return UUIDList();

  bluez::BluetoothAdapterClient::Properties* properties =
      bluez::BluezDBusManager::Get()
          ->GetBluetoothAdapterClient()
          ->GetProperties(object_path_);
  DCHECK(properties);

  std::vector<std::string> uuids = properties->uuids.value();

  return UUIDList(uuids.begin(), uuids.end());
}

void BluetoothAdapterBlueZ::CreateRfcommService(
    const BluetoothUUID& uuid,
    const ServiceOptions& options,
    const CreateServiceCallback& callback,
    const CreateServiceErrorCallback& error_callback) {
  DCHECK(!dbus_is_shutdown_);
  BLUETOOTH_LOG(DEBUG) << object_path_.value() << ": Creating RFCOMM service: "
                       << uuid.canonical_value();
  scoped_refptr<BluetoothSocketBlueZ> socket =
      BluetoothSocketBlueZ::CreateBluetoothSocket(ui_task_runner_,
                                                  socket_thread_);
  socket->Listen(this, BluetoothSocketBlueZ::kRfcomm, uuid, options,
                 base::Bind(callback, socket), error_callback);
}

void BluetoothAdapterBlueZ::CreateL2capService(
    const BluetoothUUID& uuid,
    const ServiceOptions& options,
    const CreateServiceCallback& callback,
    const CreateServiceErrorCallback& error_callback) {
  DCHECK(!dbus_is_shutdown_);
  BLUETOOTH_LOG(DEBUG) << object_path_.value() << ": Creating L2CAP service: "
                       << uuid.canonical_value();
  scoped_refptr<BluetoothSocketBlueZ> socket =
      BluetoothSocketBlueZ::CreateBluetoothSocket(ui_task_runner_,
                                                  socket_thread_);
  socket->Listen(this, BluetoothSocketBlueZ::kL2cap, uuid, options,
                 base::Bind(callback, socket), error_callback);
}

void BluetoothAdapterBlueZ::RegisterAdvertisement(
    std::unique_ptr<device::BluetoothAdvertisement::Data> advertisement_data,
    const CreateAdvertisementCallback& callback,
    const AdvertisementErrorCallback& error_callback) {
  scoped_refptr<BluetoothAdvertisementBlueZ> advertisement(
      new BluetoothAdvertisementBlueZ(std::move(advertisement_data), this));
  advertisement->Register(base::Bind(callback, advertisement), error_callback);
  advertisements_.emplace_back(advertisement);
}

void BluetoothAdapterBlueZ::SetAdvertisingInterval(
    const base::TimeDelta& min,
    const base::TimeDelta& max,
    const base::Closure& callback,
    const AdvertisementErrorCallback& error_callback) {
  DCHECK(bluez::BluezDBusManager::Get());
  uint16_t min_ms = static_cast<uint16_t>(
      std::min(static_cast<int64_t>(std::numeric_limits<uint16_t>::max()),
               min.InMilliseconds()));
  uint16_t max_ms = static_cast<uint16_t>(
      std::min(static_cast<int64_t>(std::numeric_limits<uint16_t>::max()),
               max.InMilliseconds()));
  bluez::BluezDBusManager::Get()
      ->GetBluetoothLEAdvertisingManagerClient()
      ->SetAdvertisingInterval(
          object_path_, min_ms, max_ms, callback,
          base::Bind(&SetIntervalErrorCallbackConnector, error_callback));
}

void BluetoothAdapterBlueZ::ResetAdvertising(
    const base::Closure& callback,
    const AdvertisementErrorCallback& error_callback) {
  DCHECK(bluez::BluezDBusManager::Get());
  bluez::BluezDBusManager::Get()
      ->GetBluetoothLEAdvertisingManagerClient()
      ->ResetAdvertising(
          object_path_, callback,
          base::Bind(&ResetAdvertisingErrorCallbackConnector, error_callback));
}

device::BluetoothLocalGattService* BluetoothAdapterBlueZ::GetGattService(
    const std::string& identifier) const {
  const auto& service = owned_gatt_services_.find(dbus::ObjectPath(identifier));
  return service == owned_gatt_services_.end() ? nullptr
                                               : service->second.get();
}

void BluetoothAdapterBlueZ::RemovePairingDelegateInternal(
    BluetoothDevice::PairingDelegate* pairing_delegate) {
  // Check if any device is using the pairing delegate.
  // If so, clear the pairing context which will make any responses no-ops.
  for (auto iter = devices_.begin(); iter != devices_.end(); ++iter) {
    BluetoothDeviceBlueZ* device_bluez =
        static_cast<BluetoothDeviceBlueZ*>(iter->second.get());

    BluetoothPairingBlueZ* pairing = device_bluez->GetPairing();
    if (pairing && pairing->GetPairingDelegate() == pairing_delegate)
      device_bluez->EndPairing();
  }
}

void BluetoothAdapterBlueZ::AdapterAdded(const dbus::ObjectPath& object_path) {
  // Set the adapter to the newly added adapter only if no adapter is present.
  if (!IsPresent())
    SetAdapter(object_path);
}

void BluetoothAdapterBlueZ::AdapterRemoved(
    const dbus::ObjectPath& object_path) {
  if (object_path == object_path_)
    RemoveAdapter();
}

void BluetoothAdapterBlueZ::AdapterPropertyChanged(
    const dbus::ObjectPath& object_path,
    const std::string& property_name) {
  if (object_path != object_path_)
    return;
  DCHECK(IsPresent());

  bluez::BluetoothAdapterClient::Properties* properties =
      bluez::BluezDBusManager::Get()
          ->GetBluetoothAdapterClient()
          ->GetProperties(object_path_);

  if (property_name == properties->powered.name()) {
    NotifyAdapterPoweredChanged(properties->powered.value());
  } else if (property_name == properties->discoverable.name()) {
    DiscoverableChanged(properties->discoverable.value());
  } else if (property_name == properties->discovering.name()) {
    DiscoveringChanged(properties->discovering.value());
  }
}

void BluetoothAdapterBlueZ::DeviceAdded(const dbus::ObjectPath& object_path) {
  DCHECK(bluez::BluezDBusManager::Get());
  bluez::BluetoothDeviceClient::Properties* properties =
      bluez::BluezDBusManager::Get()->GetBluetoothDeviceClient()->GetProperties(
          object_path);
  if (!properties || properties->adapter.value() != object_path_)
    return;
  DCHECK(IsPresent());

  BluetoothDeviceBlueZ* device_bluez = new BluetoothDeviceBlueZ(
      this, object_path, ui_task_runner_, socket_thread_);
  DCHECK(devices_.find(device_bluez->GetAddress()) == devices_.end());

  devices_[device_bluez->GetAddress()] = base::WrapUnique(device_bluez);

  if (properties->rssi.is_valid() && properties->eir.is_valid()) {
    NotifyDeviceAdvertisementReceived(device_bluez, properties->rssi.value(),
                                      properties->eir.value());
  }

  for (auto& observer : observers_)
    observer.DeviceAdded(this, device_bluez);
}

void BluetoothAdapterBlueZ::DeviceRemoved(const dbus::ObjectPath& object_path) {
  for (auto iter = devices_.begin(); iter != devices_.end(); ++iter) {
    BluetoothDeviceBlueZ* device_bluez =
        static_cast<BluetoothDeviceBlueZ*>(iter->second.get());
    if (device_bluez->object_path() == object_path) {
      std::unique_ptr<BluetoothDevice> scoped_device = std::move(iter->second);
      devices_.erase(iter);

      for (auto& observer : observers_)
        observer.DeviceRemoved(this, device_bluez);
      return;
    }
  }
}

void BluetoothAdapterBlueZ::DevicePropertyChanged(
    const dbus::ObjectPath& object_path,
    const std::string& property_name) {
  BluetoothDeviceBlueZ* device_bluez = GetDeviceWithPath(object_path);
  if (!device_bluez)
    return;

  bluez::BluetoothDeviceClient::Properties* properties =
      bluez::BluezDBusManager::Get()->GetBluetoothDeviceClient()->GetProperties(
          object_path);

  if (property_name == properties->address.name()) {
    for (auto iter = devices_.begin(); iter != devices_.end(); ++iter) {
      if (iter->second->GetAddress() == device_bluez->GetAddress()) {
        std::string old_address = iter->first;
        BLUETOOTH_LOG(EVENT) << "Device changed address, old: " << old_address
                             << " new: " << device_bluez->GetAddress();
        std::unique_ptr<BluetoothDevice> scoped_device =
            std::move(iter->second);
        devices_.erase(iter);

        DCHECK(devices_.find(device_bluez->GetAddress()) == devices_.end());
        devices_[device_bluez->GetAddress()] = std::move(scoped_device);
        NotifyDeviceAddressChanged(device_bluez, old_address);
        break;
      }
    }
  }

  if (property_name == properties->service_data.name())
    device_bluez->UpdateServiceData();
  else if (property_name == properties->manufacturer_data.name())
    device_bluez->UpdateManufacturerData();
  else if (property_name == properties->advertising_data_flags.name())
    device_bluez->UpdateAdvertisingDataFlags();

  if (property_name == properties->bluetooth_class.name() ||
      property_name == properties->appearance.name() ||
      property_name == properties->address.name() ||
      property_name == properties->name.name() ||
      property_name == properties->paired.name() ||
      property_name == properties->trusted.name() ||
      property_name == properties->connected.name() ||
      property_name == properties->uuids.name() ||
      property_name == properties->rssi.name() ||
      property_name == properties->tx_power.name() ||
      property_name == properties->service_data.name() ||
      property_name == properties->manufacturer_data.name() ||
      property_name == properties->advertising_data_flags.name()) {
    NotifyDeviceChanged(device_bluez);
  }

  if (property_name == properties->mtu.name())
    NotifyDeviceMTUChanged(device_bluez, properties->mtu.value());

  // Bluez does not currently provide an explicit signal for an advertisement
  // packet being received. Currently, it implicitly does so by notifying of an
  // RSSI change. We also listen for whether the EIR packet data has changed.
  if ((property_name == properties->rssi.name() ||
       property_name == properties->eir.name()) &&
      properties->rssi.is_valid() && properties->eir.is_valid()) {
    NotifyDeviceAdvertisementReceived(device_bluez, properties->rssi.value(),
                                      properties->eir.value());
  }

  if (property_name == properties->connected.name())
    NotifyDeviceConnectedStateChanged(device_bluez,
                                      properties->connected.value());

  if (property_name == properties->services_resolved.name() &&
      properties->services_resolved.value()) {
    device_bluez->UpdateGattServices(object_path);
    NotifyGattServicesDiscovered(device_bluez);
  }

  // When a device becomes paired, mark it as trusted so that the user does
  // not need to approve every incoming connection
  if (property_name == properties->paired.name()) {
    if (properties->paired.value() && !properties->trusted.value()) {
      device_bluez->SetTrusted();
    }
    NotifyDevicePairedChanged(device_bluez, properties->paired.value());
  }

  // UMA connection counting
  if (property_name == properties->connected.name()) {
    int count = 0;

    for (auto iter = devices_.begin(); iter != devices_.end(); ++iter) {
      if (iter->second->IsPaired() && iter->second->IsConnected())
        ++count;
    }

    UMA_HISTOGRAM_COUNTS_100("Bluetooth.ConnectedDeviceCount", count);
  }
}

void BluetoothAdapterBlueZ::InputPropertyChanged(
    const dbus::ObjectPath& object_path,
    const std::string& property_name) {
  BluetoothDeviceBlueZ* device_bluez = GetDeviceWithPath(object_path);
  if (!device_bluez)
    return;

  bluez::BluetoothInputClient::Properties* properties =
      bluez::BluezDBusManager::Get()->GetBluetoothInputClient()->GetProperties(
          object_path);

  // Properties structure can be removed, which triggers a change in the
  // BluetoothDevice::IsConnectable() property, as does a change in the
  // actual reconnect_mode property.
  if (!properties || property_name == properties->reconnect_mode.name()) {
    NotifyDeviceChanged(device_bluez);
  }
}

void BluetoothAdapterBlueZ::AgentManagerAdded(
    const dbus::ObjectPath& object_path) {
  BLUETOOTH_LOG(DEBUG) << "Registering pairing agent";
  bluez::BluezDBusManager::Get()
      ->GetBluetoothAgentManagerClient()
      ->RegisterAgent(
          dbus::ObjectPath(kAgentPath),
          bluetooth_agent_manager::kKeyboardDisplayCapability,
          base::BindOnce(&BluetoothAdapterBlueZ::OnRegisterAgent,
                         weak_ptr_factory_.GetWeakPtr()),
          base::BindOnce(&BluetoothAdapterBlueZ::OnRegisterAgentError,
                         weak_ptr_factory_.GetWeakPtr()));
}

void BluetoothAdapterBlueZ::AgentManagerRemoved(
    const dbus::ObjectPath& object_path) {}

void BluetoothAdapterBlueZ::Released() {
  BLUETOOTH_LOG(EVENT) << "Released";
  if (!IsPresent())
    return;
  DCHECK(agent_.get());

  // Called after we unregister the pairing agent, e.g. when changing I/O
  // capabilities. Nothing much to be done right now.
}

void BluetoothAdapterBlueZ::RequestPinCode(const dbus::ObjectPath& device_path,
                                           PinCodeCallback callback) {
  DCHECK(IsPresent());
  DCHECK(agent_.get());
  BLUETOOTH_LOG(EVENT) << device_path.value() << ": RequestPinCode";

  BluetoothPairingBlueZ* pairing = GetPairing(device_path);
  if (!pairing) {
    std::move(callback).Run(REJECTED, "");
    return;
  }

  pairing->RequestPinCode(std::move(callback));
}

void BluetoothAdapterBlueZ::DisplayPinCode(const dbus::ObjectPath& device_path,
                                           const std::string& pincode) {
  DCHECK(IsPresent());
  DCHECK(agent_.get());
  BLUETOOTH_LOG(EVENT) << device_path.value()
                       << ": DisplayPinCode: " << pincode;

  BluetoothPairingBlueZ* pairing = GetPairing(device_path);
  if (!pairing)
    return;

  pairing->DisplayPinCode(pincode);
}

void BluetoothAdapterBlueZ::RequestPasskey(const dbus::ObjectPath& device_path,
                                           PasskeyCallback callback) {
  DCHECK(IsPresent());
  DCHECK(agent_.get());
  BLUETOOTH_LOG(EVENT) << device_path.value() << ": RequestPasskey";

  BluetoothPairingBlueZ* pairing = GetPairing(device_path);
  if (!pairing) {
    std::move(callback).Run(REJECTED, 0);
    return;
  }

  pairing->RequestPasskey(std::move(callback));
}

void BluetoothAdapterBlueZ::DisplayPasskey(const dbus::ObjectPath& device_path,
                                           uint32_t passkey,
                                           uint16_t entered) {
  DCHECK(IsPresent());
  DCHECK(agent_.get());
  BLUETOOTH_LOG(EVENT) << device_path.value() << ": DisplayPasskey: " << passkey
                       << " (" << entered << " entered)";

  BluetoothPairingBlueZ* pairing = GetPairing(device_path);
  if (!pairing)
    return;

  if (entered == 0)
    pairing->DisplayPasskey(passkey);

  pairing->KeysEntered(entered);
}

void BluetoothAdapterBlueZ::RequestConfirmation(
    const dbus::ObjectPath& device_path,
    uint32_t passkey,
    ConfirmationCallback callback) {
  DCHECK(IsPresent());
  DCHECK(agent_.get());
  BLUETOOTH_LOG(EVENT) << device_path.value()
                       << ": RequestConfirmation: " << passkey;

  BluetoothPairingBlueZ* pairing = GetPairing(device_path);
  if (!pairing) {
    std::move(callback).Run(REJECTED);
    return;
  }

  pairing->RequestConfirmation(passkey, std::move(callback));
}

void BluetoothAdapterBlueZ::RequestAuthorization(
    const dbus::ObjectPath& device_path,
    ConfirmationCallback callback) {
  DCHECK(IsPresent());
  DCHECK(agent_.get());
  BLUETOOTH_LOG(EVENT) << device_path.value() << ": RequestAuthorization";

  BluetoothPairingBlueZ* pairing = GetPairing(device_path);
  if (!pairing) {
    std::move(callback).Run(REJECTED);
    return;
  }

  pairing->RequestAuthorization(std::move(callback));
}

void BluetoothAdapterBlueZ::AuthorizeService(
    const dbus::ObjectPath& device_path,
    const std::string& uuid,
    ConfirmationCallback callback) {
  DCHECK(IsPresent());
  DCHECK(agent_.get());
  BLUETOOTH_LOG(EVENT) << device_path.value() << ": AuthorizeService: " << uuid;

  BluetoothDeviceBlueZ* device_bluez = GetDeviceWithPath(device_path);
  if (!device_bluez) {
    std::move(callback).Run(CANCELLED);
    return;
  }

  // We always set paired devices to Trusted, so the only reason that this
  // method call would ever be called is in the case of a race condition where
  // our "Set('Trusted', true)" method call is still pending in the Bluetooth
  // daemon because it's busy handling the incoming connection.
  if (device_bluez->IsPaired()) {
    std::move(callback).Run(SUCCESS);
    return;
  }

  // TODO(keybuk): reject service authorizations when not paired, determine
  // whether this is acceptable long-term.
  BLUETOOTH_LOG(ERROR) << "Rejecting service connection from unpaired device "
                       << device_bluez->GetAddress() << " for UUID " << uuid;
  std::move(callback).Run(REJECTED);
}

void BluetoothAdapterBlueZ::Cancel() {
  DCHECK(IsPresent());
  DCHECK(agent_.get());
  BLUETOOTH_LOG(EVENT) << "Cancel";
}

void BluetoothAdapterBlueZ::OnRegisterAgent() {
  BLUETOOTH_LOG(EVENT)
      << "Pairing agent registered, requesting to be made default";

  bluez::BluezDBusManager::Get()
      ->GetBluetoothAgentManagerClient()
      ->RequestDefaultAgent(
          dbus::ObjectPath(kAgentPath),
          base::BindOnce(&BluetoothAdapterBlueZ::OnRequestDefaultAgent,
                         weak_ptr_factory_.GetWeakPtr()),
          base::BindOnce(&BluetoothAdapterBlueZ::OnRequestDefaultAgentError,
                         weak_ptr_factory_.GetWeakPtr()));
}

void BluetoothAdapterBlueZ::OnRegisterAgentError(
    const std::string& error_name,
    const std::string& error_message) {
  // Our agent being already registered isn't an error.
  if (error_name == bluetooth_agent_manager::kErrorAlreadyExists)
    return;

  BLUETOOTH_LOG(ERROR) << "Failed to register pairing agent: " << error_name
                       << ": " << error_message;
}

void BluetoothAdapterBlueZ::OnRequestDefaultAgent() {
  BLUETOOTH_LOG(EVENT) << "Pairing agent now default";
}

void BluetoothAdapterBlueZ::OnRequestDefaultAgentError(
    const std::string& error_name,
    const std::string& error_message) {
  BLUETOOTH_LOG(ERROR) << "Failed to make pairing agent default: " << error_name
                       << ": " << error_message;
}

void BluetoothAdapterBlueZ::CreateServiceRecord(
    const BluetoothServiceRecordBlueZ& record,
    const ServiceRecordCallback& callback,
    const ServiceRecordErrorCallback& error_callback) {
  bluez::BluezDBusManager::Get()
      ->GetBluetoothAdapterClient()
      ->CreateServiceRecord(
          object_path_, record, callback,
          base::Bind(&BluetoothAdapterBlueZ::ServiceRecordErrorConnector,
                     weak_ptr_factory_.GetWeakPtr(), error_callback));
}

void BluetoothAdapterBlueZ::RemoveServiceRecord(
    uint32_t handle,
    const base::Closure& callback,
    const ServiceRecordErrorCallback& error_callback) {
  bluez::BluezDBusManager::Get()
      ->GetBluetoothAdapterClient()
      ->RemoveServiceRecord(
          object_path_, handle, callback,
          base::Bind(&BluetoothAdapterBlueZ::ServiceRecordErrorConnector,
                     weak_ptr_factory_.GetWeakPtr(), error_callback));
}

BluetoothDeviceBlueZ* BluetoothAdapterBlueZ::GetDeviceWithPath(
    const dbus::ObjectPath& object_path) {
  if (!IsPresent())
    return nullptr;

  for (auto iter = devices_.begin(); iter != devices_.end(); ++iter) {
    BluetoothDeviceBlueZ* device_bluez =
        static_cast<BluetoothDeviceBlueZ*>(iter->second.get());
    if (device_bluez->object_path() == object_path)
      return device_bluez;
  }

  return nullptr;
}

BluetoothPairingBlueZ* BluetoothAdapterBlueZ::GetPairing(
    const dbus::ObjectPath& object_path) {
  DCHECK(IsPresent());
  BluetoothDeviceBlueZ* device_bluez = GetDeviceWithPath(object_path);
  if (!device_bluez) {
    BLUETOOTH_LOG(ERROR) << "Pairing Agent request for unknown device: "
                         << object_path.value();
    return nullptr;
  }

  BluetoothPairingBlueZ* pairing = device_bluez->GetPairing();
  if (pairing)
    return pairing;

  // The device doesn't have its own pairing context, so this is an incoming
  // pairing request that should use our best default delegate (if we have one).
  BluetoothDevice::PairingDelegate* pairing_delegate = DefaultPairingDelegate();
  if (!pairing_delegate)
    return nullptr;

  return device_bluez->BeginPairing(pairing_delegate);
}

void BluetoothAdapterBlueZ::SetAdapter(const dbus::ObjectPath& object_path) {
  DCHECK(!IsPresent());
  DCHECK(!dbus_is_shutdown_);
  object_path_ = object_path;

  BLUETOOTH_LOG(EVENT) << object_path_.value() << ": using adapter.";

#if defined(OS_CHROMEOS)
  SetStandardChromeOSAdapterName();
#endif

  bluez::BluetoothAdapterClient::Properties* properties =
      bluez::BluezDBusManager::Get()
          ->GetBluetoothAdapterClient()
          ->GetProperties(object_path_);

  PresentChanged(true);

  if (properties->powered.value())
    NotifyAdapterPoweredChanged(true);
  if (properties->discoverable.value())
    DiscoverableChanged(true);
  if (properties->discovering.value())
    DiscoveringChanged(true);

  std::vector<dbus::ObjectPath> device_paths =
      bluez::BluezDBusManager::Get()
          ->GetBluetoothDeviceClient()
          ->GetDevicesForAdapter(object_path_);

  for (auto iter = device_paths.begin(); iter != device_paths.end(); ++iter) {
    DeviceAdded(*iter);
  }
}

#if defined(OS_CHROMEOS)
void BluetoothAdapterBlueZ::SetStandardChromeOSAdapterName() {
  DCHECK(IsPresent());

  std::string alias;
  switch (chromeos::GetDeviceType()) {
    case chromeos::DeviceType::kChromebase:
      alias = "Chromebase";
      break;
    case chromeos::DeviceType::kChromebit:
      alias = "Chromebit";
      break;
    case chromeos::DeviceType::kChromebook:
      alias = "Chromebook";
      break;
    case chromeos::DeviceType::kChromebox:
      alias = "Chromebox";
      break;
    case chromeos::DeviceType::kUnknown:
      alias = "Chromebook";
      break;
  }
  // Take the lower 2 bytes of hashed Bluetooth address and combine it with the
  // device type to create a more identifiable device name.
  const std::string address = GetAddress();
  alias = base::StringPrintf("%s_%04X", alias.c_str(),
                             base::PersistentHash(address) & 0xFFFF);
  SetName(alias, base::DoNothing(), base::DoNothing());
}
#endif

void BluetoothAdapterBlueZ::RemoveAdapter() {
  DCHECK(IsPresent());
  BLUETOOTH_LOG(EVENT) << object_path_.value() << ": adapter removed.";

  bluez::BluetoothAdapterClient::Properties* properties =
      bluez::BluezDBusManager::Get()
          ->GetBluetoothAdapterClient()
          ->GetProperties(object_path_);

  object_path_ = dbus::ObjectPath("");

  if (properties->powered.value())
    NotifyAdapterPoweredChanged(false);
  if (properties->discoverable.value())
    DiscoverableChanged(false);

  // The properties->discovering.value() may not be up to date with the real
  // discovering state (BlueZ bug: http://crbug.com/822104).
  // When the adapter is removed, make sure to clear all discovery sessions no
  // matter what the current properties->discovering.value() is.
  // DiscoveringChanged() properly handles the case where there is no discovery
  // sessions currently.
  DiscoveringChanged(false);

  // Move all elements of the original devices list to a new list here,
  // leaving the original list empty so that when we send DeviceRemoved(),
  // GetDevices() returns no devices.
  DevicesMap devices_swapped;
  devices_swapped.swap(devices_);

  for (auto& iter : devices_swapped) {
    for (auto& observer : observers_)
      observer.DeviceRemoved(this, iter.second.get());
  }

  PresentChanged(false);
}

void BluetoothAdapterBlueZ::DiscoverableChanged(bool discoverable) {
  for (auto& observer : observers_)
    observer.AdapterDiscoverableChanged(this, discoverable);
}

void BluetoothAdapterBlueZ::DiscoveringChanged(bool discovering) {
  // If the adapter stopped discovery due to a reason other than a request by
  // us, reset the count to 0.
  BLUETOOTH_LOG(EVENT) << "Discovering changed: " << discovering;
  if (!discovering && NumScanningDiscoverySessions() > 0) {
    BLUETOOTH_LOG(DEBUG) << "Marking sessions as inactive.";
    MarkDiscoverySessionsAsInactive();
  }

  for (auto& observer : observers_)
    observer.AdapterDiscoveringChanged(this, discovering);
}

void BluetoothAdapterBlueZ::PresentChanged(bool present) {
#if defined(OS_CHROMEOS)
  if (present) {
    SetLongTermKeys();

    if (!set_long_term_keys_after_first_time_install_timer_.IsRunning()) {
      // The LTK list in field trials would not yet have been available if we
      // are in a first-time install situation. Schedule SetLongTermKeys() to be
      // called again in the future when the field trials will definitely be
      // available.
      set_long_term_keys_after_first_time_install_timer_.Start(
          FROM_HERE, kSetLongTermKeysAfterFirstTimeInstallDuration,
          base::Bind(&BluetoothAdapterBlueZ::SetLongTermKeys,
                     weak_ptr_factory_.GetWeakPtr()));
    }
  }
#endif

  for (auto& observer : observers_)
    observer.AdapterPresentChanged(this, present);
}

void BluetoothAdapterBlueZ::NotifyDeviceAddressChanged(
    BluetoothDeviceBlueZ* device,
    const std::string& old_address) {
  DCHECK(device->adapter_ == this);

  for (auto& observer : observers_)
    observer.DeviceAddressChanged(this, device, old_address);
}

void BluetoothAdapterBlueZ::NotifyDeviceMTUChanged(BluetoothDeviceBlueZ* device,
                                                   uint16_t mtu) {
  DCHECK(device->adapter_ == this);

  for (auto& observer : observers_)
    observer.DeviceMTUChanged(this, device, mtu);
}

void BluetoothAdapterBlueZ::NotifyDeviceAdvertisementReceived(
    BluetoothDeviceBlueZ* device,
    int16_t rssi,
    const std::vector<uint8_t>& eir) {
  DCHECK(device->adapter_ == this);

  for (auto& observer : observers_)
    observer.DeviceAdvertisementReceived(this, device, rssi, eir);

#if defined(OS_CHROMEOS)
  if (ble_scan_parser_.is_bound()) {
    ScanRecordCallback callback =
        base::BindOnce(&BluetoothAdapterBlueZ::OnAdvertisementReceived,
                       weak_ptr_factory_.GetWeakPtr(), device->GetAddress(),
                       device->GetName() ? *(device->GetName()) : std::string(),
                       rssi, device->GetAppearance());
    ble_scan_parser_->Parse(eir, std::move(callback));
  }
#endif  // defined(OS_CHROMEOS)
}

#if defined(OS_CHROMEOS)
void BluetoothAdapterBlueZ::OnAdvertisementReceived(std::string device_address,
                                                    std::string device_name,
                                                    uint8_t rssi,
                                                    uint16_t device_appearance,
                                                    ScanRecordPtr scan_record) {
  // Ignore the packet if it could not be parsed successfully.
  if (!scan_record)
    return;

  auto service_data_map = ConvertServiceDataMap(scan_record->service_data_map);
  auto manufacturer_data_map =
      ConvertManufacturerDataMap(scan_record->manufacturer_data_map);
  for (auto& observer : observers_) {
    observer.DeviceAdvertisementReceived(
        device_address, device_name, scan_record->advertisement_name, rssi,
        scan_record->tx_power, device_appearance, scan_record->service_uuids,
        service_data_map, manufacturer_data_map);
  }
}
#endif  // defined(OS_CHROMEOS)

void BluetoothAdapterBlueZ::NotifyDeviceConnectedStateChanged(
    BluetoothDeviceBlueZ* device,
    bool is_now_connected) {
  DCHECK_EQ(device->adapter_, this);
  DCHECK_EQ(device->IsConnected(), is_now_connected);

  for (auto& observer : observers_)
    observer.DeviceConnectedStateChanged(this, device, is_now_connected);
}

void BluetoothAdapterBlueZ::UseProfile(
    const BluetoothUUID& uuid,
    const dbus::ObjectPath& device_path,
    const bluez::BluetoothProfileManagerClient::Options& options,
    bluez::BluetoothProfileServiceProvider::Delegate* delegate,
    const ProfileRegisteredCallback& success_callback,
    const ErrorCompletionCallback& error_callback) {
  DCHECK(delegate);

  if (!IsPresent()) {
    BLUETOOTH_LOG(DEBUG) << "Adapter not present, erroring out";
    error_callback.Run("Adapter not present");
    return;
  }

  if (profiles_.find(uuid) != profiles_.end()) {
    // TODO(jamuraa) check that the options are the same and error when they are
    // not.
    SetProfileDelegate(uuid, device_path, delegate, success_callback,
                       error_callback);
    return;
  }

  if (profile_queues_.find(uuid) == profile_queues_.end()) {
    BluetoothAdapterProfileBlueZ::Register(
        uuid, options,
        base::Bind(&BluetoothAdapterBlueZ::OnRegisterProfile, this, uuid),
        base::Bind(&BluetoothAdapterBlueZ::OnRegisterProfileError, this, uuid));

    profile_queues_[uuid] = new std::vector<RegisterProfileCompletionPair>();
  }

  profile_queues_[uuid]->push_back(std::make_pair(
      base::Bind(&BluetoothAdapterBlueZ::SetProfileDelegate, this, uuid,
                 device_path, delegate, success_callback, error_callback),
      error_callback));
}

void BluetoothAdapterBlueZ::ReleaseProfile(
    const dbus::ObjectPath& device_path,
    BluetoothAdapterProfileBlueZ* profile) {
  BLUETOOTH_LOG(EVENT) << "Releasing Profile: "
                       << profile->uuid().canonical_value() << " from "
                       << device_path.value();
  BluetoothUUID uuid = profile->uuid();
  auto iter = profiles_.find(uuid);
  if (iter == profiles_.end()) {
    BLUETOOTH_LOG(ERROR) << "Profile not found for: " << uuid.canonical_value();
    return;
  }
  released_profiles_[uuid] = iter->second;
  profiles_.erase(iter);
  profile->RemoveDelegate(device_path,
                          base::Bind(&BluetoothAdapterBlueZ::RemoveProfile,
                                     weak_ptr_factory_.GetWeakPtr(), uuid));
}

void BluetoothAdapterBlueZ::RemoveProfile(const BluetoothUUID& uuid) {
  BLUETOOTH_LOG(EVENT) << "Remove Profile: " << uuid.canonical_value();

  auto iter = released_profiles_.find(uuid);
  if (iter == released_profiles_.end()) {
    BLUETOOTH_LOG(ERROR) << "Released Profile not found: "
                         << uuid.canonical_value();
    return;
  }
  delete iter->second;
  released_profiles_.erase(iter);
}

void BluetoothAdapterBlueZ::AddLocalGattService(
    std::unique_ptr<BluetoothLocalGattServiceBlueZ> service) {
  owned_gatt_services_[service->object_path()] = std::move(service);
}

void BluetoothAdapterBlueZ::RemoveLocalGattService(
    BluetoothLocalGattServiceBlueZ* service) {
  auto service_iter = owned_gatt_services_.find(service->object_path());
  if (service_iter == owned_gatt_services_.end()) {
    BLUETOOTH_LOG(ERROR) << "Trying to remove service: "
                         << service->object_path().value()
                         << " from adapter: " << object_path_.value()
                         << " that doesn't own it.";
    return;
  }

  if (registered_gatt_services_.count(service->object_path()) != 0) {
    registered_gatt_services_.erase(service->object_path());
    UpdateRegisteredApplication(true, base::DoNothing(), base::DoNothing());
  }

  owned_gatt_services_.erase(service_iter);
}

void BluetoothAdapterBlueZ::RegisterGattService(
    BluetoothLocalGattServiceBlueZ* service,
    const base::Closure& callback,
    device::BluetoothGattService::ErrorCallback error_callback) {
  if (registered_gatt_services_.count(service->object_path()) > 0) {
    BLUETOOTH_LOG(ERROR)
        << "Re-registering a service that is already registered!";
    std::move(error_callback)
        .Run(device::BluetoothGattService::GATT_ERROR_FAILED);
    return;
  }

  registered_gatt_services_[service->object_path()] = service;

  // Always assume that we were already registered. If we weren't registered
  // we'll just get an error back which we can ignore. Any other approach will
  // introduce a race since we will always have a period when we may have been
  // registered with BlueZ, but not know that the registration succeeded
  // because the callback hasn't come back yet.
  UpdateRegisteredApplication(true, callback, std::move(error_callback));
}

void BluetoothAdapterBlueZ::UnregisterGattService(
    BluetoothLocalGattServiceBlueZ* service,
    const base::Closure& callback,
    device::BluetoothGattService::ErrorCallback error_callback) {
  DCHECK(bluez::BluezDBusManager::Get());

  if (registered_gatt_services_.count(service->object_path()) == 0) {
    BLUETOOTH_LOG(ERROR)
        << "Unregistering a service that isn't registered! path: "
        << service->object_path().value();
    std::move(error_callback)
        .Run(device::BluetoothGattService::GATT_ERROR_FAILED);
    return;
  }

  registered_gatt_services_.erase(service->object_path());
  UpdateRegisteredApplication(false, callback, std::move(error_callback));
}

bool BluetoothAdapterBlueZ::IsGattServiceRegistered(
    BluetoothLocalGattServiceBlueZ* service) {
  return registered_gatt_services_.count(service->object_path()) != 0;
}

bool BluetoothAdapterBlueZ::SendValueChanged(
    BluetoothLocalGattCharacteristicBlueZ* characteristic,
    const std::vector<uint8_t>& value) {
  if (registered_gatt_services_.count(
          static_cast<BluetoothLocalGattServiceBlueZ*>(
              characteristic->GetService())
              ->object_path()) == 0)
    return false;
  gatt_application_provider_->SendValueChanged(characteristic->object_path(),
                                               value);
  return true;
}

dbus::ObjectPath BluetoothAdapterBlueZ::GetApplicationObjectPath() const {
  return dbus::ObjectPath(object_path_.value() + kGattApplicationObjectPath);
}

void BluetoothAdapterBlueZ::OnRegisterProfile(
    const BluetoothUUID& uuid,
    std::unique_ptr<BluetoothAdapterProfileBlueZ> profile) {
  profiles_[uuid] = profile.release();

  if (profile_queues_.find(uuid) == profile_queues_.end())
    return;

  for (auto& it : *profile_queues_[uuid])
    it.first.Run();
  delete profile_queues_[uuid];
  profile_queues_.erase(uuid);
}

void BluetoothAdapterBlueZ::SetProfileDelegate(
    const BluetoothUUID& uuid,
    const dbus::ObjectPath& device_path,
    bluez::BluetoothProfileServiceProvider::Delegate* delegate,
    const ProfileRegisteredCallback& success_callback,
    const ErrorCompletionCallback& error_callback) {
  if (profiles_.find(uuid) == profiles_.end()) {
    error_callback.Run("Cannot find profile!");
    return;
  }

  if (profiles_[uuid]->SetDelegate(device_path, delegate)) {
    success_callback.Run(profiles_[uuid]);
    return;
  }
  // Already set
  error_callback.Run(bluetooth_agent_manager::kErrorAlreadyExists);
}

void BluetoothAdapterBlueZ::OnRegisterProfileError(
    const BluetoothUUID& uuid,
    const std::string& error_name,
    const std::string& error_message) {
  BLUETOOTH_LOG(ERROR) << object_path_.value()
                       << ": Failed to register profile: " << error_name << ": "
                       << error_message;
  if (profile_queues_.find(uuid) == profile_queues_.end())
    return;

  for (auto& it : *profile_queues_[uuid])
    it.second.Run(error_message);

  delete profile_queues_[uuid];
  profile_queues_.erase(uuid);
}

void BluetoothAdapterBlueZ::OnSetDiscoverable(
    const base::Closure& callback,
    const ErrorCallback& error_callback,
    bool success) {
  if (!IsPresent()) {
    error_callback.Run();
    return;
  }

  // Set the discoverable_timeout property to zero so the adapter remains
  // discoverable forever.
  bluez::BluezDBusManager::Get()
      ->GetBluetoothAdapterClient()
      ->GetProperties(object_path_)
      ->discoverable_timeout.Set(
          0,
          base::Bind(&BluetoothAdapterBlueZ::OnPropertyChangeCompleted,
                     weak_ptr_factory_.GetWeakPtr(), callback, error_callback));
}

void BluetoothAdapterBlueZ::OnPropertyChangeCompleted(
    const base::Closure& callback,
    const ErrorCallback& error_callback,
    bool success) {
  if (IsPresent() && success) {
    callback.Run();
  } else {
    error_callback.Run();
  }
}

base::WeakPtr<BluetoothAdapter> BluetoothAdapterBlueZ::GetWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

// BluetoothAdapterBlueZ should override SetPowered() instead.
bool BluetoothAdapterBlueZ::SetPoweredImpl(bool powered) {
  NOTREACHED();
  return false;
}

void BluetoothAdapterBlueZ::UpdateFilter(
    std::unique_ptr<device::BluetoothDiscoveryFilter> discovery_filter,
    DiscoverySessionResultCallback callback) {
  DCHECK_GT(NumDiscoverySessions(), 0);
  if (!IsPresent()) {
    std::move(callback).Run(
        true, UMABluetoothDiscoverySessionOutcome::ADAPTER_NOT_PRESENT);
    return;
  }

  BLUETOOTH_LOG(EVENT) << __func__;

  auto copyable_callback = base::AdaptCallbackForRepeating(std::move(callback));

  // DCHECK(IsDiscovering()) is removed due to BlueZ bug
  // (https://crbug.com/822104).
  // TODO(sonnysasaka): Put it back here when BlueZ bug is fixed.
  SetDiscoveryFilter(
      std::move(discovery_filter),
      base::BindRepeating(copyable_callback, /*is_error*/ false,
                          UMABluetoothDiscoverySessionOutcome::SUCCESS),
      base::BindOnce(copyable_callback, true));
  return;
}

void BluetoothAdapterBlueZ::StartScanWithFilter(
    std::unique_ptr<device::BluetoothDiscoveryFilter> discovery_filter,
    DiscoverySessionResultCallback callback) {
  if (!IsPresent()) {
    std::move(callback).Run(
        true, UMABluetoothDiscoverySessionOutcome::ADAPTER_NOT_PRESENT);
    return;
  }

  BLUETOOTH_LOG(EVENT) << __func__;

  auto copyable_callback = base::AdaptCallbackForRepeating(std::move(callback));

  if (discovery_filter && !discovery_filter->IsDefault()) {
    std::unique_ptr<BluetoothDiscoveryFilter> df(
        new BluetoothDiscoveryFilter(device::BLUETOOTH_TRANSPORT_DUAL));
    df->CopyFrom(*discovery_filter);
    SetDiscoveryFilter(
        std::move(df),
        base::BindRepeating(
            &BluetoothAdapterBlueZ::OnPreSetDiscoveryFilter,
            weak_ptr_factory_.GetWeakPtr(),
            base::BindRepeating(copyable_callback, /*is_error=*/false,
                                UMABluetoothDiscoverySessionOutcome::SUCCESS),
            base::BindRepeating(copyable_callback, true)),
        base::BindOnce(
            &BluetoothAdapterBlueZ::OnPreSetDiscoveryFilterError,
            weak_ptr_factory_.GetWeakPtr(),
            base::BindRepeating(copyable_callback, /*is_error=*/false,
                                UMABluetoothDiscoverySessionOutcome::SUCCESS),
            base::BindOnce(copyable_callback, true)));
    return;
  }

  // This is the first request to start device discovery.
  bluez::BluezDBusManager::Get()->GetBluetoothAdapterClient()->StartDiscovery(
      object_path_,
      base::BindRepeating(
          &BluetoothAdapterBlueZ::OnStartDiscovery,
          weak_ptr_factory_.GetWeakPtr(),
          base::BindRepeating(copyable_callback, /*is_error=*/false,
                              UMABluetoothDiscoverySessionOutcome::SUCCESS),
          base::BindRepeating(copyable_callback, true)),
      base::BindOnce(
          &BluetoothAdapterBlueZ::OnStartDiscoveryError,
          weak_ptr_factory_.GetWeakPtr(),
          base::BindRepeating(copyable_callback, /*is_error=*/false,
                              UMABluetoothDiscoverySessionOutcome::SUCCESS),
          base::BindOnce(copyable_callback, true)));
}

void BluetoothAdapterBlueZ::StopScan(DiscoverySessionResultCallback callback) {
#if defined(OS_CHROMEOS)
  ble_scan_parser_.reset();
#endif  // defined(OS_CHROMEOS)

  // Not having an adapter qualifies as not scanning so we callback a success
  if (!IsPresent()) {
    std::move(callback).Run(
        /*is_error=*/false,
        UMABluetoothDiscoverySessionOutcome::ADAPTER_NOT_PRESENT);
    return;
  }

  BLUETOOTH_LOG(EVENT) << __func__;

  DCHECK_EQ(NumDiscoverySessions(), 0);

  // Confirm that there are no more discovery sessions left.
  DCHECK_EQ(NumDiscoverySessions(), 0);
  auto copyable_callback = base::AdaptCallbackForRepeating(std::move(callback));
  bluez::BluezDBusManager::Get()->GetBluetoothAdapterClient()->StopDiscovery(
      object_path_,
      base::BindRepeating(
          &BluetoothAdapterBlueZ::OnStopDiscovery,
          weak_ptr_factory_.GetWeakPtr(),
          base::BindRepeating(copyable_callback, /*is_error=*/false,
                              UMABluetoothDiscoverySessionOutcome::SUCCESS)),
      base::BindOnce(&BluetoothAdapterBlueZ::OnStopDiscoveryError,
                     weak_ptr_factory_.GetWeakPtr(),
                     base::BindOnce(copyable_callback, /*is_error=*/true)));
}

void BluetoothAdapterBlueZ::SetDiscoveryFilter(
    std::unique_ptr<BluetoothDiscoveryFilter> discovery_filter,
    const base::Closure& callback,
    DiscoverySessionErrorCallback error_callback) {
  if (!IsPresent()) {
    std::move(error_callback)
        .Run(UMABluetoothDiscoverySessionOutcome::ADAPTER_REMOVED);
    return;
  }

  bluez::BluetoothAdapterClient::DiscoveryFilter dbus_discovery_filter;

  if (discovery_filter.get() && !discovery_filter->IsDefault()) {
    uint16_t pathloss;
    int16_t rssi;
    uint8_t transport;
    std::set<device::BluetoothUUID> uuids;

    if (discovery_filter->GetPathloss(&pathloss))
      dbus_discovery_filter.pathloss.reset(new uint16_t(pathloss));

    if (discovery_filter->GetRSSI(&rssi))
      dbus_discovery_filter.rssi.reset(new int16_t(rssi));

    transport = discovery_filter->GetTransport();
    if (transport == device::BLUETOOTH_TRANSPORT_LE) {
      dbus_discovery_filter.transport.reset(new std::string("le"));
    } else if (transport == device::BLUETOOTH_TRANSPORT_CLASSIC) {
      dbus_discovery_filter.transport.reset(new std::string("bredr"));
    } else if (transport == device::BLUETOOTH_TRANSPORT_DUAL) {
      dbus_discovery_filter.transport.reset(new std::string("auto"));
    }

    discovery_filter->GetUUIDs(uuids);
    if (uuids.size()) {
      dbus_discovery_filter.uuids = std::unique_ptr<std::vector<std::string>>(
          new std::vector<std::string>);

      for (const auto& it : uuids)
        dbus_discovery_filter.uuids.get()->push_back(it.value());
    }
  }

  auto copyable_error_callback =
      base::AdaptCallbackForRepeating(std::move(error_callback));
  bluez::BluezDBusManager::Get()
      ->GetBluetoothAdapterClient()
      ->SetDiscoveryFilter(
          object_path_, dbus_discovery_filter,
          base::BindRepeating(&BluetoothAdapterBlueZ::OnSetDiscoveryFilter,
                              weak_ptr_factory_.GetWeakPtr(), callback,
                              copyable_error_callback),
          base::BindOnce(&BluetoothAdapterBlueZ::OnSetDiscoveryFilterError,
                         weak_ptr_factory_.GetWeakPtr(), callback,
                         copyable_error_callback));
}

void BluetoothAdapterBlueZ::OnStartDiscovery(
    const base::Closure& callback,
    DiscoverySessionErrorCallback error_callback) {
  // Report success on the original request and increment the count.
  BLUETOOTH_LOG(EVENT) << __func__;

#if defined(OS_CHROMEOS)
  device::BluetoothAdapterFactory::BleScanParserCallback
      ble_scan_parser_callback =
          device::BluetoothAdapterFactory::GetBleScanParserCallback();
  if (ble_scan_parser_callback) {
    // To avoid repeatedly restarting a crashed data decoder service,
    // don't add a connection error handler here. Wait to establish a
    // new connection after all discovery sessions are stopped.
    ble_scan_parser_.Bind(ble_scan_parser_callback.Run());
  } else {
#if DCHECK_IS_ON()
    static bool logged_once = false;
    DLOG_IF(ERROR, !logged_once)
        << "Attempted to connect to "
           "unconfigured BluetoothAdapterFactory::GetBleScanParserCallback()";
    logged_once = true;
#endif  // DCHECK_IS_ON()
  }
#endif  // defined(OS_CHROMEOS)

  if (IsPresent()) {
    callback.Run();
  } else {
    std::move(error_callback)
        .Run(UMABluetoothDiscoverySessionOutcome::ADAPTER_REMOVED);
  }
}

void BluetoothAdapterBlueZ::OnStartDiscoveryError(
    const base::Closure& callback,
    DiscoverySessionErrorCallback error_callback,
    const std::string& error_name,
    const std::string& error_message) {
  BLUETOOTH_LOG(ERROR) << object_path_.value()
                       << ": Failed to start discovery: " << error_name << ": "
                       << error_message;

  std::move(error_callback).Run(TranslateDiscoveryErrorToUMA(error_name));
}

void BluetoothAdapterBlueZ::OnStopDiscovery(const base::Closure& callback) {
  // Report success on the original request and decrement the count.
  BLUETOOTH_LOG(EVENT) << __func__;
  DCHECK_GE(NumDiscoverySessions(), 0);
  callback.Run();
}

void BluetoothAdapterBlueZ::OnStopDiscoveryError(
    DiscoverySessionErrorCallback error_callback,
    const std::string& error_name,
    const std::string& error_message) {
  BLUETOOTH_LOG(ERROR) << object_path_.value()
                       << ": Failed to stop discovery: " << error_name << ": "
                       << error_message;

  std::move(error_callback).Run(TranslateDiscoveryErrorToUMA(error_name));
}

void BluetoothAdapterBlueZ::OnPreSetDiscoveryFilter(
    const base::Closure& callback,
    DiscoverySessionErrorCallback error_callback) {

  auto copyable_error_callback =
      base::AdaptCallbackForRepeating(std::move(error_callback));
  bluez::BluezDBusManager::Get()->GetBluetoothAdapterClient()->StartDiscovery(
      object_path_,
      base::BindRepeating(&BluetoothAdapterBlueZ::OnStartDiscovery,
                          weak_ptr_factory_.GetWeakPtr(), callback,
                          copyable_error_callback),
      base::BindOnce(&BluetoothAdapterBlueZ::OnStartDiscoveryError,
                     weak_ptr_factory_.GetWeakPtr(), callback,
                     copyable_error_callback));
}

void BluetoothAdapterBlueZ::OnPreSetDiscoveryFilterError(
    const base::Closure& callback,
    DiscoverySessionErrorCallback error_callback,
    UMABluetoothDiscoverySessionOutcome outcome) {
  BLUETOOTH_LOG(ERROR) << object_path_.value()
                       << ": Failed to pre set discovery filter.";

  std::move(error_callback).Run(outcome);
}

void BluetoothAdapterBlueZ::OnSetDiscoveryFilter(
    const base::Closure& callback,
    DiscoverySessionErrorCallback error_callback) {
  // Report success on the original request and increment the count.
  BLUETOOTH_LOG(EVENT) << __func__;
  if (IsPresent()) {
    callback.Run();
  } else {
    std::move(error_callback)
        .Run(UMABluetoothDiscoverySessionOutcome::ADAPTER_REMOVED);
  }
}

void BluetoothAdapterBlueZ::OnSetDiscoveryFilterError(
    const base::Closure& callback,
    DiscoverySessionErrorCallback error_callback,
    const std::string& error_name,
    const std::string& error_message) {
  BLUETOOTH_LOG(ERROR) << object_path_.value()
                       << ": Failed to set discovery filter: " << error_name
                       << ": " << error_message;

  UMABluetoothDiscoverySessionOutcome outcome =
      TranslateDiscoveryErrorToUMA(error_name);
  if (outcome == UMABluetoothDiscoverySessionOutcome::FAILED) {
    // bluez/doc/adapter-api.txt says "Failed" is returned from
    // SetDiscoveryFilter when the controller doesn't support the requested
    // transport.
    outcome = UMABluetoothDiscoverySessionOutcome::
        BLUEZ_DBUS_FAILED_MAYBE_UNSUPPORTED_TRANSPORT;
  }
  std::move(error_callback).Run(outcome);
}

void BluetoothAdapterBlueZ::UpdateRegisteredApplication(
    bool ignore_unregister_failure,
    const base::Closure& callback,
    device::BluetoothGattService::ErrorCallback error_callback) {
  // If ignore_unregister_failure is set, we'll forward the error_callback to
  // the register call (to be called in case the register call fails). If not,
  // we'll call the error callback if this unregister itself fails.
  auto adapted_error_callback =
      base::AdaptCallbackForRepeating(std::move(error_callback));
  bluez::BluezDBusManager::Get()
      ->GetBluetoothGattManagerClient()
      ->UnregisterApplication(
          object_path_, GetApplicationObjectPath(),
          base::Bind(&BluetoothAdapterBlueZ::RegisterApplication,
                     weak_ptr_factory_.GetWeakPtr(), callback,
                     adapted_error_callback),
          ignore_unregister_failure
              ? base::Bind(&BluetoothAdapterBlueZ::RegisterApplicationOnError,
                           weak_ptr_factory_.GetWeakPtr(), callback,
                           adapted_error_callback)
              : base::Bind(&OnRegistrationErrorCallback, adapted_error_callback,
                           false));
}

void BluetoothAdapterBlueZ::RegisterApplication(
    const base::Closure& callback,
    device::BluetoothGattService::ErrorCallback error_callback) {
  // Recreate our application service provider with the currently registered
  // GATT services before we register it.
  gatt_application_provider_.reset();
  // If we have no services registered, then leave the application unregistered
  // and no application provider.
  if (registered_gatt_services_.size() == 0) {
    callback.Run();
    return;
  }
  gatt_application_provider_ = BluetoothGattApplicationServiceProvider::Create(
      bluez::BluezDBusManager::Get()->GetSystemBus(),
      GetApplicationObjectPath(), registered_gatt_services_);

  DCHECK(bluez::BluezDBusManager::Get());
  bluez::BluezDBusManager::Get()
      ->GetBluetoothGattManagerClient()
      ->RegisterApplication(object_path_, GetApplicationObjectPath(),
                            BluetoothGattManagerClient::Options(), callback,
                            base::BindOnce(&OnRegistrationErrorCallback,
                                           std::move(error_callback),
                                           /*is_register_callback=*/true));
}

void BluetoothAdapterBlueZ::RegisterApplicationOnError(
    const base::Closure& callback,
    device::BluetoothGattService::ErrorCallback error_callback,
    const std::string& /* error_name */,
    const std::string& /* error_message */) {
  RegisterApplication(callback, std::move(error_callback));
}

void BluetoothAdapterBlueZ::ServiceRecordErrorConnector(
    const ServiceRecordErrorCallback& error_callback,
    const std::string& error_name,
    const std::string& error_message) {
  BLUETOOTH_LOG(EVENT) << "Creating service record failed: error: "
                       << error_name << " - " << error_message;

  BluetoothServiceRecordBlueZ::ErrorCode code =
      BluetoothServiceRecordBlueZ::ErrorCode::UNKNOWN;
  if (error_name == bluetooth_adapter::kErrorInvalidArguments) {
    code = BluetoothServiceRecordBlueZ::ErrorCode::ERROR_INVALID_ARGUMENTS;
  } else if (error_name == bluetooth_adapter::kErrorDoesNotExist) {
    code = BluetoothServiceRecordBlueZ::ErrorCode::ERROR_RECORD_DOES_NOT_EXIST;
  } else if (error_name == bluetooth_adapter::kErrorAlreadyExists) {
    code = BluetoothServiceRecordBlueZ::ErrorCode::ERROR_RECORD_ALREADY_EXISTS;
  } else if (error_name == bluetooth_adapter::kErrorNotReady) {
    code = BluetoothServiceRecordBlueZ::ErrorCode::ERROR_ADAPTER_NOT_READY;
  }

  error_callback.Run(code);
}

#if defined(OS_CHROMEOS)
void BluetoothAdapterBlueZ::SetLongTermKeys() {
  // This method will be called and also queued up again when the adapter
  // becomes present. See PresentChanged().
  if (!IsPresent())
    return;

  bluez::BluezDBusManager::Get()->GetBluetoothAdapterClient()->SetLongTermKeys(
      object_path_, device::GetBlockedLongTermKeys(),
      base::Bind(&BluetoothAdapterBlueZ::SetLongTermKeysError,
                 weak_ptr_factory_.GetWeakPtr()));
}

void BluetoothAdapterBlueZ::SetLongTermKeysError(
    const std::string& error_name,
    const std::string& error_message) {
  BLUETOOTH_LOG(ERROR) << "Setting long term keys failed: error: " << error_name
                       << " - " << error_message;
}
#endif

}  // namespace bluez
