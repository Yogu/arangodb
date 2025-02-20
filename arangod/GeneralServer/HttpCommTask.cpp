////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "HttpCommTask.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StringBuffer.h"
#include "Basics/asio_ns.h"
#include "Basics/dtrace-wrapper.h"
#include "Cluster/ServerState.h"
#include "GeneralServer/GeneralServer.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "GeneralServer/H2CommTask.h"
#include "GeneralServer/VstCommTask.h"
#include "Logger/LogContext.h"
#include "Logger/LogMacros.h"
#include "Rest/HttpRequest.h"
#include "Rest/HttpResponse.h"
#include "Statistics/ConnectionStatistics.h"
#include "Statistics/RequestStatistics.h"

#include <cstring>

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

namespace {
using namespace arangodb;
using namespace arangodb::rest;

rest::RequestType llhttpToRequestType(llhttp_t* p) {
  switch (p->method) {
    case HTTP_DELETE:
      return RequestType::DELETE_REQ;
    case HTTP_GET:
      return RequestType::GET;
    case HTTP_HEAD:
      return RequestType::HEAD;
    case HTTP_POST:
      return RequestType::POST;
    case HTTP_PUT:
      return RequestType::PUT;
    case HTTP_OPTIONS:
      return RequestType::OPTIONS;
    case HTTP_PATCH:
      return RequestType::PATCH;
    default:
      return RequestType::ILLEGAL;
  }
}
}  // namespace

template<SocketType T>
bool HttpCommTask<T>::transferEncodingContainsChunked(
    HttpCommTask<T>& commTask, std::string const& encoding) {
  if (basics::StringUtils::tolower(encoding).find("chunked") !=
      std::string::npos) {
    commTask.sendErrorResponse(
        rest::ResponseCode::NOT_IMPLEMENTED, rest::ContentType::UNSET, 1,
        TRI_ERROR_NOT_IMPLEMENTED,
        "Parsing for transfer-encoding of type chunked not implemented.");
    return true;
  }
  return false;
}

