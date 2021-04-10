// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "core/optimizer/initializer.h"
#include "core/optimizer/propagate_cast_ops.h"
#include "core/graph/graph_utils.h"
#include "core/optimizer/utils.h"
#include <deque>

using namespace ONNX_NAMESPACE;
using namespace onnxruntime::common;
namespace onnxruntime {

std::unordered_set<std::string> fp16_allow = {"Transpose", "Reshape", "Gather", "Split", "Relu", "Where", "Dropout"};
std::unordered_set<std::string> fp16_safe = { "LayerNorm", "Gelu", "FastGelu", "Tanh", "MatMul", "MatAdd", "Add",
                                              "Sub", "Mul", "Div", "Neg", "Gemm", "FusedMatMul", "FusedGemm"};

// InsertCastNodes
// Insert a new Cast node after each NodeArg in the require_cast vector. The cast node is FLOAT16 if is_fp16 is True
// and FLOAT otherwise. This funtion fixes the graph edges in addition to inserting the cast nodes.
static Status InsertCastNodes(Graph& graph,
                              const std::unordered_set<NodeArg*>& require_cast,
                              bool is_fp16,
                              std::deque<onnxruntime::NodeIndex>& removed_nodes)
{
  //Create requirred new Cast nodes.
  for (NodeArg* node_arg : require_cast) {
    if (!node_arg->Exists()) {
      continue;
    }
    // data_type is the data type of the Cast output.
    TensorProto_DataType data_type = is_fp16 ? TensorProto_DataType_FLOAT16 : TensorProto_DataType_FLOAT;
    TypeProto type_proto;
    bool is_node_arg_cast_output = node_arg->TypeAsProto()->tensor_type().elem_type() == data_type;
    TensorProto_DataType new_node_arg_data_type = data_type;;
    if (is_node_arg_cast_output) {
      new_node_arg_data_type = (data_type == TensorProto_DataType_FLOAT) ? TensorProto_DataType_FLOAT16 : TensorProto_DataType_FLOAT;
    }
    type_proto.mutable_tensor_type()->set_elem_type(new_node_arg_data_type);
    NodeArg& new_node_arg = graph.GetOrCreateNodeArg(graph.GenerateNodeArgName(node_arg->Name()), &type_proto);
    // Expect that a NodeArg is not both a graph input as well as a graph output
    ORT_ENFORCE(!(graph.IsInputsIncludingInitializers(node_arg) && graph.IsOutput(node_arg)));
    NodeArg& cast_input = !is_node_arg_cast_output ? *node_arg : new_node_arg;
    NodeArg& cast_output = is_node_arg_cast_output ? *node_arg : new_node_arg;
    const std::vector<NodeArg*> cast_inputs = {&cast_input};
    const std::vector<NodeArg*> cast_outputs = {&cast_output};
    ONNX_NAMESPACE::AttributeProto to_attribute;
    to_attribute.set_name("to");
    to_attribute.set_type(ONNX_NAMESPACE::AttributeProto::INT);
    to_attribute.set_i(static_cast<int64_t>(data_type));
    NodeAttributes attributes({{"to", to_attribute}});

    Node& cast = graph.AddNode(graph.GenerateNodeName(node_arg->Name() + "_cast"),
                               "Cast",
                               "Created a new Cast node",
                               cast_inputs,
                               cast_outputs,
                               &attributes);
    Node* producer = graph.GetMutableProducerNode(node_arg->Name());
    std::vector<Node*> consumers = graph.GetMutableConsumerNodes(node_arg->Name());
    int output_index = (producer != nullptr) ? optimizer_utils::IndexOfNodeOutput(*producer, *node_arg) : -1;
    // Update consumers of node_arg to use the output of the cast node
    int cast_output_index = optimizer_utils::IndexOfNodeOutput(cast, cast_output);
    for (Node* consumer : graph.GetMutableConsumerNodes(node_arg->Name())) {
      if (consumer != nullptr &&
          std::find(removed_nodes.begin(), removed_nodes.end(), consumer->Index()) == removed_nodes.end()) {
        auto& consumer_inputs = consumer->MutableInputDefs();
        int input_index = optimizer_utils::IndexOfNodeInput(*consumer, *node_arg);
        if (producer != nullptr) {
          graph.RemoveEdge(producer->Index(), consumer->Index(), output_index, input_index);
        }
        std::replace(consumer_inputs.begin(), consumer_inputs.end(), &cast_input, &cast_output);
        graph.AddEdge(cast.Index(), consumer->Index(), cast_output_index, input_index);
      }
    }
    if (producer != nullptr) {
      auto& producer_outputs = producer->MutableOutputDefs();
      std::replace(producer_outputs.begin(), producer_outputs.end(), &cast_output, &cast_input);
      graph.UpdateProducerNode(cast_input.Name(), producer->Index());
      int input_index = optimizer_utils::IndexOfNodeInput(cast, cast_input);
      graph.AddEdge(producer->Index(), cast.Index(), output_index, input_index);
    }
    graph.UpdateProducerNode(cast_output.Name(), cast.Index());
  }
  return Status::OK();
}
// RemoveCastNodes
// Remove the cast nodes specified in casts vector and fix the graph edges accordingly.
static Status RemoveCastNodes(Graph& graph, std::vector<Node*> casts, std::deque<onnxruntime::NodeIndex>& removed_nodes)
{
  ORT_ENFORCE(casts.size()>0);
  Node* lead_cast = casts.front();
  Node* trail_cast = casts.back();
  NodeArg* cast_input = lead_cast->MutableInputDefs()[0];
  NodeArg* cast_output = trail_cast->MutableOutputDefs()[0];
  // Update producer node
  Node* producer = graph.GetMutableProducerNode(cast_input->Name());
  auto consumers = graph.GetMutableConsumerNodes(cast_output->Name());
  int output_index = (producer != nullptr) ? optimizer_utils::IndexOfNodeOutput(*producer, *cast_input) : -1;
  if (producer) {
    int input_index = optimizer_utils::IndexOfNodeInput(*lead_cast, *cast_input);
    graph.RemoveEdge(producer->Index(), lead_cast->Index(), output_index, input_index);
    if (consumers.empty()) {
      auto& outputs = producer->MutableOutputDefs();
      std::replace(outputs.begin(), outputs.end(), cast_input, cast_output);
      graph.UpdateProducerNode(cast_output->Name(), producer->Index());
    }
  }
  // Update consumer nodes
  if (consumers.size()>0) {
    int cast_output_index = optimizer_utils::IndexOfNodeOutput(*trail_cast, *cast_output);
    for (Node* consumer : consumers) {
      if (consumer != nullptr &&
          std::find(removed_nodes.begin(), removed_nodes.end(), consumer->Index()) == removed_nodes.end()) {
        auto& consumer_inputs = consumer->MutableInputDefs();
        int input_index = optimizer_utils::IndexOfNodeInput(*consumer, *cast_output);
        graph.RemoveEdge(trail_cast->Index(), consumer->Index(), cast_output_index, input_index);
        std::replace(consumer_inputs.begin(), consumer_inputs.end(), cast_output, cast_input);
        if (producer) {
          graph.AddEdge(producer->Index(), consumer->Index(), output_index, input_index);
        }
      }
    }
    graph.UpdateConsumerNodes(cast_input->Name(), consumers);
  }
  for (auto cast : casts) {
    graph_utils::RemoveNodeOutputEdges(graph, *cast);
    removed_nodes.push_back(cast->Index());
  }
  return Status::OK();
}

// RemoveBackToBackCasts
// Remove FLOAT and FLOAT16 casts back-to-back, either FLOAT->FLOAT16 or FLOAT16->FLOAT
// Condition: The parent cast should have only one output
static bool RemoveBackToBackCasts(Graph& graph,
                                    std::deque<onnxruntime::NodeIndex>& removed_nodes, const logging::Logger& logger)
{
  ORT_UNUSED_PARAMETER(logger);
  bool modified = false;
  for (Node& node : graph.Nodes()) {
    if (std::find(removed_nodes.begin(), removed_nodes.end(), node.Index()) == removed_nodes.end() &&
        node.OpType() == "Cast") {
      const NodeAttributes& attributes = node.GetAttributes();
      ORT_ENFORCE(attributes.find("to") != attributes.end());
      bool is_fp = attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT);
      bool is_fp16 = attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT16);
      if (node.MutableOutputDefs().size() == 1) {
        NodeArg* cast_output = node.MutableOutputDefs()[0];
        for (Node* child : graph.GetMutableConsumerNodes(cast_output->Name())) {
          if (std::find(removed_nodes.begin(), removed_nodes.end(), child->Index()) == removed_nodes.end() &&
              child->OpType() == "Cast") {
            const NodeAttributes& child_attributes = child->GetAttributes();
            ORT_ENFORCE(child_attributes.find("to") != child_attributes.end());
            bool is_child_fp = child_attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT);
            bool is_child_fp16 = child_attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT16);
            if ((is_fp && is_child_fp16) || (is_fp16 && is_child_fp)) {
              // The parent and child cancell out
              std::cout << "RemoveBackToBackCasts: Removed Cast nodes  " << node.Name() << " and " << child->Name() << std::endl;
              RemoveCastNodes(graph, {&node, child}, removed_nodes);
              modified = true;
            } else if ((is_fp16 && is_child_fp16) || (is_fp && is_child_fp)) {
              // Child is a duplicate of parent
              std::cout << "RemoveBackToBackCasts: Removed Cast node  " << child->Name() << std::endl;
              RemoveCastNodes(graph, {child}, removed_nodes);
              modified = true;
            }
          }
        }
      }
    }
  }
  return modified;
}

