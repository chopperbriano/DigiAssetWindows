/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    specificationwriter.h
 * @date    30.04.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * Declares SpecificationWriter: the inverse of SpecificationParser, it
 * serializes a set of Procedure objects back into the JSON-RPC
 * procedure-specification format (as a JSON value, a string, or a file).
 * Part of the bundled libjson-rpc-cpp library used by the node/pool server
 * RPC layer; all members are static (stateless utility class).
 */

#ifndef JSONRPC_CPP_SPECIFICATIONWRITER_H
#define JSONRPC_CPP_SPECIFICATIONWRITER_H

#include "procedure.h"
#include "specification.h"

namespace jsonrpc {
  /**
   * Stateless helper that renders Procedure objects as a JSON-RPC
   * specification document (array of procedure declarations).
   */
  class SpecificationWriter {
  public:
    // Build a JSON array value describing the given procedures.
    static Json::Value toJsonValue(const std::vector<Procedure> &procedures);
    // Render the procedures to a pretty-printed (two-space indented) JSON spec string.
    static std::string toString(const std::vector<Procedure> &procedures);
    // Write the spec string to filename; returns false if the file could not be opened.
    static bool toFile(const std::string &filename, const std::vector<Procedure> &procedures);

  private:
    // Produce a representative example JSON value for a given declared type (used as the type marker in the spec).
    static Json::Value toJsonLiteral(jsontype_t type);
    // Serialize one Procedure into target: name, "returns" example (methods only), and example params.
    static void procedureToJsonValue(const Procedure &procedure, Json::Value &target);
  };
} // namespace jsonrpc

#endif // JSONRPC_CPP_SPECIFICATIONWRITER_H
