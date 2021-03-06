/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "minddata/dataset/engine/ir/datasetops/dataset_node.h"

#include <algorithm>
#include <memory>
#include <set>

#include "minddata/dataset/engine/opt/pass.h"
#include "minddata/dataset/util/random.h"

namespace mindspore {
namespace dataset {

// Helper function to compute a default shuffle size
Status ComputeShuffleSize(int64_t num_files, int64_t num_devices, int64_t num_rows, int64_t total_rows,
                          int64_t *shuffle_size) {
  const int64_t average_files_multiplier = 4;
  const int64_t shuffle_max = 10000;
  int64_t avg_rows_per_file = 0;

  // Adjust the num rows per shard if sharding was given
  if (num_devices > 0) {
    if (num_rows % num_devices == 0) {
      num_rows = num_rows / num_devices;
    } else {
      num_rows = (num_rows / num_devices) + 1;
    }
  }

  // Cap based on total rows directive.  Some ops do not have this and give value of 0.
  if (total_rows > 0) {
    num_rows = std::min(num_rows, total_rows);
  }

  // get the average per file
  CHECK_FAIL_RETURN_UNEXPECTED(num_files != 0, "The size of dataset_files must greater than 0.");
  avg_rows_per_file = num_rows / num_files;

  *shuffle_size = std::max(avg_rows_per_file * average_files_multiplier, shuffle_max);
  return Status::OK();
}

// Helper function to inject a shuffle operator over top of current operator being built
Status AddShuffleOp(int64_t num_files, int64_t num_devices, int64_t num_rows, int64_t total_rows,
                    int32_t connector_que_size, int32_t rows_per_buffer, std::shared_ptr<DatasetOp> *shuffle_op) {
  std::shared_ptr<ShuffleOp> new_shuffle_op = nullptr;
  int64_t shuffle_size = 0;
  RETURN_EMPTY_IF_ERROR(ComputeShuffleSize(num_files, num_devices, num_rows, total_rows, &shuffle_size));
  MS_LOG(INFO) << "Dataset::AddShuffleOp - num_rows: " << num_rows << ", shuffle_size: " << shuffle_size;
  // Add the shuffle op
  *shuffle_op = std::make_shared<ShuffleOp>(shuffle_size, GetSeed(), connector_que_size, true, rows_per_buffer);
  return Status::OK();
}

// Helper function to validate dataset directory parameter
Status ValidateDatasetDirParam(const std::string &dataset_name, std::string dataset_dir) {
  if (dataset_dir.empty()) {
    std::string err_msg = dataset_name + ": dataset_dir is not specified.";
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }

  Path dir(dataset_dir);
  if (!dir.IsDirectory()) {
    std::string err_msg = dataset_name + ": dataset_dir: [" + dataset_dir + "] is an invalid directory path.";
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }

  if (access(dataset_dir.c_str(), R_OK) == -1) {
    std::string err_msg = dataset_name + ": No access to specified dataset path: " + dataset_dir;
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }

  return Status::OK();
}

// Helper function to validate dataset files parameter
Status ValidateDatasetFilesParam(const std::string &dataset_name, const std::vector<std::string> &dataset_files) {
  if (dataset_files.empty()) {
    std::string err_msg = dataset_name + ": dataset_files is not specified.";
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }

  for (auto f : dataset_files) {
    Path dataset_file(f);
    if (!dataset_file.Exists()) {
      std::string err_msg = dataset_name + ": dataset file: [" + f + "] is invalid or does not exist.";
      MS_LOG(ERROR) << err_msg;
      RETURN_STATUS_SYNTAX_ERROR(err_msg);
    }
    if (access(dataset_file.toString().c_str(), R_OK) == -1) {
      std::string err_msg = dataset_name + ": No access to specified dataset file: " + f;
      MS_LOG(ERROR) << err_msg;
      RETURN_STATUS_SYNTAX_ERROR(err_msg);
    }
  }

  return Status::OK();
}

// Helper function to validate dataset num_shards and shard_id parameters
Status ValidateDatasetShardParams(const std::string &dataset_name, int32_t num_shards, int32_t shard_id) {
  if (num_shards <= 0) {
    std::string err_msg = dataset_name + ": Invalid num_shards: " + std::to_string(num_shards);
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }

  if (shard_id < 0 || shard_id >= num_shards) {
    // num_shards;
    std::string err_msg = dataset_name + ": Invalid input, shard_id: " + std::to_string(shard_id) +
                          ", num_shards: " + std::to_string(num_shards);
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }

  return Status::OK();
}

// Helper function to validate dataset sampler parameter
Status ValidateDatasetSampler(const std::string &dataset_name, const std::shared_ptr<SamplerObj> &sampler) {
  if (sampler == nullptr) {
    std::string err_msg = dataset_name + ": Sampler is not constructed correctly, sampler: nullptr";
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  return Status::OK();
}

Status ValidateStringValue(const std::string &dataset_name, const std::string &str,
                           const std::unordered_set<std::string> &valid_strings) {
  if (valid_strings.find(str) == valid_strings.end()) {
    std::string mode;
    mode = std::accumulate(valid_strings.begin(), valid_strings.end(), mode,
                           [](std::string a, std::string b) { return std::move(a) + " " + std::move(b); });
    std::string err_msg = dataset_name + ": " + str + " does not match any mode in [" + mode + " ]";
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  return Status::OK();
}

// Helper function to validate dataset input/output column parameter
Status ValidateDatasetColumnParam(const std::string &dataset_name, const std::string &column_param,
                                  const std::vector<std::string> &columns) {
  if (columns.empty()) {
    std::string err_msg = dataset_name + ":" + column_param + " should not be empty string";
    MS_LOG(ERROR) << err_msg;
    RETURN_STATUS_SYNTAX_ERROR(err_msg);
  }
  for (uint32_t i = 0; i < columns.size(); ++i) {
    if (columns[i].empty()) {
      std::string err_msg = dataset_name + ":" + column_param + "[" + std::to_string(i) + "] must not be empty";
      MS_LOG(ERROR) << err_msg;
      RETURN_STATUS_SYNTAX_ERROR(err_msg);
    }
  }
  std::set<std::string> columns_set;
  for (auto &column_name : columns) {
    auto result = columns_set.insert(column_name);
    if (result.second == false) {
      std::string err_msg = dataset_name + ":" + column_param +
                            ": Invalid parameter, duplicate column names are not allowed: " + *result.first;
      MS_LOG(ERROR) << err_msg;
      RETURN_STATUS_SYNTAX_ERROR(err_msg);
    }
  }
  return Status::OK();
}

std::shared_ptr<SamplerObj> SelectSampler(int64_t num_samples, bool shuffle, int32_t num_shards, int32_t shard_id) {
  if (shuffle) {
    if (num_shards > 1) {
      // If shuffle enabled, sharding enabled, use distributed random sampler
      return DistributedSampler(num_shards, shard_id, shuffle, num_samples);
    }
    // If shuffle enabled, sharding disabled, use random sampler
    return RandomSampler(num_samples >= 0, num_samples);
  }
  if (num_shards > 1) {
    // If shuffle disabled, sharding enabled, use distributed sequential sampler
    return DistributedSampler(num_shards, shard_id, shuffle, num_samples);
  }
  // If shuffle disabled, sharding disabled, use sequential sampler
  return SequentialSampler(0, num_samples);
}

Status DatasetNode::AddCacheOp(std::vector<std::shared_ptr<DatasetOp>> *node_ops) {
  if (cache_ != nullptr) {
    RETURN_IF_NOT_OK(cache_->Build());
    std::shared_ptr<DatasetOp> cache_op;
    RETURN_IF_NOT_OK(cache_->CreateCacheOp(num_workers_, &cache_op));
    node_ops->push_back(cache_op);
  }
  return Status::OK();
}
// Constructor to initialize the cache
DatasetNode::DatasetNode(const std::shared_ptr<DatasetCache> &dataset_cache) : DatasetNode() { cache_ = dataset_cache; }

std::shared_ptr<DatasetNode> DatasetNode::SetNumWorkers(int32_t num_workers) {
#if !defined(_WIN32) && !defined(_WIN64)
#ifndef ENABLE_ANDROID
  int32_t cpu_count = sysconf(_SC_NPROCESSORS_CONF);
  if (cpu_count < 0 || cpu_count > INT32_MAX) {
    MS_LOG(ERROR) << "Error determining current CPU: " << cpu_count;
    return nullptr;
  }
  if (num_workers < 1 || num_workers > cpu_count) {
    MS_LOG(ERROR) << "num_workers exceeds the boundary between 1 and " << cpu_count;
    return nullptr;
  }
#endif
#endif
  num_workers_ = num_workers;
  return shared_from_this();
}

DatasetNode::DatasetNode() : cache_(nullptr), parent_(nullptr), children_({}) {
  // Fetch some default value from config manager
  std::shared_ptr<ConfigManager> cfg = GlobalContext::config_manager();
  num_workers_ = cfg->num_parallel_workers();
  rows_per_buffer_ = cfg->rows_per_buffer();
  connector_que_size_ = cfg->op_connector_size();
  worker_connector_size_ = cfg->worker_connector_size();
}

// this function will preform a deep copy of current node (and its descendants), the parent* pointer will not be copied
std::shared_ptr<DatasetNode> DatasetNode::DeepCopy() {
  std::shared_ptr<DatasetNode> new_node = this->Copy();
  for (const auto &child : children_) {
    new_node->AddChild(child->DeepCopy());
  }
  return new_node;
}

std::string DatasetNode::PrintColumns(const std::vector<std::string> &columns) const {
  std::string me;
  if (columns.empty()) {
    me = "<nil>";
  } else {
    me = "[";
    auto i = 0;
    for (auto it = columns.begin(); it < columns.end(); ++it, ++i) {
      me += *it;
      if (i < columns.size() - 1) {
        me += ", ";
      } else {
        me += "]";
      }
    }
  }
  return me;
}

void DatasetNode::PrintTree(std::ostream &out) const {
  int level = 0;
  PrintNode(out, &level);
}

void DatasetNode::PrintNode(std::ostream &out, int *level) const {
  const std::string prefix = "+-";
  const std::string indent = "  ";
  out << prefix;
  Print(out);
  for (const auto &c : this->Children()) {
    out << '\n';
    ++(*level);
    for (auto i = 0; i < *level; i++) {
      out << indent;
    }
    c->PrintNode(out, level);
    --(*level);
  }
}

// Add a node as a child, node's parent needs to be nullptr
// this function will allow child to be a nullptr, in which case it will simply skip
void DatasetNode::AddChild(std::shared_ptr<DatasetNode> child) {
  if (child != nullptr && child->parent_ == nullptr) {
    children_.push_back(child);
    child->parent_ = this;
  } else if (child != nullptr) {
    MS_LOG(WARNING) << "DatasetNode::AddChild() Fail" + child->Name() + "'s parent isn't a nullptr.";
  }
}

// Remove this node from its parent. Add the child of this node to its parent.
// for now, this remove is limited to node with a single child or no child
Status DatasetNode::Remove() {
  CHECK_FAIL_RETURN_UNEXPECTED(parent_ != nullptr, "Cannot remove root or a node without parent.");
  CHECK_FAIL_RETURN_UNEXPECTED(children_.size() < 2, "Cannot remove node with more than 1 child.");
  if (children_.empty()) {  // I am a leaf node, remove me from my parent's children list
    parent_->children_.erase(std::remove(parent_->children_.begin(), parent_->children_.end(), shared_from_this()),
                             parent_->children_.end());  // removal using "erase remove idiom"
  } else {  // replace my position in my parent's children list with my single child
    auto itr = std::find(parent_->children_.begin(), parent_->children_.end(), shared_from_this());
    CHECK_FAIL_RETURN_UNEXPECTED(itr != parent_->children_.end(), "I am not in my parent's children list.");
    children_[0]->parent_ = parent_;  // set my single child's parent ptr to my parent
    *itr = std::move(children_[0]);   // replace me in my parent's children list with my single child
    children_.clear();                //  release my single child from my children list
  }
  parent_ = nullptr;
  return Status::OK();
}

// In DFS tree traversal, each node is visited twice. Accept is called on the first visit.
Status DatasetNode::Accept(NodePass *p, bool *modified) {
  // This method will only be called if its derived class does not implement one.
  return p->Visit(shared_from_this(), modified);
}

// In DFS tree traversal, each node is visited twice. AcceptAfter is called on the second visit
// after all child nodes are visited.
Status DatasetNode::AcceptAfter(NodePass *p, bool *modified) {
  // This method will only be called if its derived class does not implement one.
  return p->VisitAfter(shared_from_this(), modified);
}

Status DatasetNode::GetShardId(int32_t *shard_id) {
  if (!Children().empty()) {
    // Get shard id from the child node
    return Children()[0]->GetShardId(shard_id);
  } else {
    RETURN_STATUS_SYNTAX_ERROR("Get Shard Id failed at source node: " + Name() + "\n");
  }
}
// Visitor accepting method for NodePass
Status SourceNode::Accept(NodePass *p, bool *modified) {
  // Downcast shared pointer then call visitor
  return p->Visit(shared_from_base<SourceNode>(), modified);
}

// Visitor accepting method for NodePass
Status SourceNode::AcceptAfter(NodePass *p, bool *modified) {
  // Downcast shared pointer then call visitor
  return p->VisitAfter(shared_from_base<SourceNode>(), modified);
}
}  // namespace dataset
}  // namespace mindspore