// SearchUpstream:
// Recursively DFS traverse bottom-up, the graph upstream collecting all the NodeArgs that require a cast
// inorder to move an FP16 Cast operation up the graph.
// Visited float NodeArgs are either in require_cast or require_type_change so that the same
// nodearg is traversed more than once.
static void SearchUpstream(Graph& graph, NodeArg* node_arg,
                           std::unordered_set<NodeArg*>& require_cast,
                           std::unordered_set<NodeArg*>& require_type_change,
                                    std::deque<onnxruntime::NodeIndex>& removed_nodes)
{
  Node* node = graph.GetMutableProducerNode(node_arg->Name());
  if (node == nullptr) {
    // The graph inputs don't have the producer nodes
    if (node_arg->TypeAsProto()->tensor_type().elem_type() == TensorProto_DataType_FLOAT) {
      require_cast.insert(node_arg);
    }
  } else if(std::find(removed_nodes.begin(), removed_nodes.end(), node->Index()) == removed_nodes.end()) {
    std::string op_type = node->OpType();
    if (op_type == "Cast" && node_arg->TypeAsProto()->tensor_type().elem_type() == TensorProto_DataType_FLOAT) {
      // This Cast node and the Cast node that will be created later will cancel out
      require_cast.insert(node_arg);
    } else if (std::find(fp16_allow.begin(), fp16_allow.end(), op_type) == fp16_allow.end() &&
        std::find(fp16_safe.begin(), fp16_safe.end(), op_type) == fp16_safe.end()) {
      // Cannot traverse-up beyond this point
      if (node_arg->Exists() && node_arg->TypeAsProto()->tensor_type().elem_type() == TensorProto_DataType_FLOAT) {
        require_cast.insert(node_arg);
      }
    } else {
      // If the node has other float32 output(s) then stop the search.
      for (const auto* output_def : node->OutputDefs()) {
        // TODO: If the specified optimization is greater than 1 then insert a Cast to the 
        // other output_def and still propagate FP16 cast up the graph.
        if (output_def != node_arg) {
          if (output_def->TypeAsProto()->tensor_type().elem_type() == TensorProto_DataType_FLOAT) {
            require_cast.insert(node_arg);
            return;
          }
        }
      }
      for (NodeArg* node_input : node->MutableInputDefs()) {
        if (node_input->TypeAsProto()->tensor_type().elem_type() == TensorProto_DataType_FLOAT &&
            require_cast.find(node_input) != require_cast.end() &&
            require_type_change.find(node_input) != require_type_change.end()) {
         SearchUpstream(graph, node_input, require_cast, require_type_change, removed_nodes);
         if (require_cast.find(node_input) != require_cast.end()) {
           require_type_change.insert(node_input);
         }
        }
      }
    }
  }
}

