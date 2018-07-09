// A converter for ONNX models between different opset versions

#pragma once

#include "onnx/common/ir.h"
#include "onnx/common/ir_pb_converter.h"
#include "onnx/common/stl_backports.h"
#include "onnx/proto_utils.h"
#include "onnx/defs/schema.h"
#include <utility>
#include <iostream>
// TODO: Remove this import once actual adapters are imported
#include "onnx/version_converter/adapters/adapter.h"

namespace ONNX_NAMESPACE { namespace version_conversion {

struct VersionConverter {
  // Schema for adapters: {<op_name>$<from_domain><from_version>$<to_domain>
  // <to_version>: adapter}
  std::map<std::string, std::map<std::string, std::map<std::string, Adapter*>>> adapters;

  std::unordered_map<Node*, OpSchema> current_opschemas;

  VersionConverter() {
    // TODO: Register adapters to the version converter
  }

  virtual ~VersionConverter() = default;

  Adapter* adapter_lookup(Node* op,
      const OpSetID& initial_version,
      const OpSetID& target_verion);

  ONNX_NAMESPACE::ModelProto convert_version(
      const ONNX_NAMESPACE::ModelProto& mp_in,
      const OpSetID initial_version,
      const OpSetID target_version) {
    std::shared_ptr<ONNX_NAMESPACE::Graph> g(ONNX_NAMESPACE::ImportModelProto(mp_in));

    if (g.get() == nullptr) {
      std::cerr << "Warning: onnx version converter is unable to parse input model. "
        << "(The IR version of the ONNX model may be too old.)" << std::endl;
      // If we can't parse the file, just return the input.
      return mp_in;
    }

    std::string initial_domain = initial_version.domain;
    std::string target_domain = target_version.domain;
    if ((initial_domain != "" && initial_domain != "ai.onnx") || (target_domain !=
        "" && target_domain != "ai.onnx")) {
      std::cerr << "Warning: default onnx version converter can only convert "
        << " between default domain opset versions ('' or 'ai.onnx')" << std::endl;
      std::cerr << "Provided initial_domain: " << initial_domain <<
        ", provided target_domain: " << target_domain << std::endl;
      return mp_in;
    }

    ONNX_NAMESPACE::ModelProto mp_out = PrepareOutput(mp_in);

    // TODO: Move to Inter-Domain Converter
    // Get initial model versions
    // std::vector<OpSetID> initial_versions = g->opset_versions;

    // No conversion necessary if Model has single, equivalent opset version
    // if (initial_versions.size() == 1 && initial_versions[0].version ==
    //    target_version.version && initial_versions[0].domain ==
    //    target_version.domain) {
    //  return mp_in;
    // }

    // Check if target_version is valid
    const std::unordered_map<std::string, std::pair<int, int>>& versions_map = OpSchemaRegistry::DomainToVersionRange::Instance().Map();
    std::string search_domain = target_version.domain;
    if (target_version.domain == "ai.onnx") {
      search_domain = "";
    }
    std::pair<int, int> version_range = versions_map.at(search_domain);
    if (target_version.version < version_range.first || target_version.version > version_range.second) {
      // Invalid target_version
      std::cerr << "Warning: invalid target_version (must be between "
        << version_range.first << " and " << version_range.second << std::endl;
      return mp_in;
    }

    // Compile list of all ops used in the model
    graph_node_list nodes = g->nodes();

    std::vector<OpSchema> all_opschemas = ONNX_NAMESPACE::OpSchemaRegistry::get_all_schemas_with_history();

    // Create Map for All Versions
    std::unordered_map<std::basic_string<char>, std::unordered_map<std::basic_string<char>, std::map<int, ONNX_NAMESPACE::OpSchema>>>  all_schemas;

    for (OpSchema schema : all_opschemas) {
      all_schemas[schema.Name()][schema.domain()][schema.since_version()] = schema;
    }

    // Create Map for Current Version
    for (Node* op : nodes) {
      // Iterate through all OperatorSetVersions, select highest that is leq initial_version
      int op_opset_version = -1;
      // TODO: Check whether this process accidentally always defaults to initial_version
      // TODO: If so, just take the SinceVersion of this schema (which returns the implementation version)
      auto op_domain_map = all_schemas[op->kind().toString()];
      if (op_domain_map.find(initial_domain) != op_domain_map.end()) {
        // If op isn't defined for initial domain, we won't convert it
        for (const auto& version_pair : op_domain_map[initial_domain]) {
          if (version_pair.first > op_opset_version && version_pair.first <= initial_version.version) {
            op_opset_version = version_pair.first;
            current_opschemas[op] = op_domain_map[initial_domain][op_opset_version];
          }
        }
      }
    }

    // Iterate over all versions to target_version for specified
    int curr_version = initial_version.version;
    int next_version;
    if (target_version.version > initial_version.version) {
      curr_version++;
      next_version = curr_version + 1;
    } else {
      next_version = curr_version - 1;
    }
    // Identify index of this domain in g.opset_versions
    int domain_index = 0;
    for (int i = 0; i < g->opset_versions.size(); i++) {
      if (g->opset_versions[i].domain == "") {
        domain_index = i;
      }
    }
    while (curr_version != target_version.version) {
      std::cerr << "curr_version: " << curr_version << ", next_version: " << next_version << std::endl;
      // Iterate through and call adapter returned by adapter_lookup for ops from current_version opset
      for (Node* op : nodes) {
        auto op_domain_map = all_schemas.at(op->kind().toString());
        if (op_domain_map.find("") != op_domain_map.end() &&
            op_domain_map[""].find(curr_version) !=
            op_domain_map[""].end()) {
          // Op is specifically defined for this domain and version
          OpSetID curr_id;
          OpSetID next_id;
          curr_id.domain = "";
          next_id.domain = "";
          curr_id.version = curr_version;
          next_id.version = next_version;
          auto op_adapter = adapter_lookup(op, curr_id, next_id);
          // If adapter_lookup returns null, no adapter is present.  Error out
          if (op_adapter == NULL) {
            // TODO: Verify that conversion is actually needed (that the operator
            // isn't already optimal, which should be caught by the above condition)
            std::cerr << "No adapter is present for " << op->kind().toString()
              << " in default domain. Please implement one and try again." << std::endl;
            return mp_in;
          } else {
            std::cerr << "Applying adapter" << std::endl;
            // adapt should handle replacing node in graph
            op_adapter->adapt(*g);
          }
        }
      }
      // Update model version
      if (target_version.version > initial_version.version) {
        curr_version++;
        next_version++;
        g->opset_versions[domain_index].version++;
      } else {
        curr_version--;
        next_version--;
        g->opset_versions[domain_index].version--;
      }
    }
    // Export g as ModelProto
    std::cerr << "Finished conversion; returning model\n";
    ExportModelProto(&mp_out, g);
    return mp_out;
  }

  void registerAdapter(Adapter* a_ptr, std::string domain) {
    OpSetID iv = a_ptr->initial_version;
    OpSetID tv = a_ptr->target_version;
    adapters[a_ptr->name][stringify_opsetid(iv)][stringify_opsetid(tv)] = a_ptr;
  }

  std::string stringify_opsetid(OpSetID target);

  std::vector<std::string> destringify_opsetid(std::string target);
};

ONNX_NAMESPACE::ModelProto ConvertVersion(
    const ONNX_NAMESPACE::ModelProto& mp_in,
    const OpSetID initial_version,
    const OpSetID target_version);
}}
