// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/media/router/providers/cast/cast_activity_record.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "base/bind.h"
#include "base/optional.h"
#include "base/stl_util.h"
#include "chrome/browser/media/router/providers/cast/cast_activity_manager.h"
#include "chrome/browser/media/router/providers/cast/cast_session_client.h"
#include "url/origin.h"

using blink::mojom::PresentationConnectionCloseReason;
using blink::mojom::PresentationConnectionMessagePtr;

namespace media_router {

CastActivityRecord::CastActivityRecord(
    const MediaRoute& route,
    const std::string& app_id,
    cast_channel::CastMessageHandler* message_handler,
    CastSessionTracker* session_tracker)
    : ActivityRecord(route, app_id, message_handler, session_tracker) {
  route_.set_controller_type(RouteControllerType::kGeneric);
}

CastActivityRecord::~CastActivityRecord() = default;

mojom::RoutePresentationConnectionPtr CastActivityRecord::AddClient(
    const CastMediaSource& source,
    const url::Origin& origin,
    int tab_id) {
  const std::string& client_id = source.client_id();
  DCHECK(!base::Contains(connected_clients_, client_id));
  std::unique_ptr<CastSessionClient> client =
      client_factory_for_test_
          ? client_factory_for_test_->MakeClientForTest(client_id, origin,
                                                        tab_id)
          : std::make_unique<CastSessionClientImpl>(
                client_id, origin, tab_id, source.auto_join_policy(), this);
  auto presentation_connection = client->Init();
  connected_clients_.emplace(client_id, std::move(client));

  // Route is now local due to connected client.
  route_.set_local(true);
  return presentation_connection;
}

void CastActivityRecord::RemoveClient(const std::string& client_id) {
  // Don't erase by key here as the |client_id| may be referring to the
  // client being deleted.
  auto it = connected_clients_.find(client_id);
  if (it != connected_clients_.end())
    connected_clients_.erase(it);
}

void CastActivityRecord::SetOrUpdateSession(const CastSession& session,
                                            const MediaSinkInternal& sink,
                                            const std::string& hash_token) {
  bool had_session_id = session_id_.has_value();
  ActivityRecord::SetOrUpdateSession(session, sink, hash_token);
  if (had_session_id) {
    for (auto& client : connected_clients_)
      client.second->SendMessageToClient(
          CreateUpdateSessionMessage(session, client.first, sink, hash_token));
  }
  if (media_controller_)
    media_controller_->SetSession(session);
}

cast_channel::Result CastActivityRecord::SendAppMessageToReceiver(
    const CastInternalMessage& cast_message) {
  CastSessionClient* client = GetClient(cast_message.client_id());
  const CastSession* session = GetSession();
  if (!session) {
    if (client && cast_message.sequence_number()) {
      client->SendErrorCodeToClient(
          *cast_message.sequence_number(),
          CastInternalMessage::ErrorCode::kSessionError,
          "Invalid session ID: " + session_id_.value_or("<missing>"));
    }

    return cast_channel::Result::kFailed;
  }
  const std::string& message_namespace = cast_message.app_message_namespace();
  if (!base::Contains(session->message_namespaces(), message_namespace)) {
    DLOG(ERROR) << "Disallowed message namespace: " << message_namespace;
    if (client && cast_message.sequence_number()) {
      client->SendErrorCodeToClient(
          *cast_message.sequence_number(),
          CastInternalMessage::ErrorCode::kInvalidParameter,
          "Invalid namespace: " + message_namespace);
    }
    return cast_channel::Result::kFailed;
  }
  return message_handler_->SendAppMessage(
      cast_channel_id(),
      cast_channel::CreateCastMessage(
          message_namespace, cast_message.app_message_body(),
          cast_message.client_id(), session->transport_id()));
}

base::Optional<int> CastActivityRecord::SendMediaRequestToReceiver(
    const CastInternalMessage& cast_message) {
  CastSession* session = GetSession();
  if (!session)
    return base::nullopt;
  return message_handler_->SendMediaRequest(
      cast_channel_id(), cast_message.v2_message_body(),
      cast_message.client_id(), session->transport_id());
}

void CastActivityRecord::SendSetVolumeRequestToReceiver(
    const CastInternalMessage& cast_message,
    cast_channel::ResultCallback callback) {
  message_handler_->SendSetVolumeRequest(
      cast_channel_id(), cast_message.v2_message_body(),
      cast_message.client_id(), std::move(callback));
}

void CastActivityRecord::SendStopSessionMessageToClients(
    const std::string& hash_token) {
  // TODO(jrw): Add test for this method.
  for (const auto& client : connected_clients_) {
    client.second->SendMessageToClient(
        CreateReceiverActionStopMessage(client.first, sink_, hash_token));
  }
}

void CastActivityRecord::HandleLeaveSession(const std::string& client_id) {
  auto client_it = connected_clients_.find(client_id);
  CHECK(client_it != connected_clients_.end());
  auto& client = *client_it->second;
  std::vector<std::string> leaving_client_ids;
  for (const auto& pair : connected_clients_) {
    if (pair.second->MatchesAutoJoinPolicy(client.origin(), client.tab_id()))
      leaving_client_ids.push_back(pair.first);
  }

  for (const auto& client_id : leaving_client_ids) {
    auto leaving_client_it = connected_clients_.find(client_id);
    CHECK(leaving_client_it != connected_clients_.end());
    leaving_client_it->second->CloseConnection(
        PresentationConnectionCloseReason::CLOSED);
    connected_clients_.erase(leaving_client_it);
  }
}

void CastActivityRecord::SendMessageToClient(
    const std::string& client_id,
    PresentationConnectionMessagePtr message) {
  auto it = connected_clients_.find(client_id);
  if (it == connected_clients_.end()) {
    DLOG(ERROR) << "Attempting to send message to nonexistent client: "
                << client_id;
    return;
  }
  it->second->SendMessageToClient(std::move(message));
}

void CastActivityRecord::SendMediaStatusToClients(
    const base::Value& media_status,
    base::Optional<int> request_id) {
  for (auto& client : connected_clients_)
    client.second->SendMediaStatusToClient(media_status, request_id);
  if (media_controller_)
    media_controller_->SetMediaStatus(media_status);
}

void CastActivityRecord::ClosePresentationConnections(
    PresentationConnectionCloseReason close_reason) {
  for (auto& client : connected_clients_)
    client.second->CloseConnection(close_reason);
}

void CastActivityRecord::TerminatePresentationConnections() {
  for (auto& client : connected_clients_)
    client.second->TerminateConnection();
}

void CastActivityRecord::CreateMediaController(
    mojo::PendingReceiver<mojom::MediaController> media_controller,
    mojo::PendingRemote<mojom::MediaStatusObserver> observer) {
  media_controller_ = std::make_unique<CastMediaController>(
      this, std::move(media_controller), std::move(observer));

  if (session_id_) {
    CastSession* session = GetSession();
    if (session) {
      media_controller_->SetSession(*session);
      base::Value status_request(base::Value::Type::DICTIONARY);
      status_request.SetKey("type", base::Value("MEDIA_GET_STATUS"));
      message_handler_->SendMediaRequest(cast_channel_id(), status_request,
                                         media_controller_->sender_id(),
                                         session->transport_id());
    }
  }
}

void CastActivityRecord::OnAppMessage(
    const cast::channel::CastMessage& message) {
  if (!session_id_) {
    DVLOG(2) << "No session associated with activity!";
    return;
  }

  const std::string& client_id = message.destination_id();
  if (client_id == "*") {
    for (const auto& client : connected_clients_) {
      SendMessageToClient(
          client.first, CreateAppMessage(*session_id_, client.first, message));
    }
  } else {
    SendMessageToClient(client_id,
                        CreateAppMessage(*session_id_, client_id, message));
  }
}

void CastActivityRecord::OnInternalMessage(
    const cast_channel::InternalMessage& message) {}

bool CastActivityRecord::CanJoinSession(const CastMediaSource& cast_source,
                                        bool incognito) const {
  if (!cast_source.ContainsApp(app_id()))
    return false;

  if (base::Contains(connected_clients_, cast_source.client_id()))
    return false;

  if (route().is_incognito() != incognito)
    return false;

  return true;
}

bool CastActivityRecord::HasJoinableClient(AutoJoinPolicy policy,
                                           const url::Origin& origin,
                                           int tab_id) const {
  return std::any_of(connected_clients_.begin(), connected_clients_.end(),
                     [policy, &origin, tab_id](const auto& client) {
                       return IsAutoJoinAllowed(policy, origin, tab_id,
                                                client.second->origin(),
                                                client.second->tab_id());
                     });
}

CastSessionClientFactoryForTest* CastActivityRecord::client_factory_for_test_ =
    nullptr;

}  // namespace media_router