// SearchDownstream:
// Recursively DFS traverse the graph downstream collecting all the NodeArgs that require a cast
// inorder to remove an FP32 Cast operation up the graph. Also collect the NodeArgs that need to 
// be converted from float to float16 along the way.
// The recursion only traverses an
static void SearchDownstream(Graph& graph, NodeArg* node_arg,
                             std::unordered_set<NodeArg*>& require_cast,
                             std::unordered_set<NodeArg*>& require_type_change,
                                    std::deque<onnxruntime::NodeIndex>& removed_nodes)
{
  for (Node* node : graph.GetMutableConsumerNodes(node_arg->Name())) {
    if (node) {
      std::string op_type = node->OpType();
      if (op_type == "Cast" && node_arg->TypeAsProto()->tensor_type().elem_type() == TensorProto_DataType_FLOAT) {
        // This Cast node and the Cast node that will be created later will cancel out
        require_cast.insert(node_arg);
      } else if (std::find(fp16_allow.begin(), fp16_allow.end(), op_type) == fp16_allow.end()) {
        if (node_arg->Exists() &&
            node_arg->TypeAsProto()->tensor_type().elem_type() == TensorProto_DataType_FLOAT) {
          require_cast.insert(node_arg);
        }
      } else {
        // If the node has other float32 inputs then stop the search
        for (const auto* input_def : node->InputDefs()) {
          // TODO: If the secified level of the optimization is greater than 1 then
          // convert initializers if any from float to float16.
          if (input_def != node_arg) {
            if (input_def->TypeAsProto()->tensor_type().elem_type() == TensorProto_DataType_FLOAT) {
              require_cast.insert(node_arg);
              return;
            }
          }
        }       
        for (NodeArg* node_output : node->MutableOutputDefs()) {
          if (node_output->TypeAsProto()->tensor_type().elem_type() == TensorProto_DataType_FLOAT &&
              require_cast.find(node_output) != require_cast.end() &&
              require_type_change.find(node_output) != require_type_change.end()) {
            SearchDownstream(graph, node_output, require_cast, require_type_change, removed_nodes);
            if (require_cast.find(node_output) != require_cast.end()) {
              require_type_change.insert(node_output);
            }
          }
        }
      }
    }
  }
}

