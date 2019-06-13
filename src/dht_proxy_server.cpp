/*
 *  Copyright (C) 2017-2019 Savoir-faire Linux Inc.
 *  Author: Sébastien Blin <sebastien.blin@savoirfairelinux.com>
 *          Adrien Béraud <adrien.beraud@savoirfairelinux.com>
 *          Vsevolod Ivanov <vsevolod.ivanov@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "dht_proxy_server.h"

#include "thread_pool.h"
#include "default_types.h"
#include "dhtrunner.h"

#include <msgpack.hpp>
#include <json/json.h>

#include <chrono>
#include <functional>
#include <limits>
#include <iostream>

using namespace std::placeholders;

namespace dht {

constexpr const std::chrono::minutes PRINT_STATS_PERIOD {2};
constexpr const size_t IO_THREADS_MAX {64};

DhtProxyServer::DhtProxyServer(std::shared_ptr<DhtRunner> dht, in_port_t port,
                              const std::string& pushServer,
                              std::shared_ptr<dht::Logger> logger,
                              const unsigned int ioThreads
):
        dht_(dht), logger_(logger), lockListener_(std::make_shared<std::mutex>()),
        listeners_(std::make_shared<std::map<restinio::connection_id_t, http::ListenerSession>>()),
        connListener_(std::make_shared<http::ConnectionListener>(
                        dht, listeners_, lockListener_, logger)),
        pushServer_(pushServer)
{
    if (not dht_)
        throw std::invalid_argument("A DHT instance must be provided");

    std::cout << "Running DHT proxy server on port " << port << std::endl;
    if (not pushServer.empty()){
#ifdef OPENDHT_PUSH_NOTIFICATIONS
        std::cout << "Using push notification server: " << pushServer << std::endl;
#else
        std::cerr << "Push server defined but built OpenDHT built without push notification support" << std::endl;
#endif
    }

    jsonBuilder_["commentStyle"] = "None";
    jsonBuilder_["indentation"] = "";

    // build http server
    auto settings = makeHttpServerSettings(port, ioThreads);
    httpServerThreadPool_.reset(new IOContextThreadPool(settings.pool_size()));

    httpServer_.reset(new restinio::http_server_t<RestRouterTraits>(
        restinio::external_io_context(httpServerThreadPool_->io_context()),
        std::forward<ServerSettings>(settings)
    ));
    // build http client
    auto pushHostPort = splitPort(pushServer_);
    uint16_t pushPort = std::atoi(pushHostPort.second.c_str());
    httpClient_.reset(new http::Client(httpServer_->io_context(),
                                       pushHostPort.first, pushPort, logger_));
    // run http server
    try {
        httpServer_->open_async([]{/*Ok.*/}, [](std::exception_ptr ex){
            std::rethrow_exception(ex);
        });
        httpServerThreadPool_->start();
    }
    catch(const std::exception &ex){
        std::cerr << "Error starting RESTinio: " << ex.what() << std::endl;
    }
    dht->forwardAllMessages(true);

    printStatsTimer_ = std::make_unique<asio::steady_timer>(
        httpServer_->io_context(), PRINT_STATS_PERIOD);
    printStatsTimer_->async_wait(std::bind(&DhtProxyServer::asyncPrintStats, this));
}

DhtProxyServer::~DhtProxyServer()
{
    stop();
}

ServerSettings
DhtProxyServer::makeHttpServerSettings(const in_port_t port, const unsigned int n_threads)
{
    using namespace std::chrono;
    auto maxThreads = std::thread::hardware_concurrency() - 1;
    auto ioThreads = n_threads < maxThreads ? n_threads : maxThreads;
    auto settings = ServerSettings(ioThreads);
    logger_->d("[restinio] io_context will run on %i thread%s",
               ioThreads, (ioThreads == 1 ? "" : "s"));
    /**
     * If max_pipelined_requests is greater than 1 then RESTinio will continue
     * to read from the socket after parsing the first request.
     * In that case, RESTinio can detect the disconnection
     * and calls state listener as expected.
     * https://github.com/Stiffstream/restinio/issues/28
     */
    settings.max_pipelined_requests(2);
    settings.logger(logger_);
    settings.port(port);
    settings.protocol(restinio::asio_ns::ip::tcp::v6());
    settings.request_handler(this->createRestRouter());
    // time limits                                              // ~ 0.8 month
    std::chrono::milliseconds timeout_request(std::numeric_limits<int>::max());
    settings.read_next_http_message_timelimit(timeout_request);
    settings.write_http_response_timelimit(60s);
    settings.handle_request_timeout(timeout_request);
    // socket options
    settings.socket_options_setter([](auto & options){
        options.set_option(asio::ip::tcp::no_delay{true});
    });
    settings.connection_state_listener(connListener_);
    return settings;
}

