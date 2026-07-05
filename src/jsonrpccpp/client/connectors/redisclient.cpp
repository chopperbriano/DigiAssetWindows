/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    redisclient.cpp
 * @date    12.08.2017
 * @author  Jacques Software <software@jacques.com.au>
 * @license See attached LICENSE.txt
 ************************************************************************/

// ---------------------------------------------------------------------------
// Role in DigiAsset for Windows:
//   Implements RedisClient, a Redis-backed transport for the bundled
//   libjson-rpc-cpp JSON-RPC client. Instead of a direct socket, a request is
//   pushed onto a shared Redis list (LPUSH) with a unique reply-queue name
//   prepended, and the response is awaited by blocking-popping that reply
//   queue (BRPOP). Requires hiredis and POSIX headers (<sys/time.h>), so it is
//   a Linux/UNIX-only connector, not part of the Windows node or pool-server
//   build; it ships to keep the vendored library source complete.
// ---------------------------------------------------------------------------

#include "redisclient.h"

#include <iostream>
#include <stdlib.h>
#include <string>
#include <sys/time.h>
#include <time.h>

using namespace jsonrpc;

/**
 * Generate a random string.
 * @param s Buffer for the random string.
 * @param len The length of the random string.
 */
void genRandom(char *s, int len) {
  static const char alphanum[] = "0123456789"
                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "abcdefghijklmnopqrstuvwxyz";

  for (int i = 0; i < len; ++i) {
    s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
  }

  s[len] = 0;
}

/**
 * Helper function for SendRpcMessage which processes the hiredis reply
 * object and determines if it's valid and extracts the result.
 * @param reply The reply object.
 * @param result The result is returned here.
 */
void ProcessReply(redisReply *reply, std::string &result) {

  // The return from hiredis is strange in that it's always an array of
  // length 2, with the first element the name of the key as a string,
  // and the second element the actual element that we popped.
  if (reply->type != REDIS_REPLY_ARRAY) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Item not an array");
  }

  if (reply->elements != 2) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Item needs two elements");
  }

  // It's the second element that we care about
  redisReply *data = reply->element[1];

  // It should be a json string
  if (data->type != REDIS_REPLY_STRING) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Item not a string");
  }

  std::string json(data->str, data->str + data->len * sizeof data->str[0]);
  result = json;
}

/**
 * Generates a unique random name for the return queue.
 * @param con Our redis context.
 * @param prefix Prefix for the queue name.
 * @param ret_queue The name is returned here.
 */
void jsonrpc::GetReturnQueue(redisContext *con, const std::string &prefix, std::string &ret_queue) {
  char id[17];
  std::stringstream str;
  genRandom(id, 16);
  str << prefix << "_" << id;
  ret_queue = str.str();

  redisReply *reply = (redisReply *)redisCommand(con, "EXISTS %s", ret_queue.c_str());
  if (reply == NULL) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "redis error: Failed to run queue check");
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    freeReplyObject(reply);
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "redis error: Failed to run queue check");
  }

  if (reply->integer != 0) {
    freeReplyObject(reply);
    // If we are really unlucky and the queue already exists then we try again.
    GetReturnQueue(con, prefix, ret_queue);
    return;
  }
  freeReplyObject(reply);
}

/**
 * @brief Constructs the client and opens a connection to the redis server.
 *
 * Sets the default response timeout to 10 seconds, connects via redisConnect(),
 * and seeds the C rand() generator (used for reply-queue names) from the current
 * time and microseconds.
 * @param host  The redis server host/IP.
 * @param port  The redis server port.
 * @param queue The request queue name to push messages onto.
 * @throw JsonRpcException if the connection cannot be created or reports an error.
 */
RedisClient::RedisClient(const std::string &host, int port, const std::string &queue) : queue(queue), con(NULL) {
  this->timeout = 10;

  con = redisConnect(host.c_str(), port);
  if (con == NULL) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "redis error: Failed to connect");
  }

  if (con->err) {
    std::stringstream err;
    err << "redis error: " << con->err;
    redisFree(con);
    con = NULL;
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, err.str());
  }

  struct timeval tv;
  gettimeofday(&tv, NULL);
  srand(time(NULL) + tv.tv_usec);
}

RedisClient::~RedisClient() {
  if (con != NULL) {
    redisFree(con);
  }
}

/**
 * @brief Sends a JSON-RPC request through redis and waits for the response.
 *
 * Generates a unique reply queue, LPUSHes "<ret_queue>!<message>" onto the
 * request queue, then BRPOPs the reply queue for up to `timeout` seconds and
 * extracts the JSON payload from the popped element into result.
 * @param message The JSON-RPC request to send.
 * @param result  Receives the JSON-RPC response popped from the reply queue.
 * @throw JsonRpcException on redis errors, a rejected LPUSH, or a BRPOP timeout.
 */
void RedisClient::SendRPCMessage(const std::string &message, std::string &result) {
  std::string ret_queue;
  GetReturnQueue(con, queue, ret_queue);

  redisReply *ret;
  std::string data = ret_queue + "!" + message;
  ret = (redisReply *)redisCommand(con, "LPUSH %s %s", queue.c_str(), data.c_str());

  if (ret == NULL) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Unknown error while sending request");
  }
  if (ret->type != REDIS_REPLY_INTEGER || ret->integer <= 0) {
    freeReplyObject(ret);
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Error while sending request, queue not updated");
  }

  freeReplyObject(ret);

  redisReply *reply = NULL;
  reply = (redisReply *)redisCommand(con, "BRPOP %s %d", ret_queue.c_str(), this->timeout);
  if (reply == NULL) {
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Unknown error while getting response");
  }
  if (reply->type == REDIS_REPLY_NIL) {
    freeReplyObject(reply);
    throw JsonRpcException(Errors::ERROR_CLIENT_CONNECTOR, "Operation timed out");
  }

  ProcessReply(reply, result);
  freeReplyObject(reply);
}

void RedisClient::SetQueue(const std::string &queue) { this->queue = queue; }

void RedisClient::SetTimeout(long timeout) { this->timeout = timeout; }