// GatherNames
// Collects all the names from the pointers of the objects stores in the container class C
// the class should have a member functions returning a string (or a ref).
template<typename C, typename T = typename C::value_type>
static std::string GatherNames(C const& items)
{
  std::vector<std::string> names;
  std::transform(items.begin(), items.end(), back_inserter(names), [](T n) { return n->Name(); });
  return std::accumulate(names.begin(), names.end(), std::string(), [](const std::string& a, const std::string& b) { return a + ", " + b;});
}

// Change the elem_type of the given NodeArgs from FLOAT to FLOAT16.
static void ChangeTypeToFP16(std::unordered_set<NodeArg*>& require_type_change, const logging::Logger& logger)
{
  ONNX_NAMESPACE::TypeProto type_proto;
  type_proto.mutable_tensor_type()->set_elem_type(TensorProto::FLOAT16);
  for (NodeArg* input : require_type_change) {
    if (input->Exists() && input->TypeAsProto()->tensor_type().elem_type() == TensorProto::FLOAT) {
      input->UpdateTypeAndShape(type_proto, true, true, logger);
    }
  }
}

// PropagateForwards
// Propagate FP32 Cast operations forwards (downstream)
// Recurrsively search the graph for Cast FP16 safe/allowed operations to expand
// the float16 computation region.
// The required_cast vector is the collection of nodes that require float cast.
// All nodeargs on a path down to any of the 
// frontier nodes require type change from FLOAT to FLOAT16. 
// require_type_change consists of such nodes.  All the frontier nodes require fp32 cast
static bool PropagateForwards(Graph& graph, Node* node,
                                    std::deque<onnxruntime::NodeIndex>& removed_nodes, const logging::Logger& logger)
{

  bool modified = false;
  if (node == nullptr) {
    return false;
  }
  if (node->OpType() == "Cast") {
    const NodeAttributes& attributes = node->GetAttributes();
    ORT_ENFORCE(attributes.find("to") != attributes.end());
    if (attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT)) {
      std::unordered_set<NodeArg*> require_cast;
      std::unordered_set<NodeArg*> require_type_change;
      NodeArg* cast_output = node->MutableOutputDefs()[0];
      SearchDownstream(graph, cast_output, require_cast, require_type_change, removed_nodes);
      if (require_cast.size() > 0 && require_cast.find(cast_output) == require_cast.end()) {
        // Remove Cast operation
        std::cout << "PropagateForwards: Removed Cast node  " << node->Name() << std::endl;
        RemoveCastNodes(graph, {node}, removed_nodes);
        InsertCastNodes(graph, require_cast, false, removed_nodes);
        std::cout << "PropagateForwwards: Inserted Cast nodes " << GatherNames<std::unordered_set<NodeArg*>>(require_cast) << std::endl;
        ChangeTypeToFP16(require_type_change, logger);
        modified = true;
      }
    }
  }
  return modified;
}