void
DhtProxyServer::stop()
{
    logger_->d("[restinio] closing http server async operations");
    httpServer_->close_async([this]{
        logger_->d("[restinio] stopping http server io_context thread pool");
        httpServerThreadPool_->stop();
    },
    [this](std::exception_ptr eptr){
        try {
           std::rethrow_exception(eptr);
        }
        catch (const std::exception &ex){
           logger_->e("[restinio] error closing http server async: %s", ex.what());
        }
    });
}

void
DhtProxyServer::updateStats() const
{
    auto now = clock::now();
    auto last = lastStatsReset_.exchange(now);
    auto count = requestNum_.exchange(0);
    auto dt = std::chrono::duration<double>(now - last);
    stats_.requestRate = count / dt.count();
#ifdef OPENDHT_PUSH_NOTIFICATIONS
    stats_.pushListenersCount = pushListeners_.size();
#endif
    stats_.putCount = puts_.size();
    stats_.listenCount = listeners_->size();
    stats_.nodeInfo = nodeInfo_;
}

void
DhtProxyServer::asyncPrintStats()
{
    if (httpServer_->io_context().stopped())
        return;

    if (dht_){
        updateStats();
        // Refresh stats cache
        auto newInfo = dht_->getNodeInfo();
        std::lock_guard<std::mutex> lck(statsMutex_);
        nodeInfo_ = std::move(newInfo);
        auto json = nodeInfo_.toJson();
        auto str = Json::writeString(jsonBuilder_, json);
        logger_->d("[stats] %s", str.c_str());
    }
    printStatsTimer_->expires_at(printStatsTimer_->expiry() + PRINT_STATS_PERIOD);
    printStatsTimer_->async_wait(std::bind(&DhtProxyServer::asyncPrintStats, this));
}

template <typename HttpResponse>
HttpResponse DhtProxyServer::initHttpResponse(HttpResponse response) const
{
    response.append_header("Server", "RESTinio");
    response.append_header(restinio::http_field::content_type, "application/json");
    response.append_header(restinio::http_field::access_control_allow_origin, "*");
    response.connection_keep_alive();
    return response;
}

std::unique_ptr<RestRouter>
DhtProxyServer::createRestRouter()
{
    using namespace std::placeholders;
    auto router = std::make_unique<RestRouter>();
    router->http_get("/", std::bind(&DhtProxyServer::getNodeInfo, this, _1, _2));
    // LEGACY STATS ROUTE
    router->add_handler(restinio::custom_http_methods_t::from_nodejs(restinio::method_stats.raw_id()),
                        "/", std::bind(&DhtProxyServer::getStats, this, _1, _2));
    // }
    router->http_get("/stats", std::bind(&DhtProxyServer::getStats, this, _1, _2));
    router->http_get("/:hash", std::bind(&DhtProxyServer::get, this, _1, _2));
    router->http_post("/:hash", std::bind(&DhtProxyServer::put, this, _1, _2));
    // LEGACY LISTEN ROUTE
    router->add_handler(restinio::custom_http_methods_t::from_nodejs(restinio::method_listen.raw_id()),
                        "/:hash", std::bind(&DhtProxyServer::listen, this, _1, _2));
    // }
    router->http_get("/:hash/listen", std::bind(&DhtProxyServer::listen, this, _1, _2));
#ifdef OPENDHT_PUSH_NOTIFICATIONS
    router->add_handler(restinio::http_method_subscribe(),
                        "/:hash", std::bind(&DhtProxyServer::subscribe, this, _1, _2));
    router->add_handler(restinio::http_method_unsubscribe(),
                        "/:hash", std::bind(&DhtProxyServer::unsubscribe, this, _1, _2));
#endif //OPENDHT_PUSH_NOTIFICATIONS
#ifdef OPENDHT_PROXY_SERVER_IDENTITY
    // LEGACY SIGN ROUTE
    router->add_handler(restinio::custom_http_methods_t::from_nodejs(restinio::method_sign.raw_id()),
                        "/:hash", std::bind(&DhtProxyServer::putSigned, this, _1, _2));
    // }
    router->http_post("/:hash/sign", std::bind(&DhtProxyServer::putSigned, this, _1, _2));
    // LEGACY ENCRYPT ROUTE
    router->add_handler(restinio::custom_http_methods_t::from_nodejs(restinio::method_encrypt.raw_id()),
                        "/:hash", std::bind(&DhtProxyServer::putEncrypted, this, _1, _2));
    // }
    router->http_post("/:hash/encrypt", std::bind(&DhtProxyServer::putEncrypted, this, _1, _2));
#endif // OPENDHT_PROXY_SERVER_IDENTITY
    router->add_handler(restinio::http_method_options(),
                        "/:hash", std::bind(&DhtProxyServer::options, this, _1, _2));
    router->http_get("/:hash/:value", std::bind(&DhtProxyServer::getFiltered, this, _1, _2));
    return router;
}

