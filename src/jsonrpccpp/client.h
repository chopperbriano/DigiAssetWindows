/**
 * @file json-rpc.h
 * @date 02.08.2011
 * @author Peter Spiess-Knafl
 * @brief json-rpc.h
 *
 * This file is meant to include all necessary .h files of this framework.
 *
 * Umbrella header for the libjson-rpc-cpp client layer. The node and pool
 * server include this to drive JSON-RPC over HTTP against DigiByte Core's
 * wallet/chain RPC endpoint. Pulls in the Client class and the RPC exception
 * type; no logic of its own.
 */

#ifndef JSONRPCCPP_CLIENT_H_
#define JSONRPCCPP_CLIENT_H_

#include <jsonrpccpp/client/client.h>
#include <jsonrpccpp/common/exception.h>

#endif /* JSONRPCCPP_CLIENT_H_ */