// PropagateBackwards
// Propagate FP16 Cast operations backwards (upstream)
// Recurrsively search the graph for Cast FP16 safe/allowed operations and expand
// float16 computation regsion and
// find the frontiers of the float16 computation region.
// The required_cast vector is the collection of 
// FP16-cast-frontiers of the cast node. All nodeargs on the path from any of the 
// frontier nodes to the cast node require type change from  FLOAT to FLOAT16.
// Each of the frontier nodes requires an fp16 cast.
static bool PropagateBackwards(Graph& graph, Node* node,
                                    std::deque<onnxruntime::NodeIndex>& removed_nodes, const logging::Logger& logger)
{
  bool modified = false;
  if (node == nullptr) {
    return false;
  }
  if (node->OpType() == "Cast") {
    const NodeAttributes& attributes = node->GetAttributes();
    ORT_ENFORCE(attributes.find("to") != attributes.end());
    if (attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT16)) {
      std::unordered_set<NodeArg*> require_cast;
      NodeArg* cast_input = node->MutableInputDefs()[0];
      std::unordered_set<NodeArg*> require_type_change;
      SearchUpstream(graph, cast_input, require_cast, require_type_change, removed_nodes);
      if (require_cast.size() > 0 && require_cast.find(cast_input) == require_cast.end()) {
        // Remove Cast operation
        std::cout << "PropagateBackwards: Removed Cast node  " << node->Name() << std::endl << std::endl;
        RemoveCastNodes(graph, {node}, removed_nodes);
        InsertCastNodes(graph, require_cast, true, removed_nodes);
        ChangeTypeToFP16(require_type_change, logger);
        std::cout << "PropagateBackwards: Inserted Cast nodes "
                  << GatherNames<std::unordered_set<NodeArg*>>(require_cast) << std::endl;
        std::cout << "PropagateBackwards: Changed the type from float to float16 : "
                  << GatherNames<std::unordered_set<NodeArg*>>(require_type_change) << std::endl;
        modified = true;
      }
    }
  }
  return modified;
}

// Fuse all nodes, replace with a single node.
// Assumptions:
// 1. all nodes are Cast ops and are of the same Cast type
// 2. all the nodes have the same input
static void FuseNodes(Graph& graph, NodeArg* input, std::vector<Node*> nodes,
                                    std::deque<onnxruntime::NodeIndex>& removed_nodes)
{
  std::vector<NodeArg*> outputs;
  for (Node* node : nodes) {
    std::vector<NodeArg*> node_outputs = node->MutableOutputDefs();
    outputs.insert(outputs.end(), node_outputs.begin(), node_outputs.end());
  }
  Node* node = nodes[0];
  (void) graph.AddNode(graph.GenerateNodeName(node->Name() + "_replace"),
                       node->OpType(),
                       "Created to replace a node",
                       {input},
                       outputs,
                       &node->GetAttributes(),
                       node->Domain());
  for (Node* n : nodes) {
    removed_nodes.push_back(n->Index());
    graph_utils::RemoveNodeOutputEdges(graph, *n);
//    graph.RemoveNode(n->Index());    
  }
}