RequestStatus
DhtProxyServer::getNodeInfo(restinio::request_handle_t request,
                            restinio::router::route_params_t params) const
{
    Json::Value result;
    std::lock_guard<std::mutex> lck(statsMutex_);
    if (nodeInfo_.ipv4.good_nodes == 0 &&
        nodeInfo_.ipv6.good_nodes == 0){
        nodeInfo_ = this->dht_->getNodeInfo();
    }
    result = nodeInfo_.toJson();
    // [ipv6:ipv4]:port or ipv4:port
    result["public_ip"] = request->remote_endpoint().address().to_string();
    auto output = Json::writeString(jsonBuilder_, result) + "\n";

    auto response = this->initHttpResponse(request->create_response());
    response.append_body(output);
    return response.done();
}

RequestStatus
DhtProxyServer::getStats(restinio::request_handle_t request,
                         restinio::router::route_params_t params)
{
    requestNum_++;
    try {
        if (dht_){
#ifdef OPENDHT_JSONCPP
            auto output = Json::writeString(jsonBuilder_, stats_.toJson()) + "\n";
            auto response = this->initHttpResponse(request->create_response());
            response.append_body(output);
            response.done();
#else
            auto response = this->initHttpResponse(
                request->create_response(restinio::status_not_found()));
            response.set_body(this->RESP_MSG_JSON_NOT_ENABLED);
            return response.done();
#endif
        } else {
            auto response = this->initHttpResponse(
                request->create_response(restinio::status_service_unavailable()));
            response.set_body(this->RESP_MSG_SERVICE_UNAVAILABLE);
            return response.done();
        }
    } catch (...){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_internal_server_error()));
        response.set_body(this->RESP_MSG_INTERNAL_SERVER_ERRROR);
        return response.done();
    }
    return restinio::request_handling_status_t::accepted;
}

RequestStatus
DhtProxyServer::get(restinio::request_handle_t request,
                    restinio::router::route_params_t params)
{
    requestNum_++;
    dht::InfoHash infoHash(params["hash"].to_string());
    if (!infoHash)
        infoHash = dht::InfoHash::get(params["hash"].to_string());

    if (!dht_){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_service_unavailable()));
        response.set_body(this->RESP_MSG_SERVICE_UNAVAILABLE);
        return response.done();
    }

    auto response = std::make_shared<ResponseByPartsBuilder>(
        this->initHttpResponse(request->create_response<ResponseByParts>()));
    response->flush();
    try {
        dht_->get(infoHash, [this, response](const dht::Sp<dht::Value>& value){
            auto output = Json::writeString(jsonBuilder_, value->toJson()) + "\n";
            response->append_chunk(output);
            response->flush();
            return true;
        },
        [response] (bool /*ok*/){
            response->done();
        });
    } catch (const std::exception& e){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_internal_server_error()));
        response.set_body(this->RESP_MSG_INTERNAL_SERVER_ERRROR);
        return response.done();
    }
    return restinio::request_handling_status_t::accepted;
}

