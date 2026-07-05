/**
 * @file json-rpc.h
 * @date 02.08.2011
 * @author Peter Spiess-Knafl
 * @brief json-rpc.h
 *
 * This file is meant to include all necessary .h files of this framework.
 *
 * Umbrella convenience header for the libjson-rpc-cpp server side: pulling in
 * this one header gives consumers the exception type and the AbstractServer
 * base used to expose JSON-RPC endpoints. In this project it backs the RPC
 * interfaces served by the node (DigiAssetWindows.exe) and the pool server.
 */

#ifndef JSONRPCCPP_SERVER_H_
#define JSONRPCCPP_SERVER_H_

#include <jsonrpccpp/common/exception.h>
#include <jsonrpccpp/server/abstractserver.h>

#endif /* JSONRPCCPP_SERVER_H_ */