// Traverse the graph recursively searching/collecting sibling Cast op nodes to fuse and call FuseNodes.
static bool FuseSubgraphs(Graph& graph, Node* parent,
                          std::deque<onnxruntime::NodeIndex>& removed_nodes,
                          const logging::Logger& logger)
{
  ORT_UNUSED_PARAMETER(logger);
  bool modified = false;
  for (NodeArg* output : parent->MutableOutputDefs()) {
    std::vector<Node*> cast_fp16_siblings;
    std::vector<Node*> cast_fp_siblings;
    for (Node* node : graph.GetMutableConsumerNodes(output->Name())) {
      if (node == nullptr ||
          std::find(removed_nodes.begin(), removed_nodes.end(), node->Index()) != removed_nodes.end()) {
        continue;
      }
      if (node->OpType() == "Cast") {
        const NodeAttributes& attributes = node->GetAttributes();
        ORT_ENFORCE(attributes.find("to") != attributes.end());
        if (attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT16)) {
          cast_fp16_siblings.push_back(node);
        } else if (attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT)) {
          cast_fp_siblings.push_back(node);
        }
      }
    }
    if (cast_fp16_siblings.size() > 1) {
      modified = true;
      FuseNodes(graph, output, cast_fp16_siblings, removed_nodes);
      std::cout << "FusedSubgraphs: Fused Cast nodes : " << GatherNames<std::vector<Node*>>(cast_fp16_siblings) << std::endl;
    }
    if (cast_fp_siblings.size() > 1) {
      modified = true;
      FuseNodes(graph, output, cast_fp_siblings, removed_nodes);
      std::cout << "FusedSubgraphs: Fused Cast nodes : " << GatherNames<std::vector<Node*>>(cast_fp_siblings) << std::endl;
    }
  }
  return modified;
}

// RemoveUnnecessaryCasts
// Remove a cast if the input elem_type is same the required cast type.
static bool RemoveUnnecessaryCasts(Graph& graph,
                                  std::deque<onnxruntime::NodeIndex>& removed_nodes,
                                  const logging::Logger& logger)
{
  ORT_UNUSED_PARAMETER(logger);
  bool modified = false;
  for (auto& node: graph.Nodes()) {
    if (node.OpType() == "Cast") {
      const NodeAttributes& attributes = node.GetAttributes();
      ORT_ENFORCE(attributes.find("to") != attributes.end());
      TensorProto_DataType data_type = static_cast<TensorProto_DataType> (attributes.at("to").i());
      NodeArg* cast_input = node.MutableInputDefs()[0];
      if (cast_input->TypeAsProto()->tensor_type().elem_type() == data_type) {
        std::cout << "Removed unnecessary cast " << node.Name() << std::endl;
        RemoveCastNodes(graph, {&node}, removed_nodes);
        modified = true;
      }
    }
  }
  return modified;
}