template<SocketType T>
int HttpCommTask<T>::on_message_began(llhttp_t* p) try {
  HttpCommTask<T>* me = static_cast<HttpCommTask<T>*>(p->data);
  me->_lastHeaderField.clear();
  me->_lastHeaderValue.clear();
  me->_origin.clear();
  me->_url.clear();
  me->_request =
      std::make_unique<HttpRequest>(me->_connectionInfo, /*messageId*/ 1);
  me->_response.reset();
  me->_lastHeaderWasValue = false;
  me->_shouldKeepAlive = false;
  me->_messageDone = false;

  // acquire a new statistics entry for the request
  me->acquireRequestStatistics(1UL).SET_READ_START(TRI_microtime());
  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
int HttpCommTask<T>::on_url(llhttp_t* p, const char* at, size_t len) try {
  HttpCommTask<T>* me = static_cast<HttpCommTask<T>*>(p->data);
  me->_request->setRequestType(llhttpToRequestType(p));
  if (me->_request->requestType() == RequestType::ILLEGAL) {
    me->sendSimpleResponse(rest::ResponseCode::METHOD_NOT_ALLOWED,
                           rest::ContentType::UNSET, 1, VPackBuffer<uint8_t>());
    return HPE_USER;
  }
  me->requestStatistics(1UL).SET_REQUEST_TYPE(me->_request->requestType());

  me->_url.append(at, len);
  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
int HttpCommTask<T>::on_status(llhttp_t* p, const char* at, size_t len) {
  // should not be used
  return HPE_OK;
}

template<SocketType T>
int HttpCommTask<T>::on_header_field(llhttp_t* p, const char* at,
                                     size_t len) try {
  HttpCommTask<T>* me = static_cast<HttpCommTask<T>*>(p->data);
  if (me->_lastHeaderWasValue) {
    me->_request->setHeader(std::move(me->_lastHeaderField),
                            std::move(me->_lastHeaderValue));
    me->_lastHeaderField.assign(at, len);
  } else {
    me->_lastHeaderField.append(at, len);
  }
  me->_lastHeaderWasValue = false;
  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
int HttpCommTask<T>::on_header_value(llhttp_t* p, const char* at,
                                     size_t len) try {
  HttpCommTask<T>* me = static_cast<HttpCommTask<T>*>(p->data);
  if (me->_lastHeaderWasValue) {
    me->_lastHeaderValue.append(at, len);
  } else {
    me->_lastHeaderValue.assign(at, len);
  }
  me->_lastHeaderWasValue = true;
  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
int HttpCommTask<T>::on_header_complete(llhttp_t* p) try {
  HttpCommTask<T>* me = static_cast<HttpCommTask<T>*>(p->data);
  me->_response.reset();
  if (!me->_lastHeaderField.empty()) {
    me->_request->setHeader(std::move(me->_lastHeaderField),
                            std::move(me->_lastHeaderValue));
  }

  bool found;
  std::string const& encoding =
      me->_request->header(StaticStrings::TransferEncoding, found);

  if (found) {
    if (transferEncodingContainsChunked(*me, encoding)) {
      return HPE_USER;
    }
  }

  if ((p->http_major != 1 || p->http_minor != 0) &&
      (p->http_major != 1 || p->http_minor != 1)) {
    me->sendSimpleResponse(rest::ResponseCode::HTTP_VERSION_NOT_SUPPORTED,
                           rest::ContentType::UNSET, 1, VPackBuffer<uint8_t>());
    return HPE_USER;
  }
  if (p->content_length > GeneralCommTask<T>::MaximalBodySize) {
    me->sendSimpleResponse(rest::ResponseCode::REQUEST_ENTITY_TOO_LARGE,
                           rest::ContentType::UNSET, 1, VPackBuffer<uint8_t>());
    return HPE_USER;
  }
  me->_shouldKeepAlive = llhttp_should_keep_alive(p);

  std::string const& expect =
      me->_request->header(StaticStrings::Expect, found);
  if (found && StringUtils::trim(expect) == "100-continue") {
    LOG_TOPIC("2b604", TRACE, arangodb::Logger::REQUESTS)
        << "received a 100-continue request";
    char const* response = "HTTP/1.1 100 Continue\r\n\r\n";
    auto buff = asio_ns::buffer(response, strlen(response));
    asio_ns::async_write(
        me->_protocol->socket, buff,
        [self = me->shared_from_this()](asio_ns::error_code const& ec,
                                        std::size_t) {
          if (ec) {
            static_cast<HttpCommTask<T>*>(self.get())->close(ec);
          }
        });
    return HPE_OK;
  }

  if (me->_request->requestType() == RequestType::HEAD) {
    // Assume that request/response has no body, proceed parsing next message
    return 1;  // 1 is defined by parser
  }
  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
int HttpCommTask<T>::on_body(llhttp_t* p, const char* at, size_t len) try {
  HttpCommTask<T>* me = static_cast<HttpCommTask<T>*>(p->data);
  me->_request->appendBody(at, len);
  return HPE_OK;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
int HttpCommTask<T>::on_message_complete(llhttp_t* p) try {
  HttpCommTask<T>* me = static_cast<HttpCommTask<T>*>(p->data);
  me->_request->parseUrl(me->_url.data(), me->_url.size());

  me->requestStatistics(1UL).SET_READ_END();
  me->_messageDone = true;

  return HPE_PAUSED;
} catch (...) {
  // the caller of this function is a C function, which doesn't know
  // exceptions. we must not let an exception escape from here.
  return HPE_INTERNAL;
}

template<SocketType T>
HttpCommTask<T>::HttpCommTask(GeneralServer& server, ConnectionInfo info,
                              std::shared_ptr<AsioSocket<T>> so)
    : GeneralCommTask<T>(server, std::move(info), std::move(so)),
      _lastHeaderWasValue(false),
      _shouldKeepAlive(false),
      _messageDone(false) {
  this->_connectionStatistics.SET_HTTP();

  // initialize http parsing code
  llhttp_settings_init(&_parserSettings);
  _parserSettings.on_message_begin = HttpCommTask<T>::on_message_began;
  _parserSettings.on_url = HttpCommTask<T>::on_url;
  _parserSettings.on_status = HttpCommTask<T>::on_status;
  _parserSettings.on_header_field = HttpCommTask<T>::on_header_field;
  _parserSettings.on_header_value = HttpCommTask<T>::on_header_value;
  _parserSettings.on_headers_complete = HttpCommTask<T>::on_header_complete;
  _parserSettings.on_body = HttpCommTask<T>::on_body;
  _parserSettings.on_message_complete = HttpCommTask<T>::on_message_complete;
  llhttp_init(&_parser, HTTP_REQUEST, &_parserSettings);
  _parser.data = this;

  this->_generalServerFeature.countHttp1Connection();
}

template<SocketType T>
HttpCommTask<T>::~HttpCommTask() noexcept = default;

template<SocketType T>
void HttpCommTask<T>::start() {
  LOG_TOPIC("358d4", DEBUG, Logger::REQUESTS)
      << "<http> opened connection \"" << (void*)this << "\"";

  asio_ns::post(this->_protocol->context.io_context,
                [self = this->shared_from_this()] {
                  static_cast<HttpCommTask<T>&>(*self.get()).checkVSTPrefix();
                });
}

template<SocketType T>
bool HttpCommTask<T>::readCallback(asio_ns::error_code ec) {
  llhttp_errno_t err = HPE_OK;
  if (!ec) {
    // Inspect the received data
    size_t nparsed = 0;
    for (auto const& buffer : this->_protocol->buffer.data()) {
      char const* data = reinterpret_cast<char const*>(buffer.data());
      char const* end = data + buffer.size();
      do {
        size_t datasize = end - data;

        TRI_IF_FAILURE("HttpCommTask<T>::readCallback_in_small_chunks") {
          // we had an issue that URLs were cut off because the url data was
          // handed in in multiple buffers. To cover this case, we simulate that
          // data fed to the parser in small chunks.
          constexpr size_t chunksize = 5;
          datasize = std::min<size_t>(datasize, chunksize);
        }

        err = llhttp_execute(&_parser, data, datasize);
        if (err != HPE_OK) {
          ptrdiff_t diff = llhttp_get_error_pos(&_parser) - data;
          TRI_ASSERT(diff >= 0);
          nparsed += static_cast<size_t>(diff);
          break;
        }
        nparsed += datasize;
        data += datasize;
      } while (ADB_UNLIKELY(data < end));
    }

    TRI_ASSERT(nparsed < std::numeric_limits<size_t>::max());
    // Remove consumed data from receive buffer.
    this->_protocol->buffer.consume(nparsed);
    // And count it in the statistics:
    this->requestStatistics(1UL).ADD_RECEIVED_BYTES(nparsed);

    if (_messageDone) {
      TRI_ASSERT(err == HPE_PAUSED);
      _messageDone = false;
      processRequest();
      return false;  // stop read loop
    }

  } else {
    // got a connection error
    if (ec == asio_ns::error::misc_errors::eof) {
      err = llhttp_finish(&_parser);
    } else {
      LOG_TOPIC("395fe", DEBUG, Logger::REQUESTS)
          << "Error while reading from socket: '" << ec.message() << "'";
      err = HPE_INVALID_EOF_STATE;
    }
  }

  if (err != HPE_OK && err != HPE_USER && err != HPE_CB_HEADERS_COMPLETE) {
    if (err == HPE_INVALID_EOF_STATE) {
      LOG_TOPIC("595fd", TRACE, Logger::REQUESTS)
          << "Connection closed by peer, with ptr " << this;
    } else {
      LOG_TOPIC("595fe", TRACE, Logger::REQUESTS)
          << "HTTP parse failure: '" << llhttp_get_error_reason(&_parser)
          << "'";
    }
    this->close(ec);
  }

  return err == HPE_OK && !ec;
}

template<SocketType T>
void HttpCommTask<T>::setIOTimeout() {
  double secs = this->_generalServerFeature.keepAliveTimeout();
  if (secs <= 0) {
    return;
  }

  const bool wasReading = this->_reading;
  const bool wasWriting = this->_writing;
  TRI_ASSERT((wasReading && !wasWriting) || (!wasReading && wasWriting));

  auto millis = std::chrono::milliseconds(static_cast<int64_t>(secs * 1000));
  this->_protocol->timer.expires_after(millis);
  this->_protocol->timer.async_wait(withLogContext(
      [=, self = CommTask::weak_from_this()](asio_ns::error_code const& ec) {
        std::shared_ptr<CommTask> s;
        if (ec || !(s = self.lock())) {  // was canceled / deallocated
          return;
        }

        auto& me = static_cast<HttpCommTask<T>&>(*s);
        if ((wasReading && me._reading) || (wasWriting && me._writing)) {
          LOG_TOPIC("5c1e0", INFO, Logger::REQUESTS)
              << "keep alive timeout, closing stream!";
          static_cast<GeneralCommTask<T>&>(*s).close(ec);
        }
      }));
}

namespace {
static constexpr const char* vst10 = "VST/1.0\r\n\r\n";
static constexpr const char* vst11 = "VST/1.1\r\n\r\n";
static constexpr const char* h2Preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
static constexpr size_t vstLen = 11;        // length of vst connection preface
static constexpr size_t h2PrefaceLen = 24;  // length of h2 connection preface
static constexpr size_t minHttpRequestLen =
    18;  // min length of http 1.0 request
}  // namespace

template<SocketType T>
void HttpCommTask<T>::checkVSTPrefix() {
  auto cb = [self = this->shared_from_this()](asio_ns::error_code const& ec,
                                              size_t nread) {
    auto& me = static_cast<HttpCommTask<T>&>(*self);
    if (ec || nread < vstLen) {
      me.close(ec);
      return;
    }
    me._protocol->buffer.commit(nread);

    auto bg = asio_ns::buffers_begin(me._protocol->buffer.data());
    if (std::equal(::vst10, ::vst10 + vstLen, bg, bg + ptrdiff_t(vstLen))) {
      me._protocol->buffer.consume(vstLen);  // remove VST/1.0 prefix
      auto commTask = std::make_unique<VstCommTask<T>>(
          me._server, me._connectionInfo, std::move(me._protocol),
          fuerte::vst::VST1_0);
      commTask->setStatistics(1UL, me.stealRequestStatistics(1UL));
      me._server.registerTask(std::move(commTask));
      me.close(ec);
      return;  // vst 1.0

    } else if (std::equal(::vst11, ::vst11 + vstLen, bg,
                          bg + ptrdiff_t(vstLen))) {
      me._protocol->buffer.consume(vstLen);  // remove VST/1.1 prefix
      auto commTask = std::make_unique<VstCommTask<T>>(
          me._server, me._connectionInfo, std::move(me._protocol),
          fuerte::vst::VST1_1);
      commTask->setStatistics(1UL, me.stealRequestStatistics(1UL));
      me._server.registerTask(std::move(commTask));
      me.close(ec);
      return;  // vst 1.1
    } else if (nread >= h2PrefaceLen &&
               std::equal(::h2Preface, ::h2Preface + h2PrefaceLen, bg,
                          bg + ptrdiff_t(h2PrefaceLen))) {
      // do not remove preface here, H2CommTask will read it from buffer
      auto commTask = std::make_unique<H2CommTask<T>>(
          me._server, me._connectionInfo, std::move(me._protocol));
      commTask->setStatistics(1UL, me.stealRequestStatistics(1UL));
      me._server.registerTask(std::move(commTask));
      me.close(ec);
      return;  // http2 upgrade
    }

    me.asyncReadSome();  // continue reading
  };
  auto buffs =
      this->_protocol->buffer.prepare(GeneralCommTask<T>::ReadBlockSize);
  asio_ns::async_read(this->_protocol->socket, buffs,
                      asio_ns::transfer_at_least(minHttpRequestLen),
                      withLogContext(std::move(cb)));
}

#ifdef USE_DTRACE
// Moved here to prevent multiplicity by template
static void __attribute__((noinline))
DTraceHttpCommTaskProcessRequest(size_t th) {
  DTRACE_PROBE1(arangod, HttpCommTaskProcessRequest, th);
}
#else
static void DTraceHttpCommTaskProcessRequest(size_t) {}
#endif

template<SocketType T>
std::string HttpCommTask<T>::url() const {
  if (_request != nullptr) {
    return std::string((_request->databaseName().empty()
                            ? ""
                            : "/_db/" + StringUtils::urlEncode(
                                            _request->databaseName()))) +
           (Logger::logRequestParameters() ? _request->fullUrl()
                                           : _request->requestPath());
  }
  return "";
}

template<SocketType T>
void HttpCommTask<T>::processRequest() {
  DTraceHttpCommTaskProcessRequest((size_t)this);

  TRI_ASSERT(_request);
  auto msgId = _request->messageId();
  auto respContentType = _request->contentTypeResponse();
  try {
    doProcessRequest();
  } catch (arangodb::basics::Exception const& ex) {
    LOG_TOPIC("1e6f8", WARN, Logger::REQUESTS)
        << "request failed with error " << ex.code() << " " << ex.message();
    this->sendErrorResponse(GeneralResponse::responseCode(ex.code()),
                            respContentType, msgId, ex.code(), ex.message());
  } catch (std::exception const& ex) {
    LOG_TOPIC("1fbd2", WARN, Logger::REQUESTS)
        << "request failed with error " << ex.what();
    this->sendErrorResponse(ResponseCode::SERVER_ERROR, respContentType, msgId,
                            ErrorCode(TRI_ERROR_FAILED), ex.what());
  }
}

template<SocketType T>
void HttpCommTask<T>::doProcessRequest() {
  this->_protocol->timer.cancel();
  if (this->stopped()) {
    return;  // we have to ignore this request because the connection has
             // already been closed
  }

  // we may have gotten an H2 Upgrade request
  if (ADB_UNLIKELY(_parser.upgrade)) {
    LOG_TOPIC("5a660", INFO, Logger::REQUESTS)
        << "detected an 'Upgrade' header";
    bool found;
    std::string const& h2 = _request->header("upgrade");
    std::string const& settings = _request->header("http2-settings", found);
    if (h2 == "h2c" && found && !settings.empty()) {
      auto task = std::make_shared<H2CommTask<T>>(
          this->_server, this->_connectionInfo, std::move(this->_protocol));
      task->setStatistics(1UL, this->stealRequestStatistics(1UL));
      task->upgradeHttp1(std::move(_request));
      this->close();
      return;
    }
  }

  // ensure there is a null byte termination. Some RestHandlers use
  // C functions like strchr that except a C string as input
  _request->appendNullTerminator();
  // no need to increase memory usage here!
  {
    LOG_TOPIC("6e770", INFO, Logger::REQUESTS)
        << "\"http-request-begin\",\"" << (void*)this << "\",\""
        << this->_connectionInfo.clientAddress << "\",\""
        << HttpRequest::translateMethod(_request->requestType()) << "\",\""
        << url() << "\"";

    std::string_view body = _request->rawPayload();
    this->_generalServerFeature.countHttp1Request(body.size());

    if (Logger::isEnabled(LogLevel::TRACE, Logger::REQUESTS) &&
        Logger::logRequestParameters()) {
      // Log HTTP headers:
      this->logRequestHeaders("http", _request->headers());

      if (!body.empty()) {
        this->logRequestBody("http", _request->contentType(), body);
      }
    }
  }

  // store origin header for later use
  _origin = _request->header(StaticStrings::Origin);

  // OPTIONS requests currently go unauthenticated
  if (_request->requestType() == rest::RequestType::OPTIONS) {
    this->processCorsOptions(std::move(_request), _origin);
    return;
  }

  ServerState::Mode mode = ServerState::mode();

  // scrape the auth headers to determine and authenticate the user
  auto authToken = this->checkAuthHeader(*_request, mode);

  // We want to separate superuser token traffic:
  if (_request->authenticated() && _request->user().empty()) {
    this->requestStatistics(1UL).SET_SUPERUSER();
  }

  // first check whether we allow the request to continue
  CommTask::Flow cont = this->prepareExecution(authToken, *_request, mode);
  if (cont != CommTask::Flow::Continue) {
    return;  // prepareExecution sends the error message
  }

  // unzip / deflate
  if (!this->handleContentEncoding(*_request)) {
    this->sendErrorResponse(rest::ResponseCode::BAD,
                            _request->contentTypeResponse(), 1,
                            TRI_ERROR_BAD_PARAMETER, "decoding error");
    return;
  }

  // create a handler and execute
  auto resp = std::make_unique<HttpResponse>(rest::ResponseCode::SERVER_ERROR,
                                             1, nullptr);
  resp->setContentType(_request->contentTypeResponse());
  this->executeRequest(std::move(_request), std::move(resp), mode);
}

#ifdef USE_DTRACE
// Moved here to prevent multiplicity by template
static void __attribute__((noinline))
DTraceHttpCommTaskSendResponse(size_t th) {
  DTRACE_PROBE1(arangod, HttpCommTaskSendResponse, th);
}
#else
static void DTraceHttpCommTaskSendResponse(size_t) {}
#endif

template<SocketType T>
void HttpCommTask<T>::sendResponse(std::unique_ptr<GeneralResponse> baseRes,
                                   RequestStatistics::Item stat) {
  if (this->stopped()) {
    return;
  }

  DTraceHttpCommTaskSendResponse((size_t)this);

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  HttpResponse& response = dynamic_cast<HttpResponse&>(*baseRes);
#else
  HttpResponse& response = static_cast<HttpResponse&>(*baseRes);
#endif

  // will add CORS headers if necessary
  this->finishExecution(*baseRes, _origin);

  // handle response code 204 No Content
  if (response.responseCode() == rest::ResponseCode::NO_CONTENT) {
    response.clearBody();
  }

  _header.clear();
  _header.reserve(220);

  _header.append(std::string_view("HTTP/1.1 "));
  _header.append(GeneralResponse::responseString(response.responseCode()));
  _header.append("\r\n", 2);

  // if we return HTTP 401, we need to send a www-authenticate header back with
  // the response. in this case we need to check if the header was already set
  // or if we need to set it ourselves.
  // note that clients can suppress sending the www-authenticate header by
  // sending us an x-omit-www-authenticate header.
  bool needWwwAuthenticate =
      (response.responseCode() == rest::ResponseCode::UNAUTHORIZED &&
       (!_request || _request->header("x-omit-www-authenticate").empty()));

  bool seenServerHeader = false;
  // bool seenConnectionHeader = false;
  for (auto const& it : response.headers()) {
    std::string const& key = it.first;
    size_t const keyLength = key.size();
    // ignore content-length
    if (key == StaticStrings::ContentLength ||
        key == StaticStrings::Connection ||
        key == StaticStrings::TransferEncoding) {
      continue;
    }

    if (key == StaticStrings::Server) {
      seenServerHeader = true;
    } else if (needWwwAuthenticate && key == StaticStrings::WwwAuthenticate) {
      needWwwAuthenticate = false;
    }

    // reserve enough space for header name + ": " + value + "\r\n"
    _header.reserve(key.size() + 2 + it.second.size() + 2);

    char const* p = key.data();
    char const* end = p + keyLength;
    int capState = 1;
    while (p < end) {
      if (capState == 1) {
        // upper case
        _header.push_back(StringUtils::toupper(*p));
        capState = 0;
      } else if (capState == 0) {
        // normal case
        _header.push_back(StringUtils::tolower(*p));
        if (*p == '-') {
          capState = 1;
        } else if (*p == ':') {
          capState = 2;
        }
      } else {
        // output as is
        _header.push_back(*p);
      }
      ++p;
    }

    _header.append(": ", 2);
    _header.append(it.second);
    _header.append("\r\n", 2);
  }

  // add "Server" response header
  if (!seenServerHeader) {
    _header.append(std::string_view("Server: ArangoDB\r\n"));
  }

  if (needWwwAuthenticate) {
    TRI_ASSERT(response.responseCode() == rest::ResponseCode::UNAUTHORIZED);
    _header.append(
        std::string_view("Www-Authenticate: Basic, realm=\"ArangoDB\"\r\n"));
    _header.append(
        std::string_view("Www-Authenticate: Bearer, token_type=\"JWT\", "
                         "realm=\"ArangoDB\"\r\n"));
  }

  // turn on the keepAlive timer
  double secs = this->_generalServerFeature.keepAliveTimeout();
  if (_shouldKeepAlive && secs > 0) {
    _header.append(std::string_view("Connection: Keep-Alive\r\n"));
  } else {
    _header.append(std::string_view("Connection: Close\r\n"));
  }

  if (response.contentType() != ContentType::CUSTOM) {
    _header.append(std::string_view("Content-Type: "));
    _header.append(rest::contentTypeToString(response.contentType()));
    _header.append("\r\n", 2);
  }

  for (auto const& it : response.cookies()) {
    _header.append(std::string_view("Set-Cookie: "));
    _header.append(it);
    _header.append("\r\n", 2);
  }

  size_t len = response.bodySize();
  TRI_ASSERT(response.responseCode() != rest::ResponseCode::NO_CONTENT ||
             len == 0)
      << "response code 204 requires body length to be zero";
  _header.append(std::string_view("Content-Length: "));
  _header.append(std::to_string(len));
  _header.append("\r\n\r\n", 4);

  TRI_ASSERT(_response == nullptr);
  _response = response.stealBody();
  // append write buffer and statistics
  TRI_ASSERT(response.responseCode() != rest::ResponseCode::NO_CONTENT ||
             _response->empty())
      << "response code 204 requires body length to be zero";

  if (Logger::isEnabled(LogLevel::TRACE, Logger::REQUESTS) &&
      Logger::logRequestParameters()) {
    // Log HTTP headers:
    this->logResponseHeaders("http", response.headers());

    if (!_response->empty()) {
      std::string_view body(_response->data(), _response->size());
      this->logRequestBody("http", response.contentType(), body,
                           true /* isResponse */);
    }
  }

  // and give some request information
  LOG_TOPIC("8f555", DEBUG, Logger::REQUESTS)
      << "\"http-request-end\",\"" << (void*)this << "\",\""
      << this->_connectionInfo.clientAddress << "\",\""
      << GeneralRequest::translateMethod(::llhttpToRequestType(&_parser))
      << "\",\"" << url() << "\",\""
      << static_cast<int>(response.responseCode()) << "\","
      << Logger::FIXED(stat.ELAPSED_SINCE_READ_START(), 6) << ","
      << Logger::FIXED(stat.ELAPSED_WHILE_QUEUED(), 6);

  // sendResponse is always called from a scheduler thread
  boost::asio::post(
      this->_protocol->context.io_context,
      [self = this->shared_from_this(), stat = std::move(stat)]() mutable {
        static_cast<HttpCommTask<T>&>(*self).writeResponse(std::move(stat));
      });
}

#ifdef USE_DTRACE
// Moved here to prevent multiplicity by template
static void __attribute__((noinline))
DTraceHttpCommTaskWriteResponse(size_t th) {
  DTRACE_PROBE1(arangod, HttpCommTaskWriteResponse, th);
}
static void __attribute__((noinline))
DTraceHttpCommTaskResponseWritten(size_t th) {
  DTRACE_PROBE1(arangod, HttpCommTaskResponseWritten, th);
}
#else
static void DTraceHttpCommTaskWriteResponse(size_t) {}
static void DTraceHttpCommTaskResponseWritten(size_t) {}
#endif

// called on IO context thread
template<SocketType T>
void HttpCommTask<T>::writeResponse(RequestStatistics::Item stat) {
  DTraceHttpCommTaskWriteResponse((size_t)this);

  TRI_ASSERT(!_header.empty());

  stat.SET_WRITE_START();

  std::array<asio_ns::const_buffer, 2> buffers;
  buffers[0] = asio_ns::buffer(_header.data(), _header.size());
  if (HTTP_HEAD != _parser.method) {
    buffers[1] = asio_ns::buffer(_response->data(), _response->size());
  }

  this->_writing = true;
  asio_ns::async_write(
      this->_protocol->socket, buffers,
      withLogContext([self = this->shared_from_this(), stat = std::move(stat)](
                         asio_ns::error_code ec, size_t nwrite) {
        DTraceHttpCommTaskResponseWritten((size_t)self.get());

        auto& me = static_cast<HttpCommTask<T>&>(*self);
        me._writing = false;

        stat.SET_WRITE_END();
        stat.ADD_SENT_BYTES(nwrite);

        me._response.reset();

        llhttp_errno_t err = llhttp_get_errno(&me._parser);
        if (ec || !me._shouldKeepAlive || err != HPE_PAUSED) {
          me.close(ec);
        } else {  // ec == HPE_PAUSED
          llhttp_resume(&me._parser);
          me.asyncReadSome();
        }
      }));
}

template<SocketType T>
std::unique_ptr<GeneralResponse> HttpCommTask<T>::createResponse(
    rest::ResponseCode responseCode, uint64_t mid) {
  TRI_ASSERT(mid == 1);
  return std::make_unique<HttpResponse>(responseCode, mid);
}

template class arangodb::rest::HttpCommTask<SocketType::Tcp>;
template class arangodb::rest::HttpCommTask<SocketType::Ssl>;
#ifndef _WIN32
template class arangodb::rest::HttpCommTask<SocketType::Unix>;
#endif