RequestStatus
DhtProxyServer::listen(restinio::request_handle_t request,
                       restinio::router::route_params_t params)
{
    requestNum_++;
    dht::InfoHash infoHash(params["hash"].to_string());
    if (!infoHash)
        infoHash = dht::InfoHash::get(params["hash"].to_string());

    if (!dht_){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_service_unavailable()));
        response.set_body(this->RESP_MSG_SERVICE_UNAVAILABLE);
        return response.done();
    }
    auto response = std::make_shared<ResponseByPartsBuilder>(
        this->initHttpResponse(request->create_response<ResponseByParts>()));
    response->flush();
    try {
        std::lock_guard<std::mutex> lock(*lockListener_);
        // save the listener to handle a disconnect
        auto &session = (*listeners_)[request->connection_id()];
        session.hash = infoHash;
        session.response = response;
        session.token = dht_->listen(infoHash, [this, response]
                (const std::vector<dht::Sp<dht::Value>>& values, bool expired){
            for (const auto& value: values){
                auto jsonVal = value->toJson();
                if (expired)
                    jsonVal["expired"] = true;
                auto output = Json::writeString(jsonBuilder_, jsonVal) + "\n";
                response->append_chunk(output);
                response->flush();
            }
            return true;
        });

    } catch (const std::exception& e){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_internal_server_error()));
        response.set_body(this->RESP_MSG_INTERNAL_SERVER_ERRROR);
        return response.done();
    }
    return restinio::request_handling_status_t::accepted;
}

#ifdef OPENDHT_PUSH_NOTIFICATIONS

RequestStatus
DhtProxyServer::subscribe(restinio::request_handle_t request,
                          restinio::router::route_params_t params)
{
    requestNum_++;

    dht::InfoHash infoHash(params["hash"].to_string());
    if (!infoHash)
        infoHash = dht::InfoHash::get(params["hash"].to_string());

    if (!dht_){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_service_unavailable()));
        response.set_body(this->RESP_MSG_SERVICE_UNAVAILABLE);
        return response.done();
    }
    try {
        std::string err;
        Json::Value root;
        Json::CharReaderBuilder rbuilder;
        auto* char_data = reinterpret_cast<const char*>(request->body().data());
        auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());
        if (!reader->parse(char_data, char_data + request->body().size(), &root, &err)){
            auto response = this->initHttpResponse(
                request->create_response(restinio::status_bad_request()));
            response.set_body(this->RESP_MSG_JSON_INCORRECT);
            return response.done();
        }
        auto pushToken = root["key"].asString();
        if (pushToken.empty()){
            auto response = this->initHttpResponse(
                request->create_response(restinio::status_bad_request()));
            response.set_body(this->RESP_MSG_NO_TOKEN);
            return response.done();
        }
        auto platform = root["platform"].asString();
        auto isAndroid = platform == "android";
        auto clientId = root.isMember("client_id") ? root["client_id"].asString() : std::string();

        logger_->w("[subscribe] %s client: %s", infoHash.toString().c_str(), clientId.c_str());
        // ================ Search for existing listener ===================
        // start the timer
        auto timeout = std::chrono::steady_clock::now() + proxy::OP_TIMEOUT;
        std::lock_guard<std::mutex> lock(lockPushListeners_);

        // Insert new or return existing push listeners of a token
        auto pushListener = pushListeners_.emplace(pushToken, PushListener{}).first;
        auto pushListeners = pushListener->second.listeners.emplace(infoHash, std::vector<Listener>{}).first;

        for (auto &listener: pushListeners->second){
            logger_->w("[subscribe] found client_id: %s", listener.clientId.c_str());
            // Found -> Resubscribe
            if (listener.clientId == clientId){
                // Reset timers
                listener.expireTimer->expires_at(timeout);
                listener.expireNotifyTimer->expires_at(timeout - proxy::OP_MARGIN);
                // Send response header
                auto response = std::make_shared<ResponseByPartsBuilder>(
                    this->initHttpResponse(request->create_response<ResponseByParts>()));
                response->flush();
                // No Refresh
                if (!root.isMember("refresh") or !root["refresh"].asBool()){
                    dht_->get(infoHash, [this, response](const dht::Sp<dht::Value>& value){
                        auto output = Json::writeString(jsonBuilder_, value->toJson()) + "\n";
                        response->append_chunk(output);
                        response->flush();
                        return true;
                    },
                    [response] (bool){
                        response->done();
                    });
                // Refresh
                } else {
                    response->append_chunk("{}\n");
                    response->done();
                }
                return restinio::request_handling_status_t::accepted;
            }
        }
        // =========== No existing listener for an infoHash ============
        // Add new listener to list of listeners
        pushListeners->second.emplace_back(Listener{});
        auto &listener = pushListeners->second.back();
        listener.clientId = clientId;

        // Add listen on dht
        listener.internalToken = dht_->listen(infoHash,
            [this, infoHash, pushToken, isAndroid, clientId]
            (const std::vector<std::shared_ptr<Value>>& values, bool expired){
                // Build message content
                Json::Value json;
                json["key"] = infoHash.toString();
                json["to"] = clientId;
                if (expired and values.size() < 2){
                    std::stringstream ss;
                    for(size_t i = 0; i < values.size(); ++i){
                        if(i != 0) ss << ",";
                        ss << values[i]->id;
                    }
                    json["exp"] = ss.str();
                }
                sendPushNotification(pushToken, std::move(json), isAndroid);
                return true;
            }
        );
        // Init & set timers
        auto &ctx = httpServer_->io_context();
        listener.expireTimer = std::make_unique<asio::steady_timer>(ctx, timeout);
        listener.expireNotifyTimer = std::make_unique<asio::steady_timer>(ctx,
                                        timeout - proxy::OP_MARGIN);
        // Launch timers
        listener.expireTimer->async_wait(std::bind(
            &DhtProxyServer::cancelPushListen, this, pushToken, infoHash, clientId));

        listener.expireNotifyTimer->async_wait(
        [this, infoHash, pushToken, isAndroid, clientId](const asio::error_code &ec){
            logger_->d("[subscribe] sending refresh %s", infoHash.toString().c_str());
            if (ec)
                logger_->d("[subscribe] error sending refresh: %s", ec.message().c_str());
            Json::Value json;
            json["timeout"] = infoHash.toString();
            json["to"] = clientId;
            sendPushNotification(pushToken, std::move(json), isAndroid);
        });
        auto response = this->initHttpResponse(request->create_response());
        response.set_body("{}\n");
        return response.done();
    }
    catch (...) {
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_internal_server_error()));
        response.set_body(this->RESP_MSG_INTERNAL_SERVER_ERRROR);
        return response.done();
    }
    return restinio::request_handling_status_t::accepted;
}