// PropagateFP32CastsFromInputsToOutputs
// This non recursive fusion, checks whether the given node is fp16 safe op and 
// whether all non-floatingpoint inputs are cast to fp32
// and propagates cast op to the non-floatingpoint outputs.
static bool PropagateFP32CastsFromInputsToOutputs(Graph& graph, Node* node,
                                                  std::deque<onnxruntime::NodeIndex>& removed_nodes,
                                                  const logging::Logger& logger)
{
  bool modified = false;
  if (std::find(fp16_safe.begin(), fp16_safe.end(), node->OpType()) != fp16_safe.end()) {
    bool has_float_inputs = false;
    bool all_float_inputs_have_casts = true;
    std::vector<Node*> casts;
    std::unordered_set<NodeArg*> require_type_change;
    for (NodeArg* input : node->MutableInputDefs()) {
      if (!input->Exists() || (input->TypeAsProto()->tensor_type().elem_type() != TensorProto::FLOAT)) {
        continue;
      }
      has_float_inputs = true;
      Node* producer = graph.GetMutableProducerNode(input->Name());
      if (producer && producer->OpType() == "Cast") {
        const NodeAttributes& attributes = producer->GetAttributes();
        ORT_ENFORCE(attributes.find("to") != attributes.end());
        if (attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT)) {
          casts.push_back(producer);
          require_type_change.insert(input);
          continue;
        }
      }
      all_float_inputs_have_casts = false;
      break;
    }
    if (has_float_inputs && all_float_inputs_have_casts && casts.size() > 0) {
      std::cout << "PropagateCastsFromInputsToOutputs: Removed Cast nodes "
                       << GatherNames<std::vector<Node*>>(casts)
                       << " feeding the same compute node " << node->Name() << std::endl;
      RemoveCastNodes(graph, casts, removed_nodes);
      std::unordered_set<NodeArg*> node_args;
      for (NodeArg* output : node->MutableOutputDefs()) {
        if (output->Exists() && output->TypeAsProto()->tensor_type().elem_type() == TensorProto::FLOAT) {
          node_args.insert(output);
        }
      }
      InsertCastNodes(graph, node_args, false, removed_nodes);
      ChangeTypeToFP16(require_type_change, logger);
      std::cout << "PropagateCastsFromInputsToOutputs: Inserted Cast node to " 
                       << GatherNames(node_args) << std::endl;
      modified = true;
    }
  }
  return modified;
}

// PropagateFP16CastsFromOutputsToInputs
// This non recursive fusion, checks whether the given node is fp16 safe op and 
// whether all non-floatingpoint outputs are cast to fp16
// and propagates cast op to the non-floatingpoint inputs.
static bool PropagateFP16CastsFromOutputsToInputs(Graph& graph, Node* node,
                                    std::deque<onnxruntime::NodeIndex>& removed_nodes, const logging::Logger& logger)
{
  bool modified = false;
  if (std::find(fp16_safe.begin(), fp16_safe.end(), node->OpType()) != fp16_safe.end()) {
    bool has_float_outputs = false;
    bool all_float_outputs_have_casts = true;
    std::vector<Node*> casts; // Cast nodes to propagate.
    std::vector<NodeArg*>& outputs = node->MutableOutputDefs();
    std::unordered_set<NodeArg*> require_type_change;
    for (auto iter = outputs.begin(); iter != outputs.end() && all_float_outputs_have_casts; ++iter) {
      NodeArg* output = *iter;
      if (!output->Exists() || (output->TypeAsProto()->tensor_type().elem_type() != TensorProto::FLOAT)) {
        continue;
      }
      has_float_outputs = true;
      std::vector<Node*> consumers = graph.GetMutableConsumerNodes(output->Name());
      for (auto node_iter = consumers.begin(); node_iter != consumers.end() && all_float_outputs_have_casts; ++node_iter) {
        Node* consumer = *node_iter;
        if (consumer != nullptr &&
            std::find(removed_nodes.begin(), removed_nodes.end(), consumer->Index()) == removed_nodes.end() &&
            consumer->OpType() == "Cast") {
          const NodeAttributes& attributes = consumer->GetAttributes();
          ORT_ENFORCE(attributes.find("to") != attributes.end());
          if (attributes.at("to").i() == static_cast<int64_t> (TensorProto::FLOAT16)) {
            casts.push_back(consumer);
            continue;
          }
        }
        all_float_outputs_have_casts = false;
      }
      require_type_change.insert(output);
    }
    if (has_float_outputs && all_float_outputs_have_casts && casts.size() > 0 ) {
      std::cout << "PropagateCastsFromOutputsToInputs: Removed Cast nodes "
                       << GatherNames<std::vector<Node*>>(casts)
                       << " feeding the same compute node " << node->Name() << std::endl;
      RemoveCastNodes(graph, casts, removed_nodes);
      std::unordered_set<NodeArg*> node_args;
      for (NodeArg* input : node->MutableInputDefs()) {
        if (input->Exists() && input->TypeAsProto()->tensor_type().elem_type() == TensorProto::FLOAT) {
          node_args.insert(input);
        }
      }
      InsertCastNodes(graph, node_args, true, removed_nodes);
      std::cout << "PropagateCastsFromOutputsToInputs: Inserted Cast node to " << GatherNames(node_args) << std::endl;
      ChangeTypeToFP16(require_type_change, logger);
      std::cout << "PropagateCastsFromOutputsToInputs: ChangeTypeToFP16" << std::endl;
      modified = true;
    }
  }
  return modified;
}

