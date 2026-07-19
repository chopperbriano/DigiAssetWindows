//
// Created by DigiAsset Core on 19/07/26.
//

#ifndef DIGIASSET_CORE_EVENTBROADCASTER_H
#define DIGIASSET_CORE_EVENTBROADCASTER_H

#include <boost/asio.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * Pushes daemon events to any connected TCP client as newline delimited JSON.
 * Connect(default port 14025, config key "eventport", 0 disables), read lines:
 *    {"event":"newBlock","height":23842000,"blocksBehind":0}
 * Writes never block the caller: clients that can't keep up are disconnected.
 * Consumers needing reliability should treat the stream as a wake up signal and
 * re-query the RPC interface for state.
 */
class EventBroadcaster {
private:
    static EventBroadcaster* _pinstance;
    static std::mutex _mutex;

    boost::asio::io_context _io;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> _acceptor;
    std::vector<std::shared_ptr<boost::asio::ip::tcp::socket>> _clients;
    std::mutex _clientsMutex;
    std::thread _acceptThread;
    bool _running = false;

    EventBroadcaster() = default;

public:
    static EventBroadcaster* GetInstance();

    void start(unsigned int port); //0 = disabled
    void stop();
    ~EventBroadcaster();

    ///sends one event line to every connected client(never blocks, drops slow clients)
    void broadcast(const std::string& jsonLine);

    EventBroadcaster(const EventBroadcaster&) = delete;
    void operator=(const EventBroadcaster&) = delete;
};

#endif //DIGIASSET_CORE_EVENTBROADCASTER_H
