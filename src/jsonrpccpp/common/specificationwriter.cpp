/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    specificationwriter.cpp
 * @date    30.04.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * Implements SpecificationWriter: serializes Procedure objects into the
 * JSON-RPC procedure-specification format. Part of the bundled
 * libjson-rpc-cpp library backing the node/pool server RPC interfaces.
 */

#include "specificationwriter.h"
#include "jsonparser.h"
#include <fstream>
#include <iostream>
#include <jsonrpccpp/common/jsonparser.h>

using namespace std;
using namespace jsonrpc;

// Build a JSON array in which each element is the serialized form of one procedure.
Json::Value SpecificationWriter::toJsonValue(const vector<Procedure> &procedures) {
  Json::Value result;
  Json::Value row;
  for (unsigned int i = 0; i < procedures.size(); i++) {
    procedureToJsonValue(procedures.at(i), row);
    result[i] = row;
    row.clear();
  }
  return result;
}
// Serialize the procedures to a pretty-printed JSON string using two-space indentation.
std::string SpecificationWriter::toString(const vector<Procedure> &procedures) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "  ";
  return Json::writeString(wb, toJsonValue(procedures));
}
// Write the serialized spec to the file at filename (truncating it). Returns false if the file could not
// be opened, true on success.
bool SpecificationWriter::toFile(const std::string &filename, const vector<Procedure> &procedures) {
  ofstream file;
  file.open(filename.c_str(), ios_base::out);
  if (!file.is_open())
    return false;
  file << toString(procedures);
  file.close();
  return true;
}
// Return a representative example JSON value for the given declared type (e.g. "somestring" for a string,
// 1 for an integer, a sample key/value object for an object). Used as the type placeholder in emitted specs.
Json::Value SpecificationWriter::toJsonLiteral(jsontype_t type) {
  Json::Value literal;
  switch (type) {
  case JSON_BOOLEAN:
    literal = true;
    break;
  case JSON_STRING:
    literal = "somestring";
    break;
  case JSON_REAL:
    literal = 1.0;
    break;
  case JSON_NUMERIC:
    literal = 1.0;
    break;
  case JSON_ARRAY:
    literal = Json::arrayValue;
    break;
  case JSON_OBJECT:
    literal["objectkey"] = "objectvalue";
    break;
  case JSON_INTEGER:
    literal = 1;
    break;
  }
  return literal;
}
// Serialize one procedure into target: always writes the name; adds a "returns" example value only for
// RPC_METHOD procedures; and emits its parameters either as a named object or a positional array,
// depending on the procedure's parameter-declaration style, using example literals for each type.
void SpecificationWriter::procedureToJsonValue(const Procedure &procedure, Json::Value &target) {
  target[KEY_SPEC_PROCEDURE_NAME] = procedure.GetProcedureName();
  if (procedure.GetProcedureType() == RPC_METHOD) {
    target[KEY_SPEC_RETURN_TYPE] = toJsonLiteral(procedure.GetReturnType());
  }
  for (parameterNameList_t::const_iterator it = procedure.GetParameters().begin(); it != procedure.GetParameters().end(); ++it) {
    if (procedure.GetParameterDeclarationType() == PARAMS_BY_NAME) {
      target[KEY_SPEC_PROCEDURE_PARAMETERS][it->first] = toJsonLiteral(it->second);
    } else {
      target[KEY_SPEC_PROCEDURE_PARAMETERS].append(toJsonLiteral(it->second));
    }
  }
}
