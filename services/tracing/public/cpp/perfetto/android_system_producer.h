// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_TRACING_PUBLIC_CPP_PERFETTO_ANDROID_SYSTEM_PRODUCER_H_
#define SERVICES_TRACING_PUBLIC_CPP_PERFETTO_ANDROID_SYSTEM_PRODUCER_H_

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/atomicops.h"
#include "base/component_export.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "services/tracing/public/cpp/perfetto/system_producer.h"
#include "services/tracing/public/cpp/perfetto/task_runner.h"

namespace perfetto {
class SharedMemoryArbiter;
class SharedMemory;
}  // namespace perfetto

namespace tracing {

class COMPONENT_EXPORT(TRACING_CPP) AndroidSystemProducer
    : public SystemProducer {
 public:
  enum class State {
    kUninitialized = 0,
    kConnecting = 1,
    kConnected = 2,
    kDisconnected = 3
  };
  AndroidSystemProducer(const char* socket, PerfettoTaskRunner* task_runner);
  ~AndroidSystemProducer() override;

  // Functions needed for AndroidSystemProducer only.
  //
  // Lets tests ignore the SDK check (Perfetto only runs on post Android Pie
  // devices by default, so for trybots on older OSs we need to ignore the check
  // for our test system service).
  //
  // TODO(nuskos): We need to make this possible for telemetry as well, since
  // they might have side loaded the app.
  void SetDisallowPreAndroidPieForTesting(bool disallow);
  // |socket| must remain alive as long as AndroidSystemProducer is around
  // trying to connect to it.
  void SetNewSocketForTesting(const char* socket);

  // PerfettoProducer implementation.
  bool IsTracingActive() override;
  void NewDataSourceAdded(
      const PerfettoTracedProcess::DataSourceBase* const data_source) override;

  // SystemProducer implementation.
  //
  // When Chrome's tracing service wants to trace it always takes priority over
  // the system Perfetto service. To cleanly shut down and let the system
  // Perfetto know we are no longer participating we unregister the data
  // sources. Once all the data sources are stopped |on_disconnect_complete| is
  // called. Afterwards we will periodically check to see if the local trace has
  // finished and then re-register everything to the system Perfetto service.
  // Which might mean we rejoin the same trace if it is still ongoing or future
  // traces.
  void DisconnectWithReply(
      base::OnceClosure on_disconnect_complete = base::OnceClosure()) override;
  // IMPORTANT!! this only resets |sequence_checker_| owned by this class, if
  // this SystemProducer is connected to a service through the unix socket there
  // will be additional sequence checkers which will fail if the socket
  // disconnects.
  void ResetSequenceForTesting() override;

  // perfetto::Producer implementation.
  // Used by the service to start and stop traces.
  void OnConnect() override;
  void OnDisconnect() override;
  void OnTracingSetup() override;
  void SetupDataSource(perfetto::DataSourceInstanceID,
                       const perfetto::DataSourceConfig&) override;
  void StartDataSource(perfetto::DataSourceInstanceID,
                       const perfetto::DataSourceConfig&) override;
  void StopDataSource(perfetto::DataSourceInstanceID) override;
  void Flush(perfetto::FlushRequestID,
             const perfetto::DataSourceInstanceID* data_source_ids,
             size_t num_data_sources) override;
  void ClearIncrementalState(
      const perfetto::DataSourceInstanceID* data_source_ids,
      size_t num_data_sources) override;

  // perfetto::TracingService::ProducerEndpoint implementation.

  // Used by the TraceWriters to signal Perfetto that shared memory chunks are
  // ready for consumption.
  void CommitData(const perfetto::CommitDataRequest& commit,
                  CommitDataCallback callback) override;

  // Used by the DataSource implementations to create TraceWriters
  // for writing their protobufs, and respond to flushes.
  perfetto::SharedMemory* shared_memory() const override;
  void NotifyFlushComplete(perfetto::FlushRequestID) override;
  void RegisterTraceWriter(uint32_t writer_id, uint32_t target_buffer) override;
  void UnregisterTraceWriter(uint32_t writer_id) override;

  // Used by PerfettoTracedProcess to create trace writers and send triggers.
  perfetto::SharedMemoryArbiter* MaybeSharedMemoryArbiter() override;
  void ActivateTriggers(const std::vector<std::string>&) override;

  // The rest of these perfetto::TracingService::ProducerEndpoint functions are
  // not currently used.
  void RegisterDataSource(const perfetto::DataSourceDescriptor&) override;
  void UnregisterDataSource(const std::string& name) override;
  void NotifyDataSourceStarted(perfetto::DataSourceInstanceID) override;
  void NotifyDataSourceStopped(perfetto::DataSourceInstanceID) override;
  size_t shared_buffer_page_size_kb() const override;
  bool IsShmemProvidedByProducer() const override;

 protected:
  // Given our current |state_| determine how to properly connect and set up our
  // connection to the service via the named fd socket provided in the
  // constructor. If we succeed OnConnect() will be called, if we fail
  // OnDisconnect() will be called.
  void Connect();

 private:
  // This sets |service_| by connecting over Perfetto's IPC connection.
  void ConnectSocket();
  // Returns true if we should skip setup because this Android device is Android
  // O or below.
  bool SkipIfPreAndroidPie() const;
  // If any OnDisconnect callbacks are stored, this will invoke them and delete
  // references to them must be called on the proper sequence.
  void InvokeStoredOnDisconnectCallbacks();
  // After a certain amount of backoff time we will attempt to Connect() or if
  // Chrome is already tracing we will wait awhile and attempt to Connect()
  // later.
  void DelayedReconnect();

  std::string socket_name_;
  uint32_t connection_backoff_ms_;
  uint64_t data_sources_tracing_ = 0;
  bool disallow_pre_android_pie = true;
  State state_ = State::kUninitialized;
  std::vector<base::OnceClosure> on_disconnect_callbacks_;

  // Connection to the Perfetto service and the shared memory that it provides.
  perfetto::SharedMemory* shared_memory_ = nullptr;
  std::unique_ptr<perfetto::SharedMemoryArbiter> shared_memory_arbiter_;
  std::unique_ptr<perfetto::TracingService::ProducerEndpoint> service_;
  // First value is the flush ID, the second is the number of
  // replies we're still waiting for.
  std::pair<uint64_t, size_t> pending_replies_for_latest_flush_;

  SEQUENCE_CHECKER(sequence_checker_);
  // NOTE: Weak pointers must be invalidated before all other member variables.
  // and thus must be the last member variable.
  base::WeakPtrFactory<AndroidSystemProducer> weak_ptr_factory_{this};
  DISALLOW_COPY_AND_ASSIGN(AndroidSystemProducer);
};

}  // namespace tracing

#endif  // SERVICES_TRACING_PUBLIC_CPP_PERFETTO_ANDROID_SYSTEM_PRODUCER_H_
