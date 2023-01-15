// Program  - Taichi program execution context

#pragma once

#include <functional>
#include <optional>
#include <atomic>
#include <stack>
#include <shared_mutex>

#define TI_RUNTIME_HOST
#include "taichi/aot/module_builder.h"
#include "taichi/ir/frontend_ir.h"
#include "taichi/ir/ir.h"
#include "taichi/ir/type_factory.h"
#include "taichi/ir/snode.h"
#include "taichi/util/lang_util.h"
#include "taichi/program/program_impl.h"
#include "taichi/program/callable.h"
#include "taichi/program/function.h"
#include "taichi/program/kernel.h"
#include "taichi/program/kernel_profiler.h"
#include "taichi/program/snode_expr_utils.h"
#include "taichi/program/snode_rw_accessors_bank.h"
#include "taichi/program/context.h"
#include "taichi/struct/snode_tree.h"
#include "taichi/system/memory_pool.h"
#include "taichi/system/threading.h"
#include "taichi/system/unified_allocator.h"
#include "taichi/program/sparse_matrix.h"
#include "taichi/ir/mesh.h"

namespace taichi::lang {

struct JITEvaluatorId {
  std::thread::id thread_id;
  // Note that on certain backends (e.g. CUDA), functions created in one
  // thread cannot be used in another. Hence the thread_id member.
  int op;
  DataType ret, lhs, rhs;
  std::string tb;
  bool is_binary;

  UnaryOpType unary_op() const {
    TI_ASSERT(!is_binary);
    return (UnaryOpType)op;
  }

  BinaryOpType binary_op() const {
    TI_ASSERT(is_binary);
    return (BinaryOpType)op;
  }

  bool operator==(const JITEvaluatorId &o) const {
    return thread_id == o.thread_id && op == o.op && ret == o.ret &&
           lhs == o.lhs && rhs == o.rhs && is_binary == o.is_binary &&
           tb == o.tb;
  }
};

}  // namespace taichi::lang

namespace std {
template <>
struct hash<taichi::lang::JITEvaluatorId> {
  std::size_t operator()(
      taichi::lang::JITEvaluatorId const &id) const noexcept {
    return ((std::size_t)id.op | (id.ret.hash() << 8) | (id.lhs.hash() << 16) |
            (id.rhs.hash() << 24) | ((std::size_t)id.is_binary << 31)) ^
           (std::hash<std::thread::id>{}(id.thread_id) << 32);
  }
};
}  // namespace std

namespace taichi::lang {

class StructCompiler;

/**
 * Note [Backend-specific ProgramImpl]
 * We're working in progress to keep Program class minimal and move all backend
 * specific logic to their corresponding backend ProgramImpls.

 * If you are thinking about exposing/adding attributes/methods to Program
 class,
 * please first think about if it's general for all backends:
 * - If so, please consider adding it to ProgramImpl class first.
 * - Otherwise please add it to a backend-specific ProgramImpl, e.g.
 * LlvmProgramImpl, MetalProgramImpl..
 */

class TI_DLL_EXPORT Program {
 public:
  using Kernel = taichi::lang::Kernel;
  // We let every thread has its own config because the constant folding pass
  // wants to change the CompileConfig so that it can compile the evaluator,
  // but we don't want it to change the global config. We will refactor it
  // later when we make Taichi thread-safe.
  std::unordered_map<std::thread::id, CompileConfig> configs;
  std::thread::id main_thread_id_;

  uint64 *result_buffer{nullptr};  // Note result_buffer is used by all backends

  std::vector<std::unique_ptr<Kernel>> kernels;

  std::unique_ptr<KernelProfilerBase> profiler{nullptr};

  std::unordered_map<JITEvaluatorId, std::unique_ptr<Kernel>>
      jit_evaluator_cache;
  std::mutex jit_evaluator_cache_mut;
  std::atomic<uint32_t> jit_evaluator_id{0};

  // Note: for now we let all Programs share a single TypeFactory for smooth
  // migration. In the future each program should have its own copy.
  static TypeFactory &get_type_factory();

  Program() : Program(default_compile_config.arch) {
  }

  explicit Program(Arch arch);

  ~Program();

