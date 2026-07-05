/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    redisserver.cpp
 * @date    15.08.2017
 * @author  Jacques Software <software@jacques.com.au>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * ROLE IN DIGIASSET FOR WINDOWS:
 * Implementation of RedisServer, the Redis-queue JSON-RPC connector from
 * vendored libjson-rpc-cpp. A background pthread blocks on BRPOP of the
 * configured queue; each message encodes "<reply-queue>!<json-request>",
 * which is parsed, dispatched to the base handler, and the result LPUSH-ed
 * back onto the reply queue. Depends on hiredis; not built into the Windows
 * node or pool server, retained for upstream parity.
 */

#include "redisserver.h"

using namespace jsonrpc;

/**
 * This is a helper method for the ListenLoop. Checks that the request is
 * valid and then retusn the request string and the queue to return the
 * response to.
 * @param req Redis request that has been received.
 * @param ret_queue The return queue is returned here.
 * @param request The request is returned here.
 * @return Returns true on success, false otherwise.
 */
bool ProcessRedisReply(redisReply *req, std::string &ret_queue, std::string &request) {

  // The return from hiredis is strange in that it's always an array of
  // length 2, with the first element the name of the key as a string,
  // and the second element the actual element that we popped.
  if (req->type != REDIS_REPLY_ARRAY) {
    return false;
  }

  if (req->elements != 2) {
    return false;
  }

  // It's the second element that we care about
  redisReply *data = req->element[1];

  // It should be a json string
  if (data->type != REDIS_REPLY_STRING) {
    return false;
  }

  std::string json(data->str, data->str + data->len * sizeof data->str[0]);

  size_t pos = json.find("!");
  if (pos == std::string::npos) {
    return false;
  }

  ret_queue = json.substr(0, pos);
  request = json.substr(pos + 1);

  return true;
}

/**
 * @brief Stores connection settings; does not yet connect (see StartListening).
 * @param host Redis server address. @param port Redis port. @param queue List to consume requests from.
 */
RedisServer::RedisServer(std::string host, int port, std::string queue) : running(false), host(host), port(port), queue(queue), con(NULL) {}

/**
 * @brief Connects to Redis and spawns the background listening thread.
 *
 * No-op returning the current state if already running. Establishes the
 * hiredis connection and, on success, starts ListenLoop on a new pthread.
 * @returns true if listening (connected and thread started), false otherwise.
 */
bool RedisServer::StartListening() {
  if (this->running) {
    return this->running;
  }

  con = redisConnect(host.c_str(), port);
  if (con == NULL) {
    return false;
  }
  if (con->err != 0) {
    redisFree(con);
    con = NULL;
    return false;
  }

  this->running = true;
  int ret = pthread_create(&(this->listenning_thread), NULL, RedisServer::LaunchLoop, this);
  this->running = static_cast<bool>(ret == 0);

  return this->running;
}

/**
 * @brief Signals the listen loop to stop, joins the thread, and frees the connection.
 * @returns true once stopped (also true if it was not running).
 */
bool RedisServer::StopListening() {
  if (!this->running) {
    return true;
  }
  this->running = false;
  pthread_join(this->listenning_thread, NULL);
  if (con != NULL) {
    redisFree(con);
  }
  return !(this->running);
}

/**
 * @brief LPUSH-es the JSON-RPC response onto the client's reply queue.
 * @param response The response payload. @param ret_queue The reply queue name.
 * @returns true if Redis acknowledged the push (integer reply > 0), false otherwise.
 */
bool RedisServer::SendResponse(const std::string &response, const std::string &ret_queue) {
  redisReply *ret;
  ret = (redisReply *)redisCommand(con, "LPUSH %s %s", ret_queue.c_str(), response.c_str());

  if (ret == NULL) {
    return false;
  }

  if (ret->type != REDIS_REPLY_INTEGER || ret->integer <= 0) {
    freeReplyObject(ret);
    return false;
  }

  freeReplyObject(ret);
  return true;
}

/**
 * @brief pthread entry point: casts p_data back to a RedisServer and runs ListenLoop.
 * @param p_data Pointer to the RedisServer instance. @returns NULL.
 */
void *RedisServer::LaunchLoop(void *p_data) {
  RedisServer *instance = reinterpret_cast<RedisServer *>(p_data);
  ;
  instance->ListenLoop();
  return NULL;
}

/**
 * @brief Background loop: while running, BRPOP-s a request (1s block) off the
 *        queue, skips nil/malformed messages, dispatches valid requests via
 *        ProcessRequest, and sends the response back with SendResponse. Frees
 *        every hiredis reply it receives.
 */
void RedisServer::ListenLoop() {
  while (this->running) {
    redisReply *req = NULL;
    std::string request;
    req = (redisReply *)redisCommand(con, "BRPOP %s 1", queue.c_str());
    if (req == NULL) {
      continue;
    }

    if (req->type == REDIS_REPLY_NIL) {
      freeReplyObject(req);
      continue;
    }

    std::string ret_queue;
    bool ret = ProcessRedisReply(req, ret_queue, request);
    freeReplyObject(req);
    if (ret == false) {
      continue;
    }

    if (this->running) {
      std::string response;
      this->ProcessRequest(request, response);
      this->SendResponse(response, ret_queue);
    }
  }
}
