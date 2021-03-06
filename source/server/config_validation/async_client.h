#pragma once

#include <chrono>

#include "envoy/http/async_client.h"
#include "envoy/http/message.h"

namespace Envoy {
namespace Http {

/**
 * Config-validation-only implementation of AsyncClient. Both methods on AsyncClient are allowed to
 * return nullptr if the request can't be created, so that's what the ValidationAsyncClient does in
 * all cases.
 */
class ValidationAsyncClient : public AsyncClient {
public:
  // Http::AsyncClient
  AsyncClient::Request* send(MessagePtr&&, Callbacks&,
                             const Optional<std::chrono::milliseconds>&) override;
  AsyncClient::Stream* start(StreamCallbacks&, const Optional<std::chrono::milliseconds>&) override;
};

} // namespace Http
} // namespace Envoy