RequestStatus
DhtProxyServer::unsubscribe(restinio::request_handle_t request,
                            restinio::router::route_params_t params)
{
    requestNum_++;

    dht::InfoHash infoHash(params["hash"].to_string());
    if (!infoHash)
        infoHash = dht::InfoHash::get(params["hash"].to_string());

    if (!dht_){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_service_unavailable()));
        response.set_body(this->RESP_MSG_SERVICE_UNAVAILABLE);
        return response.done();
    }

    try {
        std::string err;
        Json::Value root;
        Json::CharReaderBuilder rbuilder;
        auto* char_data = reinterpret_cast<const char*>(request->body().data());
        auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());

        if (!reader->parse(char_data, char_data + request->body().size(), &root, &err)){
            auto response = this->initHttpResponse(
                request->create_response(restinio::status_bad_request()));
            response.set_body(this->RESP_MSG_JSON_INCORRECT);
            return response.done();
        }
        auto pushToken = root["key"].asString();
        if (pushToken.empty())
            return restinio::request_handling_status_t::rejected;
        auto clientId = root["client_id"].asString();

        cancelPushListen(pushToken, infoHash, clientId);
        auto response = this->initHttpResponse(request->create_response());
        return response.done();
    }
    catch (...) {
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_internal_server_error()));
        response.set_body(this->RESP_MSG_INTERNAL_SERVER_ERRROR);
        return response.done();
    }
}

void
DhtProxyServer::cancelPushListen(const std::string& pushToken, const dht::InfoHash& key, const std::string& clientId)
{
    logger_->d("[cancelpushlisten] %s %s", key.toString().c_str(), clientId.c_str());
    std::lock_guard<std::mutex> lock(*lockListener_);

    auto pushListener = pushListeners_.find(pushToken);
    if (pushListener == pushListeners_.end())
        return;
    auto listeners = pushListener->second.listeners.find(key);
    if (listeners == pushListener->second.listeners.end())
        return;

    for (auto listener = listeners->second.begin(); listener != listeners->second.end();){
        if (listener->clientId == clientId){
            if (dht_)
                dht_->cancelListen(key, std::move(listener->internalToken));
            listener = listeners->second.erase(listener);
        } else {
            ++listener;
        }
    }
    if (listeners->second.empty())
        pushListener->second.listeners.erase(listeners);
    if (pushListener->second.listeners.empty())
        pushListeners_.erase(pushListener);
}