  CompileConfig &this_thread_config() {
    // std::unordered_map is not thread safe even if we do the rehash in
    // advance, so we need to add a lock to protect it.
    std::shared_lock<std::shared_mutex> read_lock(config_map_mut);
    auto thread_id = std::this_thread::get_id();
    if (!configs.count(thread_id)) {
      read_lock.unlock();
      std::unique_lock<std::shared_mutex> write_lock(config_map_mut);
      configs[thread_id] = configs[main_thread_id_];
      return configs[thread_id];
    }
    return configs[thread_id];
  }

  const CompileConfig &config() {
    return configs[main_thread_id_];
  }

  struct KernelProfilerQueryResult {
    int counter{0};
    double min{0.0};
    double max{0.0};
    double avg{0.0};
  };

  KernelProfilerQueryResult query_kernel_profile_info(const std::string &name) {
    KernelProfilerQueryResult query_result;
    profiler->query(name, query_result.counter, query_result.min,
                    query_result.max, query_result.avg);
    return query_result;
  }

  void clear_kernel_profile_info() {
    profiler->clear();
  }

  void profiler_start(const std::string &name) {
    profiler->start(name);
  }

  void profiler_stop() {
    profiler->stop();
  }

  KernelProfilerBase *get_profiler() {
    return profiler.get();
  }

  void synchronize();

  StreamSemaphore flush();

  /**
   * Materializes the runtime.
   */
  void materialize_runtime();

  int get_snode_tree_size();

  void visualize_layout(const std::string &fn);

  Kernel &kernel(const std::function<void(Kernel *)> &body,
                 const std::string &name = "",
                 AutodiffMode autodiff_mode = AutodiffMode::kNone) {
    // Expr::set_allow_store(true);
    auto func = std::make_unique<Kernel>(*this, body, name, autodiff_mode);
    // Expr::set_allow_store(false);
    kernels.emplace_back(std::move(func));
    return *kernels.back();
  }

  Function *create_function(const FunctionKey &func_key);

  // TODO: This function is doing two things: 1) compiling CHI IR, and 2)
  // offloading them to each backend. We should probably separate the logic?
  FunctionType compile(Kernel &kernel);

  void check_runtime_error();

  Kernel &get_snode_reader(SNode *snode);

  Kernel &get_snode_writer(SNode *snode);

  uint64 fetch_result_uint64(int i);

  template <typename T>
  T fetch_result(int i) {
    return taichi_union_cast_with_different_sizes<T>(fetch_result_uint64(i));
  }

  Arch get_host_arch() {
    return host_arch();
  }

  float64 get_total_compilation_time() {
    return total_compilation_time_;
  }

  void finalize();

  static int get_kernel_id() {
    static int id = 0;
    TI_ASSERT(id < 100000);
    return id++;
  }

  static int default_block_dim(const CompileConfig &config);

  // Note this method is specific to LlvmProgramImpl, but we keep it here since
  // it's exposed to python.
  void print_memory_profiler_info();

  // Returns zero if the SNode is statically allocated
  std::size_t get_snode_num_dynamically_allocated(SNode *snode);

  inline SNodeFieldMap *get_snode_to_fields() {
    return &snode_to_fields_;
  }

  inline SNodeRwAccessorsBank &get_snode_rw_accessors_bank() {
    return snode_rw_accessors_bank_;
  }

  /**
   * Destroys a new SNode tree.
   *
   * @param snode_tree The pointer to SNode tree.
   */
  void destroy_snode_tree(SNodeTree *snode_tree);

  /**
   * Adds a new SNode tree.
   *
   * @param root The root of the new SNode tree.
   * @param compile_only Only generates the compiled type
   * @return The pointer to SNode tree.
   *
   * FIXME: compile_only is mostly a hack to make AOT & cross-compilation work.
   * E.g. users who would like to AOT to a specific target backend can do so,
   * even if their platform doesn't support that backend. Unfortunately, the
   * current implementation would leave the backend in a mostly broken state. We
   * need a cleaner design to support both AOT and JIT modes.
   */
  SNodeTree *add_snode_tree(std::unique_ptr<SNode> root, bool compile_only);

  /**
   * Allocates a SNode tree id for a new SNode tree
   *
   * @return The SNode tree id allocated
   *
   * Returns and consumes a free SNode tree id if there is any,
   * Otherwise returns the size of `snode_trees_`
   */
  int allocate_snode_tree_id();