// Expand FP16 compute regions on the graph by example float16 compute nodes,
// propagating float32 Cast operation down the graph and propagating float16
// Cast operations up the graph. The following functions are performed
// 1. Fuse subgraphs
// 2. Propagate fp32 casts forwards
// 3. Propagate fp16 casts back
Status PropagateCastOps::ApplyImpl(Graph& graph, bool& modified, int graph_level, const logging::Logger& logger) const {
  // First apply the transformation to the subgraphs.
  GraphViewer graph_viewer(graph);
  const auto& node_topology_list = graph_viewer.GetNodesInTopologicalOrder();

  for (auto node_index : node_topology_list) {
    auto* node_ptr = graph.GetNode(node_index);
    if (nullptr == node_ptr)
      continue;  // node was removed

    auto& node = *node_ptr;

    ORT_RETURN_IF_ERROR(Recurse(node, modified, graph_level, logger));
  }
  int pass=0;
  bool local_modified = false;
  do {
    std::deque<onnxruntime::NodeIndex> removed_nodes;
    std::cout << "pass = " << pass << std::endl;
    if (local_modified) {
      graph.Resolve();
    }
    std::cout << "resolved" << std::endl;
    local_modified = RemoveUnnecessaryCasts(graph, removed_nodes, logger);
    std::cout << "Done RemovedUnnecessaryCasts" << std::endl;
    // Fuse subgraphs, sibling Cast nodes with same input
    for (auto& node: graph.Nodes()) {
      if (std::find(removed_nodes.begin(), removed_nodes.end(), node.Index()) == removed_nodes.end()) {
        local_modified |= FuseSubgraphs(graph, &node, removed_nodes, logger);
      }
    }
    std::cout << "Done FuseSubGraphs" << std::endl;

    // Propagate FP32 Casts forward
    for (Node& node : graph.Nodes()) {
      if (std::find(removed_nodes.begin(), removed_nodes.end(), node.Index()) == removed_nodes.end()) {
        local_modified |= PropagateForwards(graph, &node, removed_nodes, logger);
      }
    }
    std::cout << "Done PropagateForwards" << std::endl;

    local_modified |= RemoveBackToBackCasts(graph, removed_nodes, logger);
    std::cout << "Done RemoveBackToBackCasts" << std::endl;

    // Propagate FP16 Casts backward
    for (Node& node : graph.Nodes()) {
      if (std::find(removed_nodes.begin(), removed_nodes.end(), node.Index()) == removed_nodes.end()) {
        local_modified |= PropagateBackwards(graph, &node, removed_nodes, logger);
      }
    }
    std::cout << "Done PropagateBackwards" << std::endl;

    // Propagate FP16 Casts from outputs to inputs
    for (Node& node : graph.Nodes()) {
      if (std::find(removed_nodes.begin(), removed_nodes.end(), node.Index()) == removed_nodes.end()) {
        local_modified |= PropagateFP16CastsFromOutputsToInputs(graph, &node, removed_nodes, logger);
      }
    }
    std::cout << "Done PropagateFP16CastsFromOutputsToInputs" << std::endl;

    // Propagate FP32 Casts from inputs to outputs
    for (Node& node : graph.Nodes()) {
      if (std::find(removed_nodes.begin(), removed_nodes.end(), node.Index()) == removed_nodes.end()) {
        local_modified |= PropagateFP32CastsFromInputsToOutputs(graph, &node, removed_nodes, logger);
      }
    }
    std::cout << "Done PropagateFP32CastsFromInputsToOutputs" << std::endl;
    for (onnxruntime::NodeIndex removed_node : removed_nodes) {
      graph.RemoveNode(removed_node);
    }
    pass++;
    modified |= local_modified;
  } while (local_modified);

  return Status::OK();
}

} // namespace onnxruntime