void
DhtProxyServer::sendPushNotification(const std::string& token, Json::Value&& json, bool isAndroid) const
{
    if (pushServer_.empty())
        return;

    restinio::http_request_header_t header;
    header.request_target("/api/push");
    header.method(restinio::http_method_post());

    restinio::http_header_fields_t header_fields;
    header_fields.append_field(restinio::http_field_t::host, pushServer_.c_str());
    header_fields.append_field(restinio::http_field_t::user_agent, "RESTinio client");
    header_fields.append_field(restinio::http_field_t::accept, "*/*");
    header_fields.append_field(restinio::http_field_t::content_type, "application/json");

    // NOTE: see https://github.com/appleboy/gorush
    Json::Value notification(Json::objectValue);
    Json::Value tokens(Json::arrayValue);
    tokens[0] = token;
    notification["tokens"] = std::move(tokens);
    notification["platform"] = isAndroid ? 2 : 1;
    notification["data"] = std::move(json);
    notification["priority"] = "high";
    notification["time_to_live"] = 600;

    Json::Value notifications(Json::arrayValue);
    notifications[0] = notification;

    Json::Value content;
    content["notifications"] = std::move(notifications);

    Json::StreamWriterBuilder wbuilder;
    wbuilder["commentStyle"] = "None";
    wbuilder["indentation"] = "";
    auto body = Json::writeString(wbuilder, content);

    auto parser = std::make_shared<http_parser>();
    http_parser_init(parser.get(), HTTP_RESPONSE);

    auto parser_s = std::make_shared<http_parser_settings>();
    http_parser_settings_init(parser_s.get());
    parser_s->on_status = []( http_parser * parser, const char * at, size_t length ) -> int {
        if (parser->status_code == 200)
            return 0;
        std::cerr << "Error in SendPushNotification status_code=" << parser->status_code << std::endl;
        return 1;
    };
    auto request = httpClient_->create_request(header, header_fields,
        restinio::http_connection_header_t::close, body);
    httpClient_->post_request(request, parser, parser_s);
}

#endif //OPENDHT_PUSH_NOTIFICATIONS

void
DhtProxyServer::cancelPut(const InfoHash& key, Value::Id vid)
{
    std::cout << "cancelPut " << key << " " << vid << std::endl;
    auto sPuts = puts_.find(key);
    if (sPuts == puts_.end())
        return;
    auto& sPutsMap = sPuts->second.puts;
    auto put = sPutsMap.find(vid);
    if (put == sPutsMap.end())
        return;
    if (dht_)
        dht_->cancelPut(key, vid);
    if (put->second.expireNotifyTimer)
        put->second.expireNotifyTimer->cancel();
    sPutsMap.erase(put);
    if (sPutsMap.empty())
        puts_.erase(sPuts);
}

