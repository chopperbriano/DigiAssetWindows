/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    specificationparser.h
 * @date    12.03.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * Declares SpecificationParser: reads a JSON-RPC procedure-specification
 * (from a file or an in-memory string) and turns it into a list of
 * Procedure objects the RPC server can register. Part of the bundled
 * libjson-rpc-cpp library used by the node/pool server RPC layer. All
 * members are static; the class is a stateless utility namespace.
 */

#ifndef JSONRPC_CPP_SPECIFICATIONPARSER_H
#define JSONRPC_CPP_SPECIFICATIONPARSER_H

#include "exception.h"
#include "procedure.h"

namespace jsonrpc {

  /**
   * Stateless helper that parses a JSON-RPC specification document
   * (an array of procedure declarations) into Procedure objects.
   * Throws JsonRpcException on missing files or malformed/invalid specs.
   */
  class SpecificationParser {
  public:
    // Load a spec file from disk and return its procedures; throws if the file is missing or invalid.
    static std::vector<Procedure> GetProceduresFromFile(const std::string &filename);
    // Parse a spec held in a string and return its procedures; throws on syntax/duplicate-name errors.
    static std::vector<Procedure> GetProceduresFromString(const std::string &spec);

    // Read the entire file at filename into target; throws ERROR_...NOT_FOUND if it cannot be opened.
    static void GetFileContent(const std::string &filename, std::string &target);

  private:
    // Populate target from one procedure-declaration JSON object (name, method/notification type, params).
    static void GetProcedure(Json::Value &val, Procedure &target);
    static void GetMethod(Json::Value &val, Procedure &target);
    static void GetNotification(Json::Value &val, Procedure &target);
    // Map a JSON value's runtime type to the framework's jsontype_t; throws on an unknown type.
    static jsontype_t toJsonType(Json::Value &val);

    // Read an array-style params block, naming parameters param01, param02, ... in order.
    static void GetPositionalParameters(Json::Value &val, Procedure &target);
    // Read an object-style params block, keeping each parameter's declared name.
    static void GetNamedParameters(Json::Value &val, Procedure &target);
    // Extract the procedure name, accepting the current "name" key or legacy "method"/"notification" keys; "" if none.
    static std::string GetProcedureName(Json::Value &signature);
  };
} // namespace jsonrpc
#endif // JSONRPC_CPP_SPECIFICATIONPARSER_H