  /**
   * Gets the root of a SNode tree.
   *
   * @param tree_id Index of the SNode tree
   * @return Root of the tree
   */
  SNode *get_snode_root(int tree_id);

  std::unique_ptr<AotModuleBuilder> make_aot_module_builder(
      Arch arch,
      const std::vector<std::string> &caps);

  size_t get_field_in_tree_offset(int tree_id, const SNode *child) {
    return program_impl_->get_field_in_tree_offset(tree_id, child);
  }

  DevicePtr get_snode_tree_device_ptr(int tree_id) {
    return program_impl_->get_snode_tree_device_ptr(tree_id);
  }

  Device *get_compute_device() {
    return program_impl_->get_compute_device();
  }

  Device *get_graphics_device() {
    return program_impl_->get_graphics_device();
  }

  // TODO: do we still need result_buffer?
  DeviceAllocation allocate_memory_ndarray(std::size_t alloc_size,
                                           uint64 *result_buffer) {
    return program_impl_->allocate_memory_ndarray(alloc_size, result_buffer);
  }
  DeviceAllocation allocate_texture(const ImageParams &params) {
    return program_impl_->allocate_texture(params);
  }

  Ndarray *create_ndarray(
      const DataType type,
      const std::vector<int> &shape,
      ExternalArrayLayout layout = ExternalArrayLayout::kNull,
      bool zero_fill = false);

  void delete_ndarray(Ndarray *ndarray);

  Texture *create_texture(const DataType type,
                          int num_channels,
                          const std::vector<int> &shape);

  intptr_t get_ndarray_data_ptr_as_int(const Ndarray *ndarray);

  void fill_ndarray_fast_u32(Ndarray *ndarray, uint32_t val);

  Identifier get_next_global_id(const std::string &name = "") {
    return Identifier(global_id_counter_++, name);
  }

  void prepare_runtime_context(RuntimeContext *ctx);

  /** Enqueue a custom compute op to the current program execution flow.
   *
   *  @params op The lambda that is invoked to construct the custom compute Op
   *  @params image_refs The image resource references used in this compute Op
   */
  void enqueue_compute_op_lambda(
      std::function<void(Device *device, CommandList *cmdlist)> op,
      const std::vector<ComputeOpImageRef> &image_refs);

  /**
   * TODO(zhanlue): Remove this interface
   *
   * Gets the underlying ProgramImpl object
   *
   * This interface is essentially a hack to temporarily accommodate
   * historical design issues with LLVM backend
   *
   * Please limit its use to LLVM backend only
   */
  ProgramImpl *get_program_impl() {
    TI_ASSERT(arch_uses_llvm(this_thread_config().arch));
    return program_impl_.get();
  }

  // TODO(zhanlue): Move these members and corresponding interfaces to
  // ProgramImpl Ideally, Program should serve as a pure interface class and all
  // the implementations should fall inside ProgramImpl
  //
  // Once we migrated these implementations to ProgramImpl, lower-level objects
  // could store ProgramImpl rather than Program.

 private:
  uint64 ndarray_writer_counter_{0};
  uint64 ndarray_reader_counter_{0};
  int global_id_counter_{0};

  // SNode information that requires using Program.
  SNodeFieldMap snode_to_fields_;
  SNodeRwAccessorsBank snode_rw_accessors_bank_;

  std::vector<std::unique_ptr<SNodeTree>> snode_trees_;
  std::stack<int> free_snode_tree_ids_;

  std::vector<std::unique_ptr<Function>> functions_;
  std::unordered_map<FunctionKey, Function *> function_map_;

  std::unique_ptr<ProgramImpl> program_impl_;
  float64 total_compilation_time_{0.0};
  static std::atomic<int> num_instances_;
  bool finalized_{false};

  std::unique_ptr<MemoryPool> memory_pool_{nullptr};
  // TODO: Move ndarrays_ and textures_ to be managed by runtime
  std::unordered_map<void *, std::unique_ptr<Ndarray>> ndarrays_;
  std::vector<std::unique_ptr<Texture>> textures_;
  std::shared_mutex config_map_mut;
};

}  // namespace taichi::lang