RequestStatus
DhtProxyServer::put(restinio::request_handle_t request,
                    restinio::router::route_params_t params)
{
    requestNum_++;
    dht::InfoHash infoHash(params["hash"].to_string());
    if (!infoHash)
        infoHash = dht::InfoHash::get(params["hash"].to_string());

    if (!dht_){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_service_unavailable()));
        response.set_body(this->RESP_MSG_SERVICE_UNAVAILABLE);
        return response.done();
    }
    else if (request->body().empty()){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_bad_request()));
        response.set_body(this->RESP_MSG_MISSING_PARAMS);
        return response.done();
    }

    try {
        std::string err;
        Json::Value root;
        Json::CharReaderBuilder rbuilder;
        auto* char_data = reinterpret_cast<const char*>(request->body().data());
        auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());

        if (reader->parse(char_data, char_data + request->body().size(), &root, &err)){
            auto value = std::make_shared<dht::Value>(root);
            bool permanent = root.isMember("permanent");
            std::cout << "Got put " << infoHash << " " << *value <<
                         " " << (permanent ? "permanent" : "") << std::endl;
            if (permanent){
                std::string pushToken, clientId, platform;
                auto& pVal = root["permanent"];
                if (pVal.isObject()){
                    pushToken = pVal["key"].asString();
                    clientId = pVal["client_id"].asString();
                    platform = pVal["platform"].asString();
                }
                std::unique_lock<std::mutex> lock(lockSearchPuts_);
                auto timeout = std::chrono::steady_clock::now() + proxy::OP_TIMEOUT;
                auto vid = value->id;
                auto sPuts = puts_.emplace(infoHash, SearchPuts{}).first;
                auto r = sPuts->second.puts.emplace(vid, PermanentPut{});
                auto& pput = r.first->second;
                if (r.second){
                    auto &ctx = httpServer_->io_context();
                    pput.expireTimer = std::make_unique<asio::steady_timer>(ctx, timeout);
                    pput.expireTimer->async_wait([this, infoHash, vid](const asio::error_code &ec){
                        std::cout << "Permanent put expired: " << infoHash << " " << vid << std::endl;
                        if (ec)
                            std::cout << "Permanent put error: " << ec.message() << std::endl;
                        cancelPut(infoHash, vid);
                    });
#ifdef OPENDHT_PUSH_NOTIFICATIONS
                    if (not pushToken.empty()){
                        bool isAndroid = platform == "android";
                        pput.expireNotifyTimer = std::make_unique<asio::steady_timer>(ctx,
                                                    timeout - proxy::OP_MARGIN);
                        pput.expireNotifyTimer->async_wait(
                        [this, infoHash, vid, pushToken, clientId, isAndroid](const asio::error_code &ec)
                        {
                            std::cout << "Permanent put refresh: " << infoHash << " " << vid << std::endl;
                            if (ec)
                                std::cout << "Permanent put refresh error: " << ec.message() << std::endl;
                            Json::Value json;
                            json["timeout"] = infoHash.toString();
                            json["to"] = clientId;
                            json["vid"] = std::to_string(vid);
                            sendPushNotification(pushToken, std::move(json), isAndroid);
                        });
                    }
#endif
                } else {
                    pput.expireTimer->expires_at(timeout);
                    if (pput.expireNotifyTimer)
                        pput.expireNotifyTimer->expires_at(timeout - proxy::OP_MARGIN);
                }
                lock.unlock();
            }
            dht_->put(infoHash, value, [this, request, value](bool ok){
                if (ok){
                    auto output = Json::writeString(jsonBuilder_, value->toJson()) + "\n";
                    auto response = this->initHttpResponse(request->create_response());
                    response.append_body(output);
                    response.done();
                } else {
                    auto response = this->initHttpResponse(request->create_response(
                        restinio::status_bad_gateway()));
                    response.set_body(this->RESP_MSG_PUT_FAILED);
                    response.done();
                }
            }, dht::time_point::max(), permanent);
        } else {
            auto response = this->initHttpResponse(
                request->create_response(restinio::status_bad_request()));
            response.set_body(this->RESP_MSG_JSON_INCORRECT);
            return response.done();
        }
    } catch (const std::exception& e){
        std::cout << "Error performing put: " << e.what() << std::endl;
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_internal_server_error()));
        response.set_body(this->RESP_MSG_INTERNAL_SERVER_ERRROR);
        return response.done();
    }
    return restinio::request_handling_status_t::accepted;
}

#ifdef OPENDHT_PROXY_SERVER_IDENTITY

RequestStatus DhtProxyServer::putSigned(restinio::request_handle_t request,
                                        restinio::router::route_params_t params) const
{
    requestNum_++;
    dht::InfoHash infoHash(params["hash"].to_string());
    if (!infoHash)
        infoHash = dht::InfoHash::get(params["hash"].to_string());

    if (!dht_){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_service_unavailable()));
        response.set_body(this->RESP_MSG_SERVICE_UNAVAILABLE);
        return response.done();
    }
    else if (request->body().empty()){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_bad_request()));
        response.set_body(this->RESP_MSG_MISSING_PARAMS);
        return response.done();
    }

    try {
        std::string err;
        Json::Value root;
        Json::CharReaderBuilder rbuilder;
        auto* char_data = reinterpret_cast<const char*>(request->body().data());
        auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());

        if (reader->parse(char_data, char_data + request->body().size(), &root, &err)){

            auto value = std::make_shared<Value>(root);

            dht_->putSigned(infoHash, value, [this, request, value](bool ok){
                if (ok){
                    auto output = Json::writeString(jsonBuilder_, value->toJson()) + "\n";
                    auto response = this->initHttpResponse(request->create_response());
                    response.append_body(output);
                    response.done();
                } else {
                    auto response = this->initHttpResponse(request->create_response(
                        restinio::status_bad_gateway()));
                    response.set_body(this->RESP_MSG_PUT_FAILED);
                    response.done();
                }
            });
        } else {
            auto response = this->initHttpResponse(
                request->create_response(restinio::status_bad_request()));
            response.set_body(this->RESP_MSG_JSON_INCORRECT);
            return response.done();
        }
    } catch (const std::exception& e){
        std::cout << "Error performing put: " << e.what() << std::endl;
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_internal_server_error()));
        response.set_body(this->RESP_MSG_INTERNAL_SERVER_ERRROR);
        return response.done();
    }
    return restinio::request_handling_status_t::accepted;
}

