// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/flight/sql/server_session_middleware.h"
#include <boost/lexical_cast.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

using namespace std::string_literals;

namespace arrow {
namespace flight {
namespace sql {

/// \brief A factory for ServerSessionMiddleware, itself storing session data.
class ServerSessionMiddlewareFactory : public ServerMiddlewareFactory {
 protected:
  std::map<std::string, std::shared_ptr<FlightSqlSession>> session_store_;
  std::shared_mutex session_store_lock_;
  boost::uuids::random_generator uuid_generator_;

  std::vector<std::pair<std::string, std::string>> ParseCookieString(
      const std::string_view& s);

 public:
  Status StartCall(const CallInfo&, const CallHeaders& incoming_headers,
                   std::shared_ptr<ServerMiddleware>* middleware);

  /// \brief Get a new, empty session option map and its id key.
  std::shared_ptr<FlightSqlSession> GetNewSession(std::string* session_id);
};

class ServerSessionMiddlewareImpl : public ServerSessionMiddleware {
 protected:
  ServerSessionMiddlewareFactory* factory_;
  const CallHeaders& headers_;
  std::shared_ptr<FlightSqlSession> session_;
  std::string session_id_;
  const bool existing_session;

 public:
  ServerSessionMiddlewareImpl(ServerSessionMiddlewareFactory* factory,
      const CallHeaders& headers)
      : factory_(factory), headers_(headers), existing_session(false) {}

  ServerSessionMiddlewareImpl(
      ServerSessionMiddlewareFactory* factory, const CallHeaders& headers,
      std::shared_ptr<FlightSqlSession> session, std::string session_id)
      : factory_(factory),
        headers_(headers),
        session_(std::move(session)),
        session_id_(std::move(session_id)),
        existing_session(true) {}

  void SendingHeaders(AddCallHeaders* add_call_headers) override {
    if (!existing_session && session_) {
     add_call_headers->AddHeader(
         "set-cookie", static_cast<std::string>(kSessionCookieName) + "=" + session_id_);
    }
  }

  void CallCompleted(const Status&) override {}

  bool HasSession() const override { return static_cast<bool>(session_); }

  std::shared_ptr<FlightSqlSession> GetSession() override {
    if (!session_)
      session_ = factory_->GetNewSession(&session_id_);
    return session_;
  }

  const CallHeaders& GetCallHeaders() const override { return headers_; }
};

  std::vector<std::pair<std::string, std::string>> ServerSessionMiddlewareFactory::ParseCookieString(
      const std::string_view& s) {
    const std::string list_sep = "; ";
    const std::string pair_sep = "=";
    const size_t pair_sep_len = pair_sep.length();

    std::vector<std::pair<std::string, std::string>> result;

    size_t cur = 0;
    while (cur < s.length()) {
      const size_t end = s.find(list_sep, cur);
      size_t len;
      if (end == std::string::npos) {
        // No (further) list delimiters
        len = std::string::npos;
        cur = s.length();
      } else {
        len = end - cur;
        cur = end;
      }
      const std::string_view tok = s.substr(cur, len);

      const size_t val_pos = tok.find(pair_sep);
      if (val_pos == std::string::npos) {
        // The cookie header is somewhat malformed; ignore the key and continue parsing
        continue;
      }
      result.emplace_back(tok.substr(0, val_pos),
                          tok.substr(val_pos + pair_sep_len, std::string::npos));
    }

    return result;
  }

  Status ServerSessionMiddlewareFactory::StartCall(const CallInfo&, const CallHeaders& incoming_headers,
                   std::shared_ptr<ServerMiddleware>* middleware) {
    std::string session_id;

    const std::pair<CallHeaders::const_iterator, CallHeaders::const_iterator>&
        headers_it_pr = incoming_headers.equal_range("cookie");
    for (auto itr = headers_it_pr.first; itr != headers_it_pr.second; ++itr) {
      const std::string_view& cookie_header = itr->second;
      const std::vector<std::pair<std::string, std::string>> cookies =
          ParseCookieString(cookie_header);
      for (const std::pair<std::string, std::string>& cookie : cookies) {
        if (cookie.first == kSessionCookieName) {
          if (cookie.second.empty())
            return Status::Invalid("Empty ", kSessionCookieName, " cookie value.");
          session_id = std::move(cookie.second);
        }
      }
      if (!session_id.empty()) break;
    }

    if (session_id.empty()) {
      // No cookie was found
      *middleware = std::make_shared<ServerSessionMiddlewareImpl>(this, incoming_headers);
    } else {
      const std::shared_lock<std::shared_mutex> l(session_store_lock_);
      if (!session_store_.count(session_id)) {
        return Status::Invalid("Invalid or expired ", kSessionCookieName, " cookie.");
      } else {
        auto session = session_store_.at(session_id);
        *middleware = std::make_shared<ServerSessionMiddlewareImpl>(
          this, incoming_headers, std::move(session), session_id);
      }
    }

    return Status::OK();
  }

  /// \brief Get a new, empty session option map and its id key.
  std::shared_ptr<FlightSqlSession> ServerSessionMiddlewareFactory::GetNewSession(std::string* session_id) {
    std::string new_id = boost::lexical_cast<std::string>(uuid_generator_());
    *session_id = new_id;
    auto session = std::make_shared<FlightSqlSession>();

    const std::unique_lock<std::shared_mutex> l(session_store_lock_);
    session_store_[new_id] = session;

    return session;
  }


std::shared_ptr<ServerMiddlewareFactory> MakeServerSessionMiddlewareFactory() {
  return std::make_shared<ServerSessionMiddlewareFactory>();
}

::arrow::Result<std::optional<SessionOptionValue>> FlightSqlSession::GetSessionOption(
    const std::string& k) {
  const std::shared_lock<std::shared_mutex> l(map_lock_);
  if (map_.count(k)) {
    return map_.at(k);
  } else {
    return std::nullopt;
  }
}

void FlightSqlSession::SetSessionOption(const std::string& k,
                                        const SessionOptionValue& v) {
  const std::unique_lock<std::shared_mutex> l(map_lock_);
  map_[k] = v;
}

void FlightSqlSession::EraseSessionOption(const std::string& k) {
  const std::unique_lock<std::shared_mutex> l(map_lock_);
  map_.erase(k);
}

}  // namespace sql
}  // namespace flight
}  // namespace arrow
