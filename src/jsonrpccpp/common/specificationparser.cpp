/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    specificationparser.cpp
 * @date    12.03.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

/*
 * Implements SpecificationParser: converts a JSON-RPC procedure-spec
 * document into Procedure objects for the RPC server. Part of the bundled
 * libjson-rpc-cpp library backing the node/pool server RPC interfaces.
 */

#include "specificationparser.h"
#include <fstream>
#include <iomanip>
#include <jsonrpccpp/common/jsonparser.h>

using namespace std;
using namespace jsonrpc;

// Read the spec file's text and delegate to GetProceduresFromString.
vector<Procedure> SpecificationParser::GetProceduresFromFile(const string &filename) {
  string content;
  GetFileContent(filename, content);
  return GetProceduresFromString(content);
}
// Parse spec text (must be a top-level JSON array), build one Procedure per element, and
// enforce that procedure names are unique. Throws JsonRpcException on JSON syntax errors,
// a non-array root, or a duplicate procedure name.
vector<Procedure> SpecificationParser::GetProceduresFromString(const string &content) {

  Json::Value val;

  try {
    std::istringstream(content) >> val;
  } catch (Json::Exception &e) {
    throw JsonRpcException(Errors::ERROR_RPC_JSON_PARSE_ERROR, " specification file contains syntax errors");
  }
  if (!val.isArray()) {
    throw JsonRpcException(Errors::ERROR_SERVER_PROCEDURE_SPECIFICATION_SYNTAX, " top level json value is not an array");
  }

  vector<Procedure> result;
  map<string, Procedure> procnames;
  for (unsigned int i = 0; i < val.size(); i++) {
    Procedure proc;
    GetProcedure(val[i], proc);
    if (procnames.find(proc.GetProcedureName()) != procnames.end()) {
      throw JsonRpcException(Errors::ERROR_SERVER_PROCEDURE_SPECIFICATION_SYNTAX, "Procedurename not unique: " + proc.GetProcedureName());
    }
    procnames[proc.GetProcedureName()] = proc;
    result.push_back(proc);
  }
  return result;
}
// Fill result from a single procedure-declaration object: sets the name; treats the presence of a
// "returns" key as an RPC_METHOD (with that return type) and its absence as an RPC_NOTIFICATION; and
// reads any params block as positional (JSON array) or named (JSON object). Throws if the object has
// no name, or if the params field is neither array nor object.
void SpecificationParser::GetProcedure(Json::Value &signature, Procedure &result) {
  if (signature.isObject() && !GetProcedureName(signature).empty()) {
    result.SetProcedureName(GetProcedureName(signature));
    if (signature.isMember(KEY_SPEC_RETURN_TYPE)) {
      result.SetProcedureType(RPC_METHOD);
      result.SetReturnType(toJsonType(signature[KEY_SPEC_RETURN_TYPE]));
    } else {
      result.SetProcedureType(RPC_NOTIFICATION);
    }
    if (signature.isMember(KEY_SPEC_PROCEDURE_PARAMETERS)) {
      if (signature[KEY_SPEC_PROCEDURE_PARAMETERS].isObject() || signature[KEY_SPEC_PROCEDURE_PARAMETERS].isArray()) {
        if (signature[KEY_SPEC_PROCEDURE_PARAMETERS].isArray()) {
          result.SetParameterDeclarationType(PARAMS_BY_POSITION);
          GetPositionalParameters(signature, result);
        } else if (signature[KEY_SPEC_PROCEDURE_PARAMETERS].isObject()) {
          result.SetParameterDeclarationType(PARAMS_BY_NAME);
          GetNamedParameters(signature, result);
        }
      } else {
        throw JsonRpcException(Errors::ERROR_SERVER_PROCEDURE_SPECIFICATION_SYNTAX, "Invalid signature types in fileds: " + signature.toStyledString());
      }
    }
  } else {
    throw JsonRpcException(Errors::ERROR_SERVER_PROCEDURE_SPECIFICATION_SYNTAX,
                           "procedure declaration does not contain name or parameters: " + signature.toStyledString());
  }
}
// Slurp the whole file at filename into target. Throws ERROR_SERVER_PROCEDURE_SPECIFICATION_NOT_FOUND
// if the file cannot be opened.
void SpecificationParser::GetFileContent(const std::string &filename, std::string &target) {
  ifstream config(filename.c_str());

  if (config) {
    config.open(filename.c_str(), ios::in);
    target.assign((std::istreambuf_iterator<char>(config)), (std::istreambuf_iterator<char>()));
  } else {
    throw JsonRpcException(Errors::ERROR_SERVER_PROCEDURE_SPECIFICATION_NOT_FOUND, filename);
  }
}
// Map the runtime type of a JSON value to the framework's jsontype_t (used to record the declared type
// of a parameter or return value). Throws on a value type it does not recognize.
jsontype_t SpecificationParser::toJsonType(Json::Value &val) {
  jsontype_t result;
  switch (val.type()) {
  case Json::uintValue:
  case Json::intValue:
    result = JSON_INTEGER;
    break;
  case Json::realValue:
    result = JSON_REAL;
    break;
  case Json::stringValue:
    result = JSON_STRING;
    break;
  case Json::booleanValue:
    result = JSON_BOOLEAN;
    break;
  case Json::arrayValue:
    result = JSON_ARRAY;
    break;
  case Json::objectValue:
    result = JSON_OBJECT;
    break;
  default:
    throw JsonRpcException(Errors::ERROR_SERVER_PROCEDURE_SPECIFICATION_SYNTAX, "Unknown parameter type: " + val.toStyledString());
  }
  return result;
}
// Add the array-style params to result as positional parameters, synthesizing names param01, param02, ...
// (1-based, zero-padded to two digits) and inferring each parameter's type from its example value.
void SpecificationParser::GetPositionalParameters(Json::Value &val, Procedure &result) {
  // Positional parameters
  for (unsigned int i = 0; i < val[KEY_SPEC_PROCEDURE_PARAMETERS].size(); i++) {
    stringstream paramname;
    paramname << "param" << std::setfill('0') << std::setw(2) << (i + 1);
    result.AddParameter(paramname.str(), toJsonType(val[KEY_SPEC_PROCEDURE_PARAMETERS][i]));
  }
}
// Add the object-style params to result as named parameters, keeping each member key as the parameter
// name and inferring its type from the member's example value.
void SpecificationParser::GetNamedParameters(Json::Value &val, Procedure &result) {
  vector<string> parameters = val[KEY_SPEC_PROCEDURE_PARAMETERS].getMemberNames();
  for (unsigned int i = 0; i < parameters.size(); ++i) {
    result.AddParameter(parameters.at(i), toJsonType(val[KEY_SPEC_PROCEDURE_PARAMETERS][parameters.at(i)]));
  }
}

// Return the procedure's declared name, preferring the current "name" key and falling back to the legacy
// "method" then "notification" keys; returns "" when none is present as a string.
string SpecificationParser::GetProcedureName(Json::Value &signature) {
  if (signature[KEY_SPEC_PROCEDURE_NAME].isString())
    return signature[KEY_SPEC_PROCEDURE_NAME].asString();

  if (signature[KEY_SPEC_PROCEDURE_METHOD].isString())
    return signature[KEY_SPEC_PROCEDURE_METHOD].asString();

  if (signature[KEY_SPEC_PROCEDURE_NOTIFICATION].isString())
    return signature[KEY_SPEC_PROCEDURE_NOTIFICATION].asString();
  return "";
}
