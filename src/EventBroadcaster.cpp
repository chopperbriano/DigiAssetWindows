//
// Created by DigiAsset Core on 19/07/26.
//

#include "EventBroadcaster.h"
#include "Log.h"

using boost::asio::ip::tcp;
using namespace std;

EventBroadcaster* EventBroadcaster::_pinstance = nullptr;
std::mutex EventBroadcaster::_mutex;

EventBroadcaster* EventBroadcaster::GetInstance() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_pinstance == nullptr) _pinstance = new EventBroadcaster();
    return _pinstance;
}

void EventBroadcaster::start(unsigned int port) {
    if (_running || (port == 0)) return;
    try {
        _acceptor = std::unique_ptr<tcp::acceptor>(new tcp::acceptor(_io, tcp::endpoint(tcp::v4(), port)));
    } catch (const std::exception& e) {
        Log* log = Log::GetInstance();
        log->addMessage("Event stream could not bind port " + to_string(port) + ": " + e.what(), Log::WARNING);
        return;
    }
    _running = true;
    _acceptThread = std::thread([this]() {
        while (_running) {
            try {
                auto socket = std::make_shared<tcp::socket>(_io);
                _acceptor->accept(*socket);
                socket->non_blocking(true); //writes must never block the daemon
                std::lock_guard<std::mutex> lock(_clientsMutex);
                _clients.push_back(socket);
            } catch (const std::exception& e) {
                if (_running) std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
    Log::GetInstance()->addMessage("Event stream listening on port " + to_string(port));
}

void EventBroadcaster::stop() {
    if (!_running) return;
    _running = false;
    try {
        _acceptor->close();
    } catch (...) {}
    if (_acceptThread.joinable()) _acceptThread.join();
    std::lock_guard<std::mutex> lock(_clientsMutex);
    _clients.clear();
}

EventBroadcaster::~EventBroadcaster() {
    stop();
}

void EventBroadcaster::broadcast(const std::string& jsonLine) {
    if (!_running) return;
    std::string data = jsonLine + "\n";
    std::lock_guard<std::mutex> lock(_clientsMutex);
    for (auto it = _clients.begin(); it != _clients.end();) {
        boost::system::error_code error;
        boost::asio::write(**it, boost::asio::buffer(data), error);
        if (error) {
            //client gone or too slow(non blocking socket would_block) - drop it
            it = _clients.erase(it);
        } else {
            ++it;
        }
    }
}