RequestStatus
DhtProxyServer::putEncrypted(restinio::request_handle_t request,
                             restinio::router::route_params_t params)
{
    requestNum_++;
    dht::InfoHash infoHash(params["hash"].to_string());
    if (!infoHash)
        infoHash = dht::InfoHash::get(params["hash"].to_string());

    if (!dht_){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_service_unavailable()));
        response.set_body(this->RESP_MSG_SERVICE_UNAVAILABLE);
        return response.done();
    }
    else if (request->body().empty()){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_bad_request()));
        response.set_body(this->RESP_MSG_MISSING_PARAMS);
        return response.done();
    }

    try {
        std::string err;
        Json::Value root;
        Json::CharReaderBuilder rbuilder;
        auto* char_data = reinterpret_cast<const char*>(request->body().data());
        auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());

        if (reader->parse(char_data, char_data + request->body().size(), &root, &err)){
            InfoHash to(root["to"].asString());
            if (!to){
                auto response = this->initHttpResponse(
                    request->create_response(restinio::status_bad_request()));
                response.set_body(this->RESP_MSG_DESTINATION_NOT_FOUND);
                return response.done();
            }
            auto value = std::make_shared<Value>(root);
            dht_->putEncrypted(infoHash, to, value, [this, request, value](bool ok){
                if (ok){
                    auto output = Json::writeString(jsonBuilder_, value->toJson()) + "\n";
                    auto response = this->initHttpResponse(request->create_response());
                    response.append_body(output);
                    response.done();
                } else {
                    auto response = this->initHttpResponse(request->create_response(
                        restinio::status_bad_gateway()));
                    response.set_body(this->RESP_MSG_PUT_FAILED);
                    response.done();
                }
            });
        } else {
            auto response = this->initHttpResponse(
                request->create_response(restinio::status_bad_request()));
            response.set_body(this->RESP_MSG_JSON_INCORRECT);
            return response.done();
        }
    } catch (const std::exception& e){
        std::cout << "Error performing put: " << e.what() << std::endl;
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_internal_server_error()));
        response.set_body(this->RESP_MSG_INTERNAL_SERVER_ERRROR);
        return response.done();
    }
    return restinio::request_handling_status_t::accepted;
}

#endif // OPENDHT_PROXY_SERVER_IDENTITY

RequestStatus
DhtProxyServer::options(restinio::request_handle_t request,
                        restinio::router::route_params_t params)
{
    this->requestNum_++;
#ifdef OPENDHT_PROXY_SERVER_IDENTITY
    const auto methods = "OPTIONS, GET, POST, LISTEN, SIGN, ENCRYPT";
#else
    const auto methods = "OPTIONS, GET, POST, LISTEN";
#endif
    auto response = initHttpResponse(request->create_response());
    response.append_header(restinio::http_field::access_control_allow_methods, methods);
    response.append_header(restinio::http_field::access_control_allow_headers, "content-type");
    response.append_header(restinio::http_field::access_control_max_age, "86400");
    return response.done();
}

RequestStatus
DhtProxyServer::getFiltered(restinio::request_handle_t request,
                            restinio::router::route_params_t params)
{
    requestNum_++;
    auto value = params["value"].to_string();
    dht::InfoHash infoHash(params["hash"].to_string());
    if (!infoHash)
        infoHash = dht::InfoHash::get(params["hash"].to_string());

    if (!dht_){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_service_unavailable()));
        response.set_body(this->RESP_MSG_SERVICE_UNAVAILABLE);
        return response.done();
    }

    auto response = std::make_shared<ResponseByPartsBuilder>(
        this->initHttpResponse(request->create_response<ResponseByParts>()));
    response->flush();
    try {
        dht_->get(infoHash, [this, response](const dht::Sp<dht::Value>& value){
            auto output = Json::writeString(jsonBuilder_, value->toJson()) + "\n";
            response->append_chunk(output);
            response->flush();
            return true;
        },
        [response] (bool /*ok*/){
            response->done();
        },
            {}, value
        );
    } catch (const std::exception& e){
        auto response = this->initHttpResponse(
            request->create_response(restinio::status_internal_server_error()));
        response.set_body(this->RESP_MSG_INTERNAL_SERVER_ERRROR);
        return response.done();
    }
    return restinio::request_handling_status_t::accepted;
}

}
