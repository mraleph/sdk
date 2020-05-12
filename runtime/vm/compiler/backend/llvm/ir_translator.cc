#include "vm/compiler/backend/llvm/ir_translator.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include <unordered_map>

#include "vm/compiler/aot/precompiler.h"
#include "vm/compiler/assembler/object_pool_builder.h"
#include "vm/compiler/backend/il_printer.h"
#include "vm/compiler/backend/llvm/compile.h"
#include "vm/compiler/backend/llvm/compiler_state.h"
#include "vm/compiler/backend/llvm/initialize_llvm.h"
#include "vm/compiler/backend/llvm/liveness_analysis.h"
#include "vm/compiler/backend/llvm/output.h"
#include "vm/compiler/backend/llvm/stack_map_info.h"
#include "vm/compiler/backend/llvm/target_specific.h"
#include "vm/compiler/runtime_api.h"
#include "vm/object_store.h"
#include "vm/type_testing_stubs.h"

namespace dart {
namespace dart_llvm {
namespace {
class AnonImpl;
class Label;
class AssemblerResolver;
struct NotMergedPhiDesc {
  BlockEntryInstr* pred;
  int value;
  LValue phi;
};

struct GCRelocateDesc {
  int ssa_index;
  int where;
  GCRelocateDesc(int _ssa_index, int w) : ssa_index(_ssa_index), where(w) {}
};

using GCRelocateDescList = std::vector<GCRelocateDesc>;

enum class ValueType { LLVMValue };

struct ValueDesc {
  ValueType type;
  union {
    LValue llvm_value;
  };
};

using ExceptionBlockLiveInValueMap = std::unordered_map<int, ValueDesc>;

struct ExceptionBlockLiveInEntry {
  ExceptionBlockLiveInEntry(ExceptionBlockLiveInValueMap&& _live_in_value_map,
                            LBasicBlock _from_block);
  ExceptionBlockLiveInValueMap live_in_value_map;
  LBasicBlock from_block;
};

struct IRTranslatorBlockImpl {
  std::vector<NotMergedPhiDesc> not_merged_phis;
  std::unordered_map<int, ValueDesc> values_;
  // values for exception block.
  std::unordered_map<int, ValueDesc> exception_values_;

  LBasicBlock native_bb = nullptr;
  LBasicBlock continuation = nullptr;
  // exception vals
  LValue exception_val = nullptr;
  LValue stacktrace_val = nullptr;
  std::vector<ExceptionBlockLiveInEntry> exception_live_in_entries;

  AnonImpl* anon_impl;
  int try_index = kInvalidTryIndex;

  bool started = false;
  bool ended = false;
  bool need_merge = false;
  bool exception_block = false;

  inline void SetLLVMValue(int ssa_id, LValue value) {
    values_[ssa_id] = {ValueType::LLVMValue, {value}};
  }

  inline void SetValue(int nid, const ValueDesc& value) {
    values_[nid] = value;
  }

  LValue GetLLVMValue(int ssa_id);

  LValue GetLLVMValue(Value* v) {
    return GetLLVMValue(v->definition()->ssa_temp_index());
  }

  bool IsDefined(Definition* def) {
    auto found = values_.find(def->ssa_temp_index());
    return found != values_.end();
  }

#if 0
  LValue GetLLVMValue(Definition* d) {
    return GetLLVMValue(d->ssa_temp_index());
  }
#endif

  const ValueDesc& GetValue(int nid) const {
    auto found = values_.find(nid);
    EMASSERT(found != values_.end());
    return found->second;
  }

  const ValueDesc& GetValue(Value* v) const {
    return GetValue(v->definition()->ssa_temp_index());
  }

  inline std::unordered_map<int, ValueDesc>& values() { return values_; }

  inline void StartTranslate() {
    EMASSERT(!started);
    EMASSERT(!ended);
    started = true;
  }

  inline void EndTranslate() {
    EMASSERT(started);
    EMASSERT(!ended);
    ended = true;
  }
};

class ValueInterceptor {
 public:
  virtual ~ValueInterceptor() = default;
  // return true means intercepted, and anon impl must not record this value.
  // return false means not intercepted, and anon impl must record this value.
  virtual bool SetLLVMValue(int, LValue) = 0;
  virtual LValue GetLLVMValue(int) = 0;
};

class AnonImpl {
 public:
  AnonImpl() = default;
  ~AnonImpl() = default;
  LBasicBlock GetNativeBB(BlockEntryInstr* bb);
  LBasicBlock EnsureNativeBB(BlockEntryInstr* bb);
  IRTranslatorBlockImpl* GetTranslatorBlockImpl(BlockEntryInstr* bb);
  LBasicBlock GetNativeBBContinuation(BlockEntryInstr* bb);
  bool IsBBStartedToTranslate(BlockEntryInstr* bb);
  bool IsBBEndedToTranslate(BlockEntryInstr* bb);
  void StartTranslate(BlockEntryInstr* bb);
  void MergePredecessors(BlockEntryInstr* bb);
  void MergeExceptionVirtualPredecessors(BlockEntryInstr* bb);
  // When called, the current builder position must point to the end of catch block's native bb.
  void MergeExceptionLiveInEntries(const ExceptionBlockLiveInEntry& e,
                                   BlockEntryInstr* catch_block,
                                   IRTranslatorBlockImpl* catch_block_impl);
  bool AllPredecessorStarted(BlockEntryInstr* bb, BlockEntryInstr** ref_pred);
  void End();
  void EndCurrentBlock();
  void ProcessPhiWorkList();
  void BuildPhiAndPushToWorkList(BlockEntryInstr* bb,
                                 BlockEntryInstr* ref_pred);
  // Phis
  LValue EnsurePhiInput(BlockEntryInstr* pred, int index, LType type);
  LValue EnsurePhiInputAndPosition(BlockEntryInstr* pred,
                                   int index,
                                   LType type);
  // Exception handling
  LBasicBlock EnsureNativeCatchBlock(int try_index);
  IRTranslatorBlockImpl* GetCatchBlockImplInitIfNeeded(
      CatchBlockEntryInstr* instr);
  // Debug & line info.
  void SetDebugLine(Instruction*);
  // Access to memory.
  LValue BuildAccessPointer(LValue base, LValue offset, Representation rep);
  LValue BuildAccessPointer(LValue base, LValue offset, LType type);
  // Calls
  int NextPatchPoint();
  void SubmitStackMap(std::unique_ptr<StackMapInfo> info);
  void PushArgument(LValue v);
  LValue GenerateCall(
      Instruction*,
      TokenPosition token_pos,
      intptr_t deopt_id,
      const Code& stub,
      RawPcDescriptors::Kind kind,
      size_t stack_argument_count,
      const std::vector<std::pair<Register, LValue>>& gp_parameters);
  LValue GenerateRuntimeCall(Instruction*,
                             TokenPosition token_pos,
                             intptr_t deopt_id,
                             const RuntimeEntry& entry,
                             intptr_t argument_count,
                             bool return_on_stack = true);
  LValue GenerateStaticCall(
      Instruction*,
      int ssa_index,
      const std::vector<LValue>& arguments,
      intptr_t deopt_id,
      TokenPosition token_pos,
      const Function& function,
      ArgumentsInfo args_info,
      const ICData& ic_data_in,
      Code::EntryKind entry_kind = Code::EntryKind::kNormal);
  LValue GenerateMegamorphicInstanceCall(Instruction* instr,
                                         const String& name,
                                         const Array& arguments_descriptor,
                                         intptr_t deopt_id,
                                         TokenPosition token_pos);
  // InstanceOf
#if 0
  RawSubtypeTestCache* GenerateInlineInstanceof(
      LValue value,
      LValue instantiator_type_arguments, 
      LValue function_type_arguments,
      AssemblerResolver& resolver,
      TokenPosition token_pos,
      const AbstractType& type,
      Label& is_instance_lbl,
      Label& is_not_instance_lbl);
#endif
  // Types
  LType GetMachineRepresentationType(Representation);
  LValue TaggedToWord(LValue v);
  LValue WordToTagged(LValue v);
  LValue EnsureBoolean(LValue v);
  LValue EnsureIntPtr(LValue v);
  LValue EnsureInt32(LValue v);
  LValue SmiTag(LValue v);
  LValue SmiUntag(LValue v);
  LValue TstSmi(LValue v);
  LValue BooleanToObject(LValue boolean);
  LValue LoadClassId(LValue o);
  LValue CompareClassId(LValue o, intptr_t clsid);
  LValue CompareObject(LValue, const Object&);

  // expect
  LValue ExpectTrue(LValue);
  LValue ExpectFalse(LValue);

  // Value helpers for current bb
  LValue GetLLVMValue(int ssa_id);

  LValue GetLLVMValue(Value* v) {
    return GetLLVMValue(v->definition()->ssa_temp_index());
  }
  LValue GetLLVMValue(Definition* d) {
    return GetLLVMValue(d->ssa_temp_index());
  }
  void SetLLVMValue(int ssa_id, LValue v);
  void SetLLVMValue(Definition* d, LValue v);
  void SetLazyValue(Definition*);
  // MaterializeDef
  LValue MaterializeDef(Definition* d);
  bool IsLazyValue(int ssa_index);

  // Load Store
  compiler::ObjectPoolBuilder& object_pool_builder() {
    return *precompiler_->global_object_pool_builder();
  }
  LValue LoadFieldFromOffset(LValue base, int offset, LType type);
  LValue LoadFieldFromOffset(LValue base, int offset);
  LValue LoadFromOffset(LValue base, int offset, LType type);
  LValue LoadObject(const Object& object, bool is_unique = false);

  void StoreToOffset(LValue base, int offset, LValue v);
  void StoreToOffset(LValue base, LValue offset, LValue v);
  void StoreIntoObject(Instruction* instruction,
                       LValue object,  // Object we are storing into.
                       LValue dest,    // Where we are storing into.
                       LValue value,   // Value we are storing.
                       compiler::Assembler::CanBeSmi can_value_be_smi =
                           compiler::Assembler::kValueCanBeSmi);
  void StoreIntoArray(Instruction* instruction,
                      LValue object,  // Object we are storing into.
                      LValue dest,    // Where we are storing into.
                      LValue value,   // Value we are storing.
                      compiler::Assembler::CanBeSmi can_value_be_smi =
                          compiler::Assembler::kValueCanBeSmi);
  // Allocate
  LValue TryAllocate(AssemblerResolver& resolver,
                     Label& failure,
                     intptr_t cid,
                     intptr_t instance_size);
  LValue BoxInteger32(Instruction*, LValue value, bool fit, Representation rep);

  // Basic blocks continuation
  LBasicBlock NewBasicBlock(const char* fmt, ...);
  void SetCurrentBlockContinuation(LBasicBlock continuation);

  inline CompilerState& compiler_state() { return *compiler_state_; }
  inline LivenessAnalysis& liveness() { return *liveness_analysis_; }
  inline Output& output() { return *output_; }
  inline IRTranslatorBlockImpl* current_bb_impl() { return current_bb_impl_; }
  inline BlockEntryInstr* current_bb() { return current_bb_; }
  inline FlowGraph* flow_graph() { return flow_graph_; }
  inline Thread* thread() { return thread_; }

  // classes
  const Class& mint_class() {
    return Class::ZoneHandle(
        flow_graph()->isolate()->object_store()->mint_class());
  }

  const Class& double_class() {
    return Class::ZoneHandle(
        flow_graph()->isolate()->object_store()->double_class());
  }

  Zone* zone() { return flow_graph_->zone(); }

  BlockEntryInstr* current_bb_;
  IRTranslatorBlockImpl* current_bb_impl_;
  FlowGraph* flow_graph_;
  Precompiler* precompiler_;
  std::unique_ptr<CompilerState> compiler_state_;
  std::unique_ptr<LivenessAnalysis> liveness_analysis_;
  std::unique_ptr<Output> output_;
  std::unordered_map<BlockEntryInstr*, IRTranslatorBlockImpl> block_map_;
  StackMapInfoMap stack_map_info_map;
  std::vector<BlockEntryInstr*> phi_rebuild_worklist_;
  std::vector<LBasicBlock> catch_blocks_;
  // lazy values
  std::unordered_map<int, Definition*> lazy_values_;
  std::unordered_map<int, LValue> lazy_values_block_cache_;

  // Value intercept
  ValueInterceptor* value_interceptor_ = nullptr;

  // debug lines
  std::vector<Instruction*> debug_instrs_;
  // Calls
  std::vector<LValue> pushed_arguments_;
  Thread* thread_;
  int patch_point_id_ = 0;
  bool visited_function_entry_ = false;
  DISALLOW_COPY_AND_ASSIGN(AnonImpl);
};

class ContinuationResolver {
 protected:
  ContinuationResolver(AnonImpl& impl, int ssa_id);
  ~ContinuationResolver() = default;
  inline AnonImpl& impl() { return impl_; }
  inline Output& output() { return impl().output(); }
  inline BlockEntryInstr* current_bb() { return impl().current_bb(); }
  inline int ssa_id() const { return ssa_id_; }

 private:
  AnonImpl& impl_;
  int ssa_id_;
  DISALLOW_COPY_AND_ASSIGN(ContinuationResolver);
};

class DiamondContinuationResolver : public ContinuationResolver {
 public:
  DiamondContinuationResolver(AnonImpl& impl, int ssa_id);
  ~DiamondContinuationResolver() = default;
  DiamondContinuationResolver& BuildCmp(std::function<LValue()>);
  DiamondContinuationResolver& BuildLeft(std::function<LValue()>);
  DiamondContinuationResolver& BuildRight(std::function<LValue()>);
  LValue End();
  void BranchToOtherLeafOnCondition(LValue cond);
  LBasicBlock left_block() { return blocks_[0]; }
  LBasicBlock right_block() { return blocks_[1]; }
  void LeftHint() { hint_ = -1; }
  void RightHint() { hint_ = 1; }

 private:
  LBasicBlock blocks_[3];
  LValue values_[2];
  // 0 no hint
  // -1 left hint
  // 1 right hint
  int hint_;
  int building_block_index_;
};

class CallResolver : public ContinuationResolver {
 public:
  struct CallResolverParameter {
    CallResolverParameter(Instruction*, size_t, std::unique_ptr<CallSiteInfo>);
    ~CallResolverParameter() = default;
    void set_second_return(bool);
    Instruction* call_instruction;
    size_t instruction_size;
    std::unique_ptr<CallSiteInfo> callsite_info;
    bool second_return;
    DISALLOW_COPY_AND_ASSIGN(CallResolverParameter);
  };
  explicit CallResolver(AnonImpl& impl,
                        int ssa_id,
                        CallResolverParameter& call_resolver_parameter);
  ~CallResolver() = default;
  void SetGParameter(int reg, LValue);
  void AddStackParameter(LValue);
  LValue GetStackParameter(size_t i);
  LValue BuildCall();

 private:
  void GenerateStatePointFunction();
  void ExtractCallInfo();
  void EmitCall();
  void EmitRelocatesIfNeeded();
  void EmitExceptionBlockIfNeeded();
  void EmitPatchPoint();
  bool need_invoke() { return tail_call_ == false && catch_block_ != nullptr; }

  CallResolverParameter& call_resolver_parameter_;

  // state in the middle.
  LValue statepoint_function_ = nullptr;
  LType callee_type_ = nullptr;
  LType return_type_ = nullptr;
  LValue call_value_ = nullptr;
  LValue statepoint_value_ = nullptr;
  // exception blocks
  IRTranslatorBlockImpl* catch_block_impl_ = nullptr;
  LBasicBlock continuation_block_ = nullptr;
  CatchBlockEntryInstr* catch_block_ = nullptr;
  BitVector* call_live_out_ = nullptr;
  GCRelocateDescList gc_desc_list_;
  ExceptionBlockLiveInValueMap exception_block_live_in_value_map_;
  std::vector<LValue> parameters_;
  int patchid_ = 0;
  bool tail_call_ = false;
  DISALLOW_COPY_AND_ASSIGN(CallResolver);
};

class BoxAllocationSlowPath {
 public:
  BoxAllocationSlowPath(Instruction* instruction,
                        const Class& cls,
                        AnonImpl& impl);
  LValue Allocate();
  AnonImpl& impl() { return impl_; }
  Output& output() { return *impl_.output_; }

 private:
  LValue BuildSlowPath();
  Instruction* instruction_;
  const Class& cls_;
  AnonImpl& impl_;
  DISALLOW_COPY_AND_ASSIGN(BoxAllocationSlowPath);
};

class ComparisonResolver : public FlowGraphVisitor {
 public:
  explicit ComparisonResolver(AnonImpl& impl);
  ~ComparisonResolver() = default;
  AnonImpl& impl() { return impl_; }
  Output& output() { return *impl_.output_; }
  LValue result() {
    EMASSERT(result_ != nullptr);
    return result_;
  }

 private:
  void VisitStrictCompare(StrictCompareInstr* instr) override;
  void VisitEqualityCompare(EqualityCompareInstr* instr) override;
  void VisitCheckedSmiComparison(CheckedSmiComparisonInstr* instr) override;
  void VisitCaseInsensitiveCompare(CaseInsensitiveCompareInstr* instr) override;

  AnonImpl& impl_;
  LValue result_;
};

class Label {
 public:
  Label(const char* fmt, ...);

 private:
  void InitBBIfNeeded(AnonImpl& impl);
  LBasicBlock basic_block() { return bb_; }
  friend class AssemblerResolver;
  std::string name_;
  LBasicBlock bb_;
};

class AssemblerResolver : public ValueInterceptor {
 public:
  AssemblerResolver(AnonImpl&);
  ~AssemblerResolver() override;

  void Branch(Label&);
  void BranchIf(LValue cond, Label&);
  void BranchIfNot(LValue cond, Label&);
  void Bind(Label&);
  LValue End();
  void GotoMerge();
  void GotoMergeIf(LValue cond);
  void GotoMergeIfNot(LValue cond);
  void GotoMergeWithValue(LValue v);
  void GotoMergeWithValueIf(LValue cond, LValue v);
  void GotoMergeWithValueIfNot(LValue cond, LValue v);
  LBasicBlock current() { return current_; }

 private:
  bool SetLLVMValue(int ssa_id, LValue index) override;
  LValue GetLLVMValue(int ssa_id) override;
  void AssignBlockInitialValues(LBasicBlock);
  void AssignModifiedValueToMerge();
  void MergeModified();
  Output& output() { return impl_.output(); }
  AnonImpl& impl() { return impl_; }
  AnonImpl& impl_;
  LBasicBlock current_;
  LBasicBlock merge_;
  std::vector<LBasicBlock> merge_blocks_;
  std::vector<LValue> merge_values_;

  BitVector* values_modified_;
  // modified merging
  std::unordered_map<LBasicBlock, std::unordered_map<int, LValue>>
      initial_modified_values_lookup_;
  std::unordered_map<int, LValue> current_modified_;
  std::unordered_map<LBasicBlock, std::unordered_map<int, LValue>>
      modified_values_to_merge_;
  ValueInterceptor* old_value_interceptor_;
};

ExceptionBlockLiveInEntry::ExceptionBlockLiveInEntry(
    ExceptionBlockLiveInValueMap&& _live_in_value_map,
    LBasicBlock _from_block)
    : live_in_value_map(std::move(_live_in_value_map)),
      from_block(_from_block) {}

LValue IRTranslatorBlockImpl::GetLLVMValue(int ssa_id) {
  auto found = values_.find(ssa_id);
  EMASSERT(found != values_.end());
  EMASSERT(found->second.type == ValueType::LLVMValue);
  EMASSERT(found->second.llvm_value);
  return found->second.llvm_value;
}

IRTranslatorBlockImpl* AnonImpl::GetTranslatorBlockImpl(BlockEntryInstr* bb) {
  auto& ref = block_map_[bb];
  if (ref.anon_impl == nullptr) {
    // init.
    ref.anon_impl = this;
  }
  return &ref;
}

LBasicBlock AnonImpl::EnsureNativeBB(BlockEntryInstr* bb) {
  IRTranslatorBlockImpl* impl = GetTranslatorBlockImpl(bb);
  if (impl->native_bb) return impl->native_bb;
  char buf[256];
  snprintf(buf, 256, "B%d", static_cast<int>(bb->block_id()));
  LBasicBlock native_bb = output_->appendBasicBlock(buf);
  impl->native_bb = native_bb;
  impl->continuation = native_bb;
  return native_bb;
}

LBasicBlock AnonImpl::GetNativeBB(BlockEntryInstr* bb) {
  return GetTranslatorBlockImpl(bb)->native_bb;
}

LBasicBlock AnonImpl::GetNativeBBContinuation(BlockEntryInstr* bb) {
  return GetTranslatorBlockImpl(bb)->continuation;
}

bool AnonImpl::IsBBStartedToTranslate(BlockEntryInstr* bb) {
  auto impl = GetTranslatorBlockImpl(bb);
  return impl->started;
}

bool AnonImpl::IsBBEndedToTranslate(BlockEntryInstr* bb) {
  auto impl = GetTranslatorBlockImpl(bb);
  return impl->ended;
}

void AnonImpl::StartTranslate(BlockEntryInstr* bb) {
  current_bb_ = bb;
  current_bb_impl_ = GetTranslatorBlockImpl(bb);
  current_bb_impl()->StartTranslate();
  EnsureNativeBB(bb);
  output_->positionToBBEnd(GetNativeBB(bb));
}

void AnonImpl::MergePredecessors(BlockEntryInstr* bb) {
  intptr_t predecessor_count = bb->PredecessorCount();
  if (predecessor_count == 0) return;
  IRTranslatorBlockImpl* block_impl = GetTranslatorBlockImpl(bb);
  EMASSERT(!block_impl->exception_block);
  if (predecessor_count == 1) {
    // Don't use phi if only one predecessor.
    BlockEntryInstr* pred = bb->PredecessorAt(0);
    IRTranslatorBlockImpl* pred_block_impl = GetTranslatorBlockImpl(pred);
    EMASSERT(IsBBStartedToTranslate(pred));
    for (BitVector::Iterator it(liveness().GetLiveInSet(bb)); !it.Done();
         it.Advance()) {
      int live = it.Current();
      if (IsLazyValue(live)) continue;
      auto& value = pred_block_impl->GetValue(live);
      block_impl->SetValue(live, value);
    }
    return;
  }
  BlockEntryInstr* ref_pred = nullptr;
  if (!AllPredecessorStarted(bb, &ref_pred)) {
    EMASSERT(!!ref_pred);
    BuildPhiAndPushToWorkList(bb, ref_pred);
    return;
  }
  // Use phi.
  for (BitVector::Iterator it(liveness().GetLiveInSet(bb)); !it.Done();
       it.Advance()) {
    int live = it.Current();
    auto& value = GetTranslatorBlockImpl(ref_pred)->GetValue(live);
    if (value.type != ValueType::LLVMValue) {
      block_impl->SetValue(live, value);
      continue;
    }
    LValue ref_value = value.llvm_value;
    LType ref_type = typeOf(ref_value);
    if (ref_type != output().tagged_type()) {
      // FIXME: Should add EMASSERT that all values are the same.
      block_impl->SetLLVMValue(live, ref_value);
      continue;
    }
    LValue phi = output().buildPhi(ref_type);
    for (intptr_t i = 0; i < predecessor_count; ++i) {
      BlockEntryInstr* pred = bb->PredecessorAt(i);
      LValue value = GetTranslatorBlockImpl(pred)->GetLLVMValue(live);
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &native, 1);
    }
    block_impl->SetLLVMValue(live, phi);
  }
}

void AnonImpl::MergeExceptionVirtualPredecessors(BlockEntryInstr* bb) {
  IRTranslatorBlockImpl* block_impl = current_bb_impl();
  LValue landing_pad = output().buildLandingPad();
  for (auto& e : block_impl->exception_live_in_entries) {
    MergeExceptionLiveInEntries(e, current_bb(), block_impl);
  }
  block_impl->exception_live_in_entries.clear();
  block_impl->exception_val =
      output().buildCall(output().repo().gcExceptionIntrinsic(), landing_pad);
  block_impl->stacktrace_val = output().buildCall(
      output().repo().gcExceptionDataIntrinsic(), landing_pad);
}

void AnonImpl::MergeExceptionLiveInEntries(
    const ExceptionBlockLiveInEntry& e,
    BlockEntryInstr* catch_block,
    IRTranslatorBlockImpl* catch_block_impl) {
  auto& values = catch_block_impl->values();
  auto MergeNotExist = [&](int ssa_id, const ValueDesc& value_desc_incoming) {
    if (value_desc_incoming.type != ValueType::LLVMValue) {
      values.emplace(ssa_id, value_desc_incoming);
    } else {
      LValue phi = output().buildPhi(typeOf(value_desc_incoming.llvm_value));
      addIncoming(phi, &value_desc_incoming.llvm_value, &e.from_block, 1);
      ValueDesc value_desc{ValueType::LLVMValue, {phi}};
      values.emplace(ssa_id, value_desc);
    }
  };
  auto MergeExist = [&](int ssa_id, ValueDesc& value_desc,
                        const ValueDesc& value_desc_incoming) {
    if (value_desc_incoming.type != ValueType::LLVMValue) {
      EMASSERT(value_desc_incoming.type == value_desc.type);
    } else {
      LValue phi = value_desc.llvm_value;
      EMASSERT(typeOf(phi) == typeOf(value_desc_incoming.llvm_value));
      addIncoming(phi, &value_desc_incoming.llvm_value, &e.from_block, 1);
    }
  };
  for (BitVector::Iterator it(liveness().GetLiveInSet(catch_block)); !it.Done();
       it.Advance()) {
    int live_ssa_index = it.Current();
    auto found_incoming = e.live_in_value_map.find(live_ssa_index);
    EMASSERT(found_incoming != e.live_in_value_map.end());
    auto& value_desc_incoming = found_incoming->second;
    auto found = values.find(live_ssa_index);
    if (found == values.end()) {
      MergeNotExist(live_ssa_index, value_desc_incoming);
    } else {
      MergeExist(live_ssa_index, found->second, value_desc_incoming);
    }
  }
}

bool AnonImpl::AllPredecessorStarted(BlockEntryInstr* bb,
                                     BlockEntryInstr** ref_pred) {
  bool ret_value = true;
  intptr_t predecessor_count = bb->PredecessorCount();
  for (intptr_t i = 0; i < predecessor_count; ++i) {
    BlockEntryInstr* pred = bb->PredecessorAt(i);
    if (IsBBStartedToTranslate(pred)) {
      if (!*ref_pred) *ref_pred = pred;
    } else {
      ret_value = false;
    }
  }
  return ret_value;
}

void AnonImpl::BuildPhiAndPushToWorkList(BlockEntryInstr* bb,
                                         BlockEntryInstr* ref_pred) {
  IRTranslatorBlockImpl* block_impl = GetTranslatorBlockImpl(bb);
  IRTranslatorBlockImpl* ref_pred_impl = GetTranslatorBlockImpl(ref_pred);
  for (BitVector::Iterator it(liveness().GetLiveInSet(bb)); !it.Done();
       it.Advance()) {
    int live = it.Current();
    const ValueDesc& value_desc = ref_pred_impl->GetValue(live);
    if (value_desc.type != ValueType::LLVMValue) {
      block_impl->SetValue(live, value_desc);
      continue;
    }
    LValue ref_value = value_desc.llvm_value;
    LType ref_type = typeOf(ref_value);
    if (ref_type != output().tagged_type()) {
      block_impl->SetLLVMValue(live, ref_value);
      continue;
    }
    LValue phi = output().buildPhi(ref_type);
    block_impl->SetLLVMValue(live, phi);
    intptr_t predecessor_count = bb->PredecessorCount();
    for (intptr_t i = 0; i < predecessor_count; ++i) {
      BlockEntryInstr* pred = bb->PredecessorAt(i);
      if (!IsBBStartedToTranslate(pred)) {
        block_impl->not_merged_phis.emplace_back();
        NotMergedPhiDesc& not_merged_phi = block_impl->not_merged_phis.back();
        not_merged_phi.phi = phi;
        not_merged_phi.value = live;
        not_merged_phi.pred = pred;
        continue;
      }
      LValue value = GetTranslatorBlockImpl(pred)->GetLLVMValue(live);
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &native, 1);
    }
  }
  phi_rebuild_worklist_.push_back(bb);
}

void AnonImpl::End() {
  EMASSERT(!!current_bb_);
  EndCurrentBlock();
  ProcessPhiWorkList();
  output().positionToBBEnd(output().prologue());
  output().buildBr(GetNativeBB(flow_graph_->graph_entry()));
  output().finalize();
  output().EmitDebugInfo(std::move(debug_instrs_));
  Compile(compiler_state());
}

void AnonImpl::EndCurrentBlock() {
  GetTranslatorBlockImpl(current_bb_)->EndTranslate();
  lazy_values_block_cache_.clear();
  current_bb_ = nullptr;
  current_bb_impl_ = nullptr;
  pushed_arguments_.clear();
}

void AnonImpl::ProcessPhiWorkList() {
  for (BlockEntryInstr* bb : phi_rebuild_worklist_) {
    auto impl = GetTranslatorBlockImpl(bb);
    for (auto& e : impl->not_merged_phis) {
      BlockEntryInstr* pred = e.pred;
      EMASSERT(IsBBStartedToTranslate(pred));
      LValue value = EnsurePhiInput(pred, e.value, typeOf(e.phi));
      LBasicBlock native = GetNativeBBContinuation(pred);
      addIncoming(e.phi, &value, &native, 1);
    }
    impl->not_merged_phis.clear();
  }
  phi_rebuild_worklist_.clear();
}

LValue AnonImpl::EnsurePhiInput(BlockEntryInstr* pred, int index, LType type) {
  LValue val = GetTranslatorBlockImpl(pred)->GetLLVMValue(index);
  LType value_type = typeOf(val);
  if (value_type == type) return val;
  LValue terminator =
      LLVMGetBasicBlockTerminator(GetNativeBBContinuation(pred));
  if ((value_type == output().repo().intPtr) &&
      (type == output().tagged_type())) {
    output().positionBefore(terminator);
    LValue ret_val = WordToTagged(val);
    return ret_val;
  }
  LLVMTypeKind value_type_kind = LLVMGetTypeKind(value_type);
  if ((LLVMPointerTypeKind == value_type_kind) &&
      (type == output().repo().intPtr)) {
    output().positionBefore(terminator);
    LValue ret_val = TaggedToWord(val);
    return ret_val;
  }
  if ((value_type == output().repo().boolean) &&
      (type == output().repo().intPtr)) {
    output().positionBefore(terminator);
    LValue ret_val = output().buildCast(LLVMZExt, val, output().repo().intPtr);
    return ret_val;
  }
  LLVMTypeKind type_kind = LLVMGetTypeKind(type);
  if ((LLVMIntegerTypeKind == value_type_kind) &&
      (value_type_kind == type_kind)) {
    // handle both integer
    output().positionBefore(terminator);
    LValue ret_val;
    if (LLVMGetIntTypeWidth(value_type) > LLVMGetIntTypeWidth(type)) {
      ret_val = output().buildCast(LLVMTrunc, val, type);
    } else {
      ret_val = output().buildCast(LLVMZExt, val, type);
    }
    return ret_val;
  }
  __builtin_trap();
}

LValue AnonImpl::EnsurePhiInputAndPosition(BlockEntryInstr* pred,
                                           int index,
                                           LType type) {
  LValue value = EnsurePhiInput(pred, index, type);
  output().positionToBBEnd(GetNativeBB(current_bb_));
  return value;
}

LValue AnonImpl::MaterializeDef(Definition* def) {
  LValue v;
  if (UnboxedConstantInstr* unboxed = def->AsUnboxedConstant()) {
    const Object& object = unboxed->value();
    if (object.IsDouble()) {
      const Double& n = Double::Cast(object);
      v = output().constDouble(n.value());
    } else if (object.IsSmi()) {
      v = output().constTagged(compiler::target::ToRawSmi(object));
    } else {
      LLVMLOGE(
          "MaterializeDef: UnboxedConstantInstr failed to interpret: unknown "
          "type: %s",
          object.ToCString());
      UNREACHABLE();
    }
  } else if (ConstantInstr* constant_object = def->AsConstant()) {
    v = LoadObject(constant_object->value());
  } else {
    LLVMLOGE("MaterializeDef: unknown def: %s\n", def->ToCString());
    UNREACHABLE();
  }
  return v;
}

bool AnonImpl::IsLazyValue(int ssa_index) {
  return lazy_values_.find(ssa_index) != lazy_values_.end();
}

LBasicBlock AnonImpl::EnsureNativeCatchBlock(int try_index) {
  char buf[256];
  LBasicBlock native_bb;
  if ((native_bb = catch_blocks_[try_index]) == nullptr) {
    snprintf(buf, 256, "B_catch_block_%d", try_index);
    native_bb = output().appendBasicBlock(buf);
    catch_blocks_[try_index] = native_bb;
  }
  return native_bb;
}

IRTranslatorBlockImpl* AnonImpl::GetCatchBlockImplInitIfNeeded(
    CatchBlockEntryInstr* instr) {
  IRTranslatorBlockImpl* block_impl = GetTranslatorBlockImpl(instr);
  if (block_impl->native_bb) return block_impl;
  LBasicBlock native_bb = EnsureNativeCatchBlock(instr->catch_try_index());
  block_impl->native_bb = native_bb;
  block_impl->exception_block = true;
  return block_impl;
}

void AnonImpl::SetDebugLine(Instruction* instr) {
  debug_instrs_.emplace_back(instr);
  output().setDebugInfo(debug_instrs_.size(), nullptr);
}

LValue AnonImpl::BuildAccessPointer(LValue base,
                                    LValue offset,
                                    Representation rep) {
  return BuildAccessPointer(base, offset,
                            pointerType(GetMachineRepresentationType(rep)));
}

LValue AnonImpl::BuildAccessPointer(LValue base, LValue offset, LType type) {
  LLVMTypeKind kind = LLVMGetTypeKind(typeOf(base));
  if (kind == LLVMIntegerTypeKind) {
    base = output().buildCast(LLVMIntToPtr, base, output().repo().ref8);
  }
  LValue pointer = output().buildGEPWithByteOffset(base, offset, type);
  return pointer;
}

int AnonImpl::NextPatchPoint() {
  return patch_point_id_++;
}

void AnonImpl::SubmitStackMap(std::unique_ptr<StackMapInfo> info) {
  int patchid = info->patchid();
  auto where = stack_map_info_map.emplace(patchid, std::move(info));
  EMASSERT(where.second && "Submit overlapped patch id");
}

void AnonImpl::PushArgument(LValue v) {
  pushed_arguments_.emplace_back(v);
}

LValue AnonImpl::GenerateCall(
    Instruction* instr,
    TokenPosition token_pos,
    intptr_t deopt_id,
    const Code& stub,
    RawPcDescriptors::Kind kind,
    size_t stack_argument_count,
    const std::vector<std::pair<Register, LValue>>& gp_parameters) {
  if (!stub.InVMIsolateHeap()) {
    std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
    callsite_info->set_type(CallSiteInfo::CallTargetType::kStubRelative);
    callsite_info->set_token_pos(token_pos);
    callsite_info->set_code(&stub);
    callsite_info->set_kind(kind);
    callsite_info->set_deopt_id(deopt_id);
    callsite_info->set_stack_parameter_count(stack_argument_count);
    CallResolver::CallResolverParameter param(instr, Instr::kInstrSize,
                                              std::move(callsite_info));
    CallResolver resolver(*this, -1, param);
    for (size_t i = 0; i < stack_argument_count; ++i) {
      LValue argument = pushed_arguments_.back();
      pushed_arguments_.pop_back();
      resolver.AddStackParameter(argument);
    }
    for (auto p : gp_parameters) {
      resolver.SetGParameter(p.first, p.second);
    }
    return resolver.BuildCall();
  } else {
    const int32_t offset = compiler::target::ObjectPool::element_offset(
        object_pool_builder().FindObject(
            compiler::ToObject(stub),
            compiler::ObjectPoolBuilderEntry::kNotPatchable));

    LValue gep = output().buildGEPWithByteOffset(
        output().pp(), output().constIntPtr(offset - kHeapObjectTag),
        pointerType(output().tagged_type()));
    LValue code_object = output().buildInvariantLoad(gep);

    std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
    callsite_info->set_type(CallSiteInfo::CallTargetType::kCodeObject);
    callsite_info->set_token_pos(token_pos);
    callsite_info->set_code(&stub);
    callsite_info->set_kind(kind);
    callsite_info->set_stack_parameter_count(stack_argument_count);
    CallResolver::CallResolverParameter param(instr, Instr::kInstrSize,
                                              std::move(callsite_info));
    CallResolver resolver(*this, -1, param);
    resolver.SetGParameter(static_cast<int>(CODE_REG), code_object);
    for (size_t i = 0; i < stack_argument_count; ++i) {
      LValue argument = pushed_arguments_.back();
      pushed_arguments_.pop_back();
      resolver.AddStackParameter(argument);
    }
    for (auto p : gp_parameters) {
      resolver.SetGParameter(p.first, p.second);
    }
    return resolver.BuildCall();
  }
}

LValue AnonImpl::GenerateRuntimeCall(Instruction* instr,
                                     TokenPosition token_pos,
                                     intptr_t deopt_id,
                                     const RuntimeEntry& runtime_entry,
                                     intptr_t argument_count,
                                     bool return_on_stack) {
  EMASSERT(!runtime_entry.is_leaf());
  LValue runtime_entry_point =
      LoadFromOffset(output().thread(),
                     compiler::target::Thread::OffsetFromThread(&runtime_entry),
                     pointerType(output().repo().ref8));
  LValue target_gep = output().buildGEPWithByteOffset(
      output().thread(),
      compiler::target::Thread::call_to_runtime_entry_point_offset(),
      pointerType(output().repo().ref8));
  LValue target = output().buildLoad(target_gep);
  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  callsite_info->set_type(CallSiteInfo::CallTargetType::kReg);
  callsite_info->set_token_pos(token_pos);
  callsite_info->set_deopt_id(deopt_id);
  callsite_info->set_return_on_stack(return_on_stack);
  callsite_info->set_reg(kRuntimeCallTargetReg);
  callsite_info->set_stack_parameter_count(argument_count);
  CallResolver::CallResolverParameter param(
      instr,
      return_on_stack ? kRuntimeCallReturnOnStackInstrSize
                      : kRuntimeCallInstrSize,
      std::move(callsite_info));
  CallResolver resolver(*this, -1, param);
  resolver.SetGParameter(static_cast<int>(kRuntimeCallTargetReg), target);
  resolver.SetGParameter(static_cast<int>(kRuntimeCallEntryReg),
                         runtime_entry_point);
  resolver.SetGParameter(static_cast<int>(kRuntimeCallArgCountReg),
                         output().constIntPtr(argument_count));
  // add stack parameters.
  EMASSERT(static_cast<intptr_t>(pushed_arguments_.size()) >= argument_count);
  for (intptr_t i = 0; i < argument_count; ++i) {
    LValue argument = pushed_arguments_.back();
    pushed_arguments_.pop_back();
    resolver.AddStackParameter(argument);
  }
  if (return_on_stack) {
    resolver.AddStackParameter(LoadObject(Object::null_object()));
  }
  return resolver.BuildCall();
}

LValue AnonImpl::GenerateStaticCall(Instruction* instr,
                                    int ssa_index,
                                    const std::vector<LValue>& arguments,
                                    intptr_t deopt_id,
                                    TokenPosition token_pos,
                                    const Function& function,
                                    ArgumentsInfo args_info,
                                    const ICData& ic_data_in,
                                    Code::EntryKind entry_kind) {
  const ICData& ic_data = ICData::ZoneHandle(ic_data_in.Original());
  const Array& arguments_descriptor = Array::ZoneHandle(
      zone(), ic_data.IsNull() ? args_info.ToArgumentsDescriptor()
                               : ic_data.arguments_descriptor());
  LValue argument_desc_val = LLVMGetUndef(output().tagged_type());
  if (function.HasOptionalParameters() || function.IsGeneric()) {
    argument_desc_val = LoadObject(arguments_descriptor);
  } else {
    if (!(FLAG_precompiled_mode && FLAG_use_bare_instructions)) {
      argument_desc_val = output().constTagged(0);
    }
  }
  int argument_count = args_info.count_with_type_args;
  EMASSERT(argument_count == static_cast<int>(arguments.size()));

  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  callsite_info->set_type(CallSiteInfo::CallTargetType::kCallRelative);
  callsite_info->set_token_pos(token_pos);
  callsite_info->set_deopt_id(deopt_id);
  callsite_info->set_stack_parameter_count(argument_count);
  callsite_info->set_target(&function);
  callsite_info->set_entry_kind(entry_kind);
  CallResolver::CallResolverParameter param(instr, Instr::kInstrSize,
                                            std::move(callsite_info));
  CallResolver resolver(*this, ssa_index, param);
  resolver.SetGParameter(static_cast<int>(ARGS_DESC_REG), argument_desc_val);
  // setup stack parameters
  for (int i = argument_count - 1; i >= 0; --i) {
    LValue param = arguments[i];
    resolver.AddStackParameter(param);
  }
  return resolver.BuildCall();
}

LValue AnonImpl::GenerateMegamorphicInstanceCall(
    Instruction* instr,
    const String& name,
    const Array& arguments_descriptor,
    intptr_t deopt_id,
    TokenPosition token_pos) {
  const ArgumentsDescriptor args_desc(arguments_descriptor);
  int argument_count = args_desc.Count();
  EMASSERT(argument_count >= static_cast<int>(pushed_arguments_.size()));
  const MegamorphicCache& cache = MegamorphicCache::ZoneHandle(
      zone(),
      MegamorphicCacheTable::Lookup(thread(), name, arguments_descriptor));
  LValue cache_value = LoadObject(cache);
  LValue entry_gep = output().buildGEPWithByteOffset(
      output().thread(),
      compiler::target::Thread::megamorphic_call_checked_entry_offset(),
      pointerType(output().repo().ref8));
  LValue entry = output().buildLoad(entry_gep);

  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  callsite_info->set_type(CallSiteInfo::CallTargetType::kReg);
  callsite_info->set_token_pos(token_pos);
  callsite_info->set_deopt_id(deopt_id);
  callsite_info->set_stack_parameter_count(argument_count);
  CallResolver::CallResolverParameter param(instr, kCallInstrSize,
                                            std::move(callsite_info));
  CallResolver resolver(*this, -1, param);
  resolver.SetGParameter(kCallTargetReg, entry);
  for (int i = 0; i < argument_count; ++i) {
    LValue argument = pushed_arguments_.back();
    pushed_arguments_.pop_back();
    resolver.AddStackParameter(argument);
  }
  LValue receiver = resolver.GetStackParameter((argument_count - 1));
  resolver.SetGParameter(kReceiverReg, receiver);
  resolver.SetGParameter(kICReg, cache_value);
  return resolver.BuildCall();
}

#if 0
RawSubtypeTestCache* AnonImpl::GenerateInlineInstanceof(
    LValue value,
    LValue instantiator_type_arguments, 
    LValue function_type_arguments,
    AssemblerResolver& resolver,
    TokenPosition token_pos,
    const AbstractType& type,
    Label& is_instance,
    Label& is_not_instance) {
  enum TypeTestStubKind {
    kTestTypeOneArg,
    kTestTypeTwoArgs,
    kTestTypeFourArgs,
    kTestTypeSixArgs,
  };
 auto GenerateBoolToJump = [&] (LValue v,
                                           Label& is_true,
                                           Label& is_false) {
  Label fall_through("fall_through");
  LValue equal_to_null = CompareObject(v, Object::null_object());
  resolver.BranchIf(equal_to_null, fall_through);
  LValue equal_to_true = CompareObject(v, Bool::True());
  resolver.BranchIf(equal_to_true, is_true);
  resolver.Branch(is_false);
  resolver.Bind(&fall_through);
}
  auto GenerateCallSubtypeTestStub = [&] (TypeTestStubKind test_kind) {
    const SubtypeTestCache& type_test_cache =
      SubtypeTestCache::ZoneHandle(zone(), SubtypeTestCache::New());
    LValue type_test_cache_obj_val = LoadObject(type_test_cache, true);
    std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
    callsite_info->set_type(CallSiteInfo::CallTargetType::kCodeObject);
    callsite_info->set_token_pos(token_pos);
    callsite_info->set_deopt_id(deopt_id);
    callsite_info->set_stack_parameter_count(argument_count);
    callsite_info->set_return_on_stack(return_on_stack);
    CallResolver::CallResolverParameter param(instr, return_on_stack ? kRuntimeCallReturnOnStackInstrSize : kRuntimeCallInstrSize, std::move(callsite_info));
    param.set_second_return(true);
    CallResolver call_resolver(*this, -1, param);
    LValue code_object;
    call_resolver.SetGParameter(kInstanceOfInstanceReg, value);

    if (test_kind == kTestTypeOneArg) {
      code_object = LoadObject(StubCode::Subtype1TestCache());
    } else if (test_kind == kTestTypeTwoArgs) {
      code_object = LoadObject(StubCode::Subtype2TestCache());
    } else if (test_kind == kTestTypeFourArgs) {
      call_resolver.SetGParameter(kInstanceOfFunctionTypeReg, function_type_arguments);
      call_resolver.SetGParameter(kInstanceOfInstantiatorTypeReg, instantiator_type_arguments);
      __ BranchLink(StubCode::Subtype4TestCache());
      code_object = LoadObject(StubCode::Subtype4TestCache());
    } else if (test_kind == kTestTypeSixArgs) {
      call_resolver.SetGParameter(kInstanceOfFunctionTypeReg, function_type_arguments);
      call_resolver.SetGParameter(kInstanceOfInstantiatorTypeReg, instantiator_type_arguments);
      code_object = LoadObject(StubCode::Subtype6TestCache());
    } else {
      UNREACHABLE();
    }
    call_resolver.SetGParameter(static_cast<int>(CODE_REG), code_object);
    LValue compose = call_resolver.BuildCall();
    LValue result = output().buildExtractValue(compose, 1);
    // Result is in R1: null -> not found, otherwise Bool::True or Bool::False.
    GenerateBoolToJump(R1, is_instance_lbl, is_not_instance_lbl);
    return type_test_cache.raw();
  };

  auto GenerateFunctionTypeTest = [&] {
    resolver.BranchIf(TstSmi(value), is_not_instance);
    return GenerateCallSubtypeTestStub(kTestTypeSixArgs);
  };
  if (type.IsFunctionType()) {
    return GenerateFunctionTypeTest();
  }
  if (type.IsInstantiated()) {
    const Class& type_class = Class::ZoneHandle(zone(), type.type_class());
    // A class equality check is only applicable with a dst type (not a
    // function type) of a non-parameterized class or with a raw dst type of
    // a parameterized class.
    if (type_class.NumTypeArguments() > 0) {
      return GenerateInstantiatedTypeWithArgumentsTest(
          token_pos, type, is_instance_lbl, is_not_instance_lbl);
      // Fall through to runtime call.
    }
    const bool has_fall_through = GenerateInstantiatedTypeNoArgumentsTest(
        token_pos, type, is_instance_lbl, is_not_instance_lbl);
    if (has_fall_through) {
      // If test non-conclusive so far, try the inlined type-test cache.
      // 'type' is known at compile time.
      return GenerateSubtype1TestCacheLookup(
          token_pos, type_class, is_instance_lbl, is_not_instance_lbl);
    } else {
      return SubtypeTestCache::null();
    }
  }
  return GenerateUninstantiatedTypeTest(token_pos, type, is_instance_lbl,
                                        is_not_instance_lbl);
}
#endif

LType AnonImpl::GetMachineRepresentationType(Representation rep) {
  LType type;
  switch (rep) {
    case kTagged:
      type = output().tagged_type();
      break;
    case kUnboxedDouble:
      type = output().repo().doubleType;
      break;
    case kUnboxedFloat:
      type = output().repo().floatType;
      break;
    case kUnboxedInt32:
    case kUnboxedUint32:
      type = output().repo().int32;
      break;
    case kUnboxedInt64:
      type = output().repo().int64;
      break;
    default:
      UNREACHABLE();
  }
  return type;
}

LValue AnonImpl::TaggedToWord(LValue v) {
  EMASSERT(typeOf(v) == output().tagged_type());
  return output().buildCast(LLVMPtrToInt, v, output().repo().intPtr);
}

LValue AnonImpl::WordToTagged(LValue v) {
  EMASSERT(typeOf(v) == output().repo().intPtr);
  return output().buildCast(LLVMIntToPtr, v, output().tagged_type());
}

LValue AnonImpl::EnsureBoolean(LValue v) {
  LType type = typeOf(v);
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  if (kind == LLVMPointerTypeKind) v = TaggedToWord(v);
  type = typeOf(v);
  if (LLVMGetIntTypeWidth(type) == 1) return v;
  v = output().buildICmp(LLVMIntNE, v, output().repo().intPtrZero);
  return v;
}

LValue AnonImpl::EnsureIntPtr(LValue v) {
  LType type = typeOf(v);
  if (type == output().repo().intPtr) return v;
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  if (kind == LLVMPointerTypeKind)
    return output().buildCast(LLVMZExt, v, output().repo().intPtr);
  return v;
}

LValue AnonImpl::EnsureInt32(LValue v) {
  LType type = typeOf(v);
  if (type == output().repo().int32) return v;
  LLVMTypeKind kind = LLVMGetTypeKind(type);
  if (kind == LLVMPointerTypeKind) {
    return output().buildCast(LLVMPtrToInt, v, output().repo().int32);
  }
  if (type == output().repo().boolean) {
    return output().buildCast(LLVMZExt, v, output().repo().int32);
  }
  EMASSERT(type == output().repo().int32);
  return v;
}

LValue AnonImpl::SmiTag(LValue v) {
  EMASSERT(typeOf(v) == output().repo().intPtr);
  v = output().buildShl(v, output().constIntPtr(kSmiTagSize));
  return WordToTagged(v);
}

LValue AnonImpl::SmiUntag(LValue v) {
  EMASSERT(typeOf(v) == output().tagged_type());
  v = TaggedToWord(v);
  return output().buildSar(v, output().constIntPtr(kSmiTagSize));
}

LValue AnonImpl::TstSmi(LValue v) {
  EMASSERT(typeOf(v) == output().tagged_type());
  LValue address = TaggedToWord(v);
  LValue and_value =
      output().buildAnd(address, output().constIntPtr(kSmiTagMask));
  return output().buildICmp(LLVMIntNE, and_value,
                            output().constIntPtr(kSmiTagMask));
}

LValue AnonImpl::BooleanToObject(LValue boolean) {
  EMASSERT(typeOf(boolean) == output().repo().boolean);
  return output().buildSelect(boolean, LoadObject(Bool::True()),
                              LoadObject(Bool::False()));
}

LValue AnonImpl::LoadClassId(LValue o) {
  const intptr_t class_id_offset =
      compiler::target::Object::tags_offset() +
      compiler::target::RawObject::kClassIdTagPos / kBitsPerByte;
  return LoadFieldFromOffset(o, class_id_offset,
                             pointerType(output().repo().int16));
}

LValue AnonImpl::CompareClassId(LValue o, intptr_t clsid) {
  LValue clsid_val = LoadClassId(o);
  clsid_val = output().buildCast(LLVMZExt, clsid_val, output().repo().intPtr);
  return output().buildICmp(LLVMIntEQ, clsid_val, output().constIntPtr(clsid));
}

LValue AnonImpl::CompareObject(LValue v, const Object& o) {
  LValue o_val = LoadObject(o);
  return output().buildICmp(LLVMIntEQ, v, o_val);
}

LValue AnonImpl::ExpectTrue(LValue cond) {
  EMASSERT(typeOf(cond) == output().repo().boolean);

  return output().buildCall(output().repo().expectIntrinsic(), cond,
                            output().repo().booleanTrue);
}

LValue AnonImpl::ExpectFalse(LValue cond) {
  EMASSERT(typeOf(cond) == output().repo().boolean);

  return output().buildCall(output().repo().expectIntrinsic(), cond,
                            output().repo().booleanFalse);
}

LValue AnonImpl::GetLLVMValue(int ssa_id) {
  auto found_in_lazy_values = lazy_values_.find(ssa_id);
  if (found_in_lazy_values != lazy_values_.end()) {
    auto found_in_cache = lazy_values_block_cache_.find(ssa_id);
    if (found_in_cache != lazy_values_block_cache_.end())
      return found_in_cache->second;
    LValue v = MaterializeDef(found_in_lazy_values->second);
    lazy_values_block_cache_[ssa_id] = v;
    return v;
  }
  if (value_interceptor_) {
    LValue v = value_interceptor_->GetLLVMValue(ssa_id);
    if (v) return v;
  }
  return current_bb_impl()->GetLLVMValue(ssa_id);
}

void AnonImpl::SetLLVMValue(int ssa_id, LValue val) {
  if (value_interceptor_) {
    if (value_interceptor_->SetLLVMValue(ssa_id, val)) return;
  }
  current_bb_impl()->SetLLVMValue(ssa_id, val);
}

void AnonImpl::SetLLVMValue(Definition* d, LValue val) {
  current_bb_impl()->SetLLVMValue(d->ssa_temp_index(), val);
}

void AnonImpl::SetLazyValue(Definition* d) {
  lazy_values_.emplace(d->ssa_temp_index(), d);
}

LValue AnonImpl::LoadFieldFromOffset(LValue base, int offset, LType type) {
  LValue gep = output().buildGEPWithByteOffset(
      base, output().constIntPtr(offset - kHeapObjectTag), type);
  return output().buildLoad(gep);
}

LValue AnonImpl::LoadFieldFromOffset(LValue base, int offset) {
  return LoadFieldFromOffset(base, offset, pointerType(output().tagged_type()));
}

LValue AnonImpl::LoadFromOffset(LValue base, int offset, LType type) {
  LValue gep =
      output().buildGEPWithByteOffset(base, output().constIntPtr(offset), type);
  return output().buildLoad(gep);
}

LValue AnonImpl::LoadObject(const Object& object, bool is_unique) {
  intptr_t offset = 0;
  if (compiler::target::CanLoadFromThread(object, &offset)) {
    // Load common VM constants from the thread. This works also in places where
    // no constant pool is set up (e.g. intrinsic code).
    LValue gep = output().buildGEPWithByteOffset(
        output().thread(), output().constIntPtr(offset),
        pointerType(output().tagged_type()));
    return output().buildInvariantLoad(gep);
  } else if (compiler::target::IsSmi(object)) {
    // Relocation doesn't apply to Smis.
    return output().constIntPtr(
        static_cast<intptr_t>(compiler::target::ToRawSmi(object)));
  } else {
    // Make sure that class CallPattern is able to decode this load from the
    // object pool.
    EMASSERT(FLAG_use_bare_instructions);
    const auto index = is_unique ? object_pool_builder().AddObject(object)
                                 : object_pool_builder().FindObject(object);
    const int32_t offset = compiler::target::ObjectPool::element_offset(index);
    LValue gep = output().buildGEPWithByteOffset(
        output().pp(), output().constIntPtr(offset - kHeapObjectTag),
        pointerType(output().tagged_type()));
    return output().buildInvariantLoad(gep);
  }
}

void AnonImpl::StoreToOffset(LValue base, int offset, LValue v) {
  StoreToOffset(base, output().constIntPtr(offset), v);
}

void AnonImpl::StoreToOffset(LValue base, LValue offset, LValue v) {
  LValue gep =
      output().buildGEPWithByteOffset(base, offset, pointerType(typeOf(v)));
  output().buildStore(v, gep);
}

void AnonImpl::StoreIntoObject(Instruction* instr,
                               LValue object,  // Object we are storing into.
                               LValue dest,    // Where we are storing into.
                               LValue value,   // Value we are storing.
                               compiler::Assembler::CanBeSmi can_value_be_smi) {
  LValue gep =
      BuildAccessPointer(object, dest, pointerType(output().tagged_type()));
  output().buildStore(value, gep);
  DiamondContinuationResolver diamond(*this, -1);
  diamond.BuildCmp([&]() {
    if (can_value_be_smi == compiler::Assembler::kValueCanBeSmi) {
      return TstSmi(value);
    }
    return output().repo().booleanFalse;
  });
  diamond.BuildLeft([&]() {
    // Smi case.
    return value;
  });
  diamond.BuildRight([&]() {
    // None Smi case
    LValue object_tag = LoadFieldFromOffset(
        object, compiler::target::Object::tags_offset(), output().repo().ref8);
    LValue value_tag = LoadFieldFromOffset(
        value, compiler::target::Object::tags_offset(), output().repo().ref8);
    LValue and_value = output().buildAnd(
        value_tag, output().buildShr(
                       object_tag,
                       output().constInt8(
                           compiler::target::RawObject::kBarrierOverlapShift)));
    and_value = output().buildCast(LLVMZExt, and_value, output().repo().intPtr);
    LValue barrier_mask_gep = output().buildGEPWithByteOffset(
        output().thread(),
        output().constIntPtr(
            compiler::target::Thread::write_barrier_mask_offset()),
        pointerType(output().repo().intPtr));
    LValue barrier_mask = output().buildLoad(barrier_mask_gep);
    DiamondContinuationResolver resolver(*this, -1);
    resolver.RightHint();
    resolver.BuildCmp([&]() {
      LValue test_value = output().buildAnd(and_value, barrier_mask);
      return output().buildICmp(LLVMIntNE, test_value, output().constIntPtr(0));
    });
    resolver.BuildLeft([&]() {
      // should emit
      LValue entry_gep = output().buildGEPWithByteOffset(
          output().thread(),
          compiler::target::Thread::write_barrier_wrappers_thread_offset(
              kWriteBarrierValueReg),
          pointerType(output().repo().ref8));
      LValue entry = output().buildLoad(entry_gep);
      std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
      callsite_info->set_type(CallSiteInfo::CallTargetType::kReg);

      CallResolver::CallResolverParameter param(instr, kCallInstrSize,
                                                std::move(callsite_info));
      CallResolver call_resolver(*this, -1, param);
      call_resolver.SetGParameter(kCallTargetReg, entry);
      call_resolver.SetGParameter(kWriteBarrierObjectReg, object);
      call_resolver.SetGParameter(kWriteBarrierValueReg, value);
      call_resolver.BuildCall();
      return value;
    });
    resolver.BuildRight([&]() { return value; });
    return resolver.End();
  });
  diamond.End();
}

void AnonImpl::StoreIntoArray(Instruction* instr,
                              LValue object,  // Object we are storing into.
                              LValue dest,    // Where we are storing into.
                              LValue value,   // Value we are storing.
                              compiler::Assembler::CanBeSmi can_value_be_smi) {
  LValue gep =
      BuildAccessPointer(object, dest, pointerType(output().tagged_type()));
  output().buildStore(value, gep);
  DiamondContinuationResolver diamond(*this, -1);
  diamond.BuildCmp([&]() {
    if (can_value_be_smi == compiler::Assembler::kValueCanBeSmi) {
      return TstSmi(value);
    }
    return output().repo().booleanFalse;
  });
  diamond.BuildLeft([&]() {
    // Smi case.
    return value;
  });
  diamond.BuildRight([&]() {
    // None Smi case
    LValue object_tag = LoadFieldFromOffset(
        object, compiler::target::Object::tags_offset(), output().repo().int8);
    LValue value_tag = LoadFieldFromOffset(
        value, compiler::target::Object::tags_offset(), output().repo().int8);
    LValue and_value = output().buildAnd(
        value_tag, output().buildShr(
                       object_tag,
                       output().constInt8(
                           compiler::target::RawObject::kBarrierOverlapShift)));
    and_value = output().buildCast(LLVMZExt, and_value, output().repo().intPtr);
    LValue barrier_mask_gep = output().buildGEPWithByteOffset(
        output().thread(),
        output().constIntPtr(
            compiler::target::Thread::write_barrier_mask_offset()),
        pointerType(output().repo().intPtr));
    LValue barrier_mask = output().buildLoad(barrier_mask_gep);
    DiamondContinuationResolver resolver(*this, -1);
    resolver.BuildCmp([&]() {
      LValue test_value = output().buildAnd(and_value, barrier_mask);
      return output().buildICmp(LLVMIntNE, test_value, output().constIntPtr(0));
    });
    resolver.BuildLeft([&]() {
      // should emit
      LValue entry_gep = output().buildGEPWithByteOffset(
          output().thread(),
          compiler::target::Thread::array_write_barrier_entry_point_offset(),
          pointerType(output().repo().ref8));
      LValue entry = output().buildLoad(entry_gep);
      std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
      callsite_info->set_type(CallSiteInfo::CallTargetType::kReg);
      CallResolver::CallResolverParameter param(instr, kCallInstrSize,
                                                std::move(callsite_info));
      CallResolver call_resolver(*this, -1, param);
      LValue slot = output().buildAdd(TaggedToWord(object), dest);
      slot = output().buildSub(slot, output().constIntPtr(kHeapObjectTag));
      call_resolver.SetGParameter(kCallTargetReg, entry);
      call_resolver.SetGParameter(kWriteBarrierObjectReg, object);
      call_resolver.SetGParameter(kWriteBarrierValueReg, value);
      call_resolver.SetGParameter(kWriteBarrierSlotReg, slot);
      call_resolver.BuildCall();
      return value;
    });
    resolver.BuildRight([&]() { return value; });
    return resolver.End();
  });
  diamond.End();
}

LValue AnonImpl::TryAllocate(AssemblerResolver& resolver,
                             Label& failure,
                             intptr_t cid,
                             intptr_t instance_size) {
  if (compiler::target::Heap::IsAllocatableInNewSpace(instance_size)) {
    LValue top_offset = output().buildGEPWithByteOffset(
        output().thread(), compiler::target::Thread::top_offset(),
        pointerType(output().repo().intPtr));
    LValue instance = output().buildLoad(top_offset);
    // TODO(koda): Protect against unsigned overflow here.
#if !defined(TARGET_ARCH_IS_64_BIT)
    LValue uadd_overflow =
        output().buildCall(output().repo().uaddWithOverflow32Intrinsic(),
                           instance, output().constInt32(instance_size));
#else
    LValue uadd_overflow =
        output().buildCall(output().repo().uaddWithOverflow64Intrinsic(),
                           instance, output().constInt64(instance_size));
#endif
    LValue end_offset = output().buildGEPWithByteOffset(
        output().thread(), compiler::target::Thread::end_offset(),
        pointerType(output().repo().intPtr));
    LValue end = output().buildLoad(end_offset);

    LValue icmp = output().buildICmp(
        LLVMIntULE, end, output().buildExtractValue(uadd_overflow, 0));
    LValue overflowed = output().buildExtractValue(uadd_overflow, 1);
    LValue cond = output().buildOr(icmp, overflowed);
    resolver.BranchIf(ExpectFalse(cond), failure);
    output().buildStore(output().buildExtractValue(uadd_overflow, 0),
                        top_offset);
    EMASSERT(instance_size >= kHeapObjectTag);
    const uint32_t tags =
        compiler::target::MakeTagWordForNewSpaceObject(cid, instance_size);
    output().buildStore(
        output().constIntPtr(tags),
        BuildAccessPointer(
            instance,
            output().constIntPtr(compiler::target::Object::tags_offset()),
            pointerType(output().repo().intPtr)));
    instance =
        output().buildAdd(instance, output().constIntPtr(kHeapObjectTag));
    return WordToTagged(instance);
  } else {
    resolver.Branch(failure);
    return nullptr;
  }
}

LValue AnonImpl::BoxInteger32(Instruction* instr,
                              LValue value,
                              bool fit,
                              Representation rep) {
  EMASSERT(typeOf(value) == output().repo().int32);
#if defined(TARGET_ARCH_IS_32_BIT)
  LValue result = SmiTag(value);
  if (!fit) {
    AssemblerResolver resolver(*this);
    LValue fit_in_smi;
    if (kUnboxedInt32 == rep) {
      fit_in_smi = output().buildICmp(LLVMIntEQ, value, SmiUntag(result));
    } else {
      // high 2 bit must be 0 for 32bit arch unsigned integer.
      LValue and_value =
          output().buildAnd(value, output().constInt32(0xC0000000));
      fit_in_smi =
          output().buildICmp(LLVMIntEQ, and_value, output().constInt32(0));
    }
    Label slow_path("BoxInteger32SlowPath");
    resolver.BranchIfNot(ExpectTrue(fit_in_smi), slow_path);
    resolver.GotoMergeWithValue(result);

    resolver.Bind(slow_path);
    BoxAllocationSlowPath box_allocate(instr, mint_class(), *this);
    LValue mint = box_allocate.Allocate();
    StoreToOffset(mint, compiler::target::Mint::value_offset() - kHeapObjectTag,
                  value);
    resolver.GotoMergeWithValue(mint);
    result = resolver.End();
  }
#else
  LValue result = output().buildCast(LLVMSExt, value, output().int64);
  result = output().buildShl(result, output().constInt64(kSmiTagSize));
  result = WordToTagged(result);
#endif
  return result;
}

LBasicBlock AnonImpl::NewBasicBlock(const char* fmt, ...) {
  va_list va;
  va_start(va, fmt);
  char buf[256];
  vsnprintf(buf, 256, fmt, va);
  va_end(va);
  LBasicBlock bb = output().appendBasicBlock(buf);
  return bb;
}

void AnonImpl::SetCurrentBlockContinuation(LBasicBlock continuation) {
  current_bb_impl()->continuation = continuation;
}

ContinuationResolver::ContinuationResolver(AnonImpl& impl, int ssa_id)
    : impl_(impl), ssa_id_(ssa_id) {}

DiamondContinuationResolver::DiamondContinuationResolver(AnonImpl& _impl,
                                                         int ssa_id)
    : ContinuationResolver(_impl, ssa_id), hint_(0), building_block_index_(-1) {
  blocks_[0] = impl().NewBasicBlock("left_for_%d", ssa_id);
  blocks_[1] = impl().NewBasicBlock("right_for_%d", ssa_id);
  blocks_[2] = impl().NewBasicBlock("continuation_for_%d", ssa_id);
}

DiamondContinuationResolver& DiamondContinuationResolver::BuildCmp(
    std::function<LValue()> f) {
  LValue cmp_value = f();
  if (hint_ < 0) {
    cmp_value = impl().ExpectTrue(cmp_value);
  } else if (hint_ > 0) {
    cmp_value = impl().ExpectFalse(cmp_value);
  }
  output().buildCondBr(cmp_value, blocks_[0], blocks_[1]);
  return *this;
}

DiamondContinuationResolver& DiamondContinuationResolver::BuildLeft(
    std::function<LValue()> f) {
  output().positionToBBEnd(blocks_[0]);
  building_block_index_ = 0;
  values_[0] = f();
  output().buildBr(blocks_[2]);
  return *this;
}

DiamondContinuationResolver& DiamondContinuationResolver::BuildRight(
    std::function<LValue()> f) {
  output().positionToBBEnd(blocks_[1]);
  building_block_index_ = 1;
  values_[1] = f();
  output().buildBr(blocks_[2]);
  return *this;
}

LValue DiamondContinuationResolver::End() {
  output().positionToBBEnd(blocks_[2]);
  LValue phi = output().buildPhi(typeOf(values_[0]));
  addIncoming(phi, values_, blocks_, 2);
  impl().SetCurrentBlockContinuation(blocks_[2]);
  return phi;
}

void DiamondContinuationResolver::BranchToOtherLeafOnCondition(LValue cond) {
  EMASSERT(building_block_index_ != -1);
  int other_leaf_index = building_block_index_ ^ 1;
  LBasicBlock diamond_continuation =
      impl().NewBasicBlock("diamond_continuation");

  cond = output().buildCall(output().repo().expectIntrinsic(), cond,
                            output().repo().booleanFalse);
  output().buildCondBr(cond, blocks_[other_leaf_index], diamond_continuation);
  output().positionToBBEnd(diamond_continuation);
  blocks_[building_block_index_] = diamond_continuation;
}

CallResolver::CallResolverParameter::CallResolverParameter(
    Instruction* instr,
    size_t _instruction_size,
    std::unique_ptr<CallSiteInfo> _callinfo)
    : call_instruction(instr),
      instruction_size(_instruction_size),
      callsite_info(std::move(_callinfo)),
      second_return(false) {}

void CallResolver::CallResolverParameter::set_second_return(bool s) {
  second_return = s;
}

CallResolver::CallResolver(
    AnonImpl& impl,
    int ssa_id,
    CallResolver::CallResolverParameter& call_resolver_parameter)
    : ContinuationResolver(impl, ssa_id),
      call_resolver_parameter_(call_resolver_parameter),
      parameters_(kV8CCRegisterParameterCount,
                  LLVMGetUndef(output().tagged_type())) {
  // init parameters_.
  parameters_[static_cast<int>(PP)] = output().pp();
  parameters_[static_cast<int>(THR)] = LLVMGetUndef(output().repo().ref8);
  parameters_[static_cast<int>(FP)] = LLVMGetUndef(output().repo().ref8);
}

void CallResolver::SetGParameter(int reg, LValue val) {
  EMASSERT(reg < kV8CCRegisterParameterCount);
  EMASSERT(reg >= 0);
  parameters_[reg] = val;
}

void CallResolver::AddStackParameter(LValue v) {
  EMASSERT(typeOf(v) == output().tagged_type());
  parameters_.emplace_back(v);
  EMASSERT(parameters_.size() - kV8CCRegisterParameterCount <=
           call_resolver_parameter_.callsite_info->stack_parameter_count());
}

LValue CallResolver::GetStackParameter(size_t i) {
  i += kV8CCRegisterParameterCount;
  EMASSERT(parameters_.size() > i);
  return parameters_[i];
}

LValue CallResolver::BuildCall() {
  EMASSERT(call_resolver_parameter_.callsite_info->type() !=
           CallSiteInfo::CallTargetType::kUnspecify);
  EMASSERT(parameters_.size() - kV8CCRegisterParameterCount ==
           call_resolver_parameter_.callsite_info->stack_parameter_count());
  EMASSERT(call_resolver_parameter_.callsite_info->type() !=
               CallSiteInfo::CallTargetType::kCodeObject ||
           !LLVMIsUndef(parameters_[static_cast<int>(CODE_REG)]));
  ExtractCallInfo();
  GenerateStatePointFunction();
  EmitCall();
  EmitExceptionBlockIfNeeded();

  // reset to continuation.
  if (need_invoke()) {
    output().positionToBBEnd(continuation_block_);
    impl().SetCurrentBlockContinuation(continuation_block_);
  }
  EmitRelocatesIfNeeded();
  EmitPatchPoint();
  return call_value_;
}

void CallResolver::ExtractCallInfo() {
  call_live_out_ =
      impl().liveness().GetCallOutAt(call_resolver_parameter_.call_instruction);
  if (impl().current_bb()->try_index() != kInvalidTryIndex) {
    catch_block_ = impl().flow_graph()->graph_entry()->GetCatchEntry(
        impl().current_bb()->try_index());
  }
  if (call_resolver_parameter_.call_instruction->IsTailCall()) {
    tail_call_ = true;
  }
}

void CallResolver::GenerateStatePointFunction() {
  return_type_ = call_resolver_parameter_.second_return
                     ? output().tagged_pair()
                     : output().tagged_type();
  std::vector<LType> operand_value_types;
  for (LValue param : parameters_) {
    operand_value_types.emplace_back(typeOf(param));
  }
  LType callee_function_type =
      functionType(return_type_, operand_value_types.data(),
                   operand_value_types.size(), NotVariadic);
  callee_type_ = pointerType(callee_function_type);

  statepoint_function_ = output().getStatePointFunction(callee_type_);
}

void CallResolver::EmitCall() {
  std::vector<LValue> statepoint_operands;
  patchid_ = impl().NextPatchPoint();
  statepoint_operands.push_back(output().constInt64(patchid_));
  statepoint_operands.push_back(
      output().constInt32(call_resolver_parameter_.instruction_size));
  statepoint_operands.push_back(constNull(callee_type_));
  statepoint_operands.push_back(
      output().constInt32(parameters_.size()));           // # call params
  statepoint_operands.push_back(output().constInt32(0));  // flags
  for (LValue parameter : parameters_)
    statepoint_operands.push_back(parameter);
  statepoint_operands.push_back(output().constInt32(0));  // # transition args
  statepoint_operands.push_back(output().constInt32(0));  // # deopt arguments
  // normal block
  if (!tail_call_) {
    for (BitVector::Iterator it(call_live_out_); !it.Done(); it.Advance()) {
      int live_ssa_index = it.Current();
      if (impl().IsLazyValue(live_ssa_index)) continue;
      ValueDesc desc = impl().current_bb_impl()->GetValue(live_ssa_index);
      if (desc.type != ValueType::LLVMValue) continue;
      gc_desc_list_.emplace_back(live_ssa_index, statepoint_operands.size());
      statepoint_operands.emplace_back(desc.llvm_value);
    }
  }

  if (need_invoke()) {
    for (BitVector::Iterator it(impl().liveness().GetLiveInSet(catch_block_));
         !it.Done(); it.Advance()) {
      int live_ssa_index = it.Current();
      ValueDesc desc = impl().current_bb_impl()->GetValue(live_ssa_index);
      exception_block_live_in_value_map_.emplace(live_ssa_index, desc);
    }
  }

  if (!need_invoke()) {
    statepoint_value_ =
        output().buildCall(statepoint_function_, statepoint_operands.data(),
                           statepoint_operands.size());
    if (tail_call_) output().buildReturnForTailCall();
  } else {
    catch_block_impl_ = impl().GetCatchBlockImplInitIfNeeded(catch_block_);
    continuation_block_ =
        impl().NewBasicBlock("continuation_block_for_%d", ssa_id());
    statepoint_value_ =
        output().buildInvoke(statepoint_function_, statepoint_operands.data(),
                             statepoint_operands.size(), continuation_block_,
                             catch_block_impl_->native_bb);
  }
  LLVMSetInstructionCallConv(statepoint_value_, LLVMV8CallConv);
}

void CallResolver::EmitRelocatesIfNeeded() {
  if (tail_call_) return;
  for (auto& gc_relocate : gc_desc_list_) {
    LValue relocated = output().buildCall(
        output().repo().gcRelocateIntrinsic(), statepoint_value_,
        output().constInt32(gc_relocate.where),
        output().constInt32(gc_relocate.where));
    impl().current_bb_impl()->SetLLVMValue(gc_relocate.ssa_index, relocated);
  }

  LValue intrinsic = output().getGCResultFunction(return_type_);
  call_value_ = output().buildCall(intrinsic, statepoint_value_);
}

void CallResolver::EmitPatchPoint() {
  std::unique_ptr<CallSiteInfo> callsite_info(
      std::move(call_resolver_parameter_.callsite_info));
  callsite_info->set_is_tailcall(tail_call_);
  callsite_info->set_patchid(patchid_);
  callsite_info->set_try_index(impl().current_bb()->try_index());
  impl().SubmitStackMap(std::move(callsite_info));
}

void CallResolver::EmitExceptionBlockIfNeeded() {
  if (!need_invoke()) return;
  if (!impl().IsBBStartedToTranslate(catch_block_)) {
    catch_block_impl_->exception_live_in_entries.emplace_back(
        std::move(exception_block_live_in_value_map_),
        impl().current_bb_impl()->continuation);
  } else {
    ExceptionBlockLiveInEntry e = {
        std::move(exception_block_live_in_value_map_),
        impl().current_bb_impl()->continuation};
    impl().MergeExceptionLiveInEntries(e, catch_block_, catch_block_impl_);
  }
}

BoxAllocationSlowPath::BoxAllocationSlowPath(Instruction* instruction,
                                             const Class& cls,
                                             AnonImpl& impl)
    : instruction_(instruction), cls_(cls), impl_(impl) {}

LValue BoxAllocationSlowPath::Allocate() {
  const intptr_t instance_size = compiler::target::Class::GetInstanceSize(cls_);
  const classid_t cid = compiler::target::Class::GetId(cls_);
  AssemblerResolver resolver(impl());
  Label slow_path("BoxAllocationSlowPath:slow_path");
  LValue instance = impl().TryAllocate(resolver, slow_path, cid, instance_size);
  if (instance) resolver.GotoMergeWithValue(instance);
  resolver.Bind(slow_path);
  resolver.GotoMergeWithValue(BuildSlowPath());
  return resolver.End();
}

LValue BoxAllocationSlowPath::BuildSlowPath() {
  const Code& stub = Code::ZoneHandle(
      impl().zone(), StubCode::GetAllocationStubForClass(cls_));
  return impl().GenerateCall(instruction_, instruction_->token_pos(), -1, stub,
                             RawPcDescriptors::kOther, 0, {});
}

ComparisonResolver::ComparisonResolver(AnonImpl& impl)
    : FlowGraphVisitor(impl.flow_graph()->reverse_postorder()),
      impl_(impl),
      result_(nullptr) {}

void ComparisonResolver::VisitStrictCompare(StrictCompareInstr* instr) {
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));
  if (instr->needs_number_check()) {
    Label reference_compare("reference_compare_%d", instr->ssa_temp_index()),
        check_mint("check_mint_%d", instr->ssa_temp_index());
    AssemblerResolver resolver(impl());
    LValue left_is_smi = impl().TstSmi(left);
    resolver.BranchIf(left_is_smi, reference_compare);
    LValue right_is_smi = impl().TstSmi(left);
    resolver.BranchIf(right_is_smi, reference_compare);
    LValue left_is_double = impl().CompareClassId(left, kDoubleCid);
    resolver.BranchIfNot(left_is_double, check_mint);
    LValue right_is_double = impl().CompareClassId(right, kDoubleCid);
    resolver.GotoMergeWithValueIfNot(right_is_double,
                                     output().repo().booleanFalse);
    LValue left_double_bytes = impl().LoadFieldFromOffset(
        left, compiler::target::Double::value_offset(), output().repo().int64);
    LValue right_double_bytes = impl().LoadFieldFromOffset(
        right, compiler::target::Double::value_offset(), output().repo().int64);
    resolver.GotoMergeWithValue(
        output().buildICmp(LLVMIntEQ, left_double_bytes, right_double_bytes));

    resolver.Bind(check_mint);
    LValue left_is_mint = impl().CompareClassId(left, kMintCid);
    resolver.BranchIfNot(left_is_mint, reference_compare);
    LValue right_is_mint = impl().CompareClassId(right, kMintCid);
    resolver.GotoMergeWithValueIfNot(right_is_mint,
                                     output().repo().booleanFalse);
    LValue left_mint_val = impl().LoadFieldFromOffset(
        left, compiler::target::Mint::value_offset(), output().repo().int64);
    LValue right_mint_val = impl().LoadFieldFromOffset(
        right, compiler::target::Mint::value_offset(), output().repo().int64);
    resolver.GotoMergeWithValue(
        output().buildICmp(LLVMIntEQ, left_mint_val, right_mint_val));

    resolver.Bind(reference_compare);
    LValue left_word = impl().TaggedToWord(left);
    LValue right_word = impl().TaggedToWord(right);
    resolver.GotoMergeWithValue(
        output().buildICmp(LLVMIntEQ, left_word, right_word));

    result_ = resolver.End();
  } else {
    result_ = output().buildICmp(
        instr->kind() == Token::kEQ_STRICT ? LLVMIntEQ : LLVMIntNE, left,
        right);
  }
}

static LLVMIntPredicate TokenKindToSmiCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ:
      return LLVMIntEQ;
    case Token::kNE:
      return LLVMIntNE;
    case Token::kLT:
      return LLVMIntSLT;
    case Token::kGT:
      return LLVMIntSGT;
    case Token::kLTE:
      return LLVMIntSLE;
    case Token::kGTE:
      return LLVMIntSGE;
    default:
      UNREACHABLE();
  }
}

static LLVMRealPredicate TokenKindToDoubleCondition(Token::Kind kind) {
  switch (kind) {
    case Token::kEQ:
      return LLVMRealOEQ;
    case Token::kNE:
      return LLVMRealONE;
    case Token::kLT:
      return LLVMRealOLT;
    case Token::kGT:
      return LLVMRealOGT;
    case Token::kLTE:
      return LLVMRealOLE;
    case Token::kGTE:
      return LLVMRealOGE;
    default:
      UNREACHABLE();
  }
}

void ComparisonResolver::VisitEqualityCompare(EqualityCompareInstr* instr) {
  LValue cmp_value;
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));
  if (instr->operation_cid() == kSmiCid) {
    EMASSERT(typeOf(left) == output().tagged_type());
    EMASSERT(typeOf(right) == output().tagged_type());
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()),
                           impl().SmiUntag(left), impl().SmiUntag(right));
  } else if (instr->operation_cid() == kMintCid) {
    EMASSERT(typeOf(left) == output().repo().intPtr);
    EMASSERT(typeOf(right) == output().repo().intPtr);
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()), left, right);
  } else {
    EMASSERT(instr->operation_cid() == kDoubleCid);
    EMASSERT(typeOf(left) == output().repo().doubleType);
    EMASSERT(typeOf(right) == output().repo().doubleType);
    cmp_value = output().buildFCmp(TokenKindToDoubleCondition(instr->kind()),
                                   left, right);
  }
  result_ = cmp_value;
}

void ComparisonResolver::VisitCheckedSmiComparison(
    CheckedSmiComparisonInstr* instr) {
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));

  intptr_t left_cid = instr->left()->Type()->ToCid();
  intptr_t right_cid = instr->right()->Type()->ToCid();
  DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
  diamond.LeftHint();
  diamond.BuildCmp([&] {
    LValue is_smi;
    if (instr->left()->definition() == instr->right()->definition()) {
      is_smi = impl().TstSmi(left);
    } else if (left_cid == kSmiCid) {
      is_smi = impl().TstSmi(right);
    } else if (right_cid == kSmiCid) {
      is_smi = impl().TstSmi(left);
    } else {
      LValue left_address = impl().TaggedToWord(left);
      LValue right_address = impl().TaggedToWord(right);
      LValue orr_address = output().buildOr(left_address, right_address);
      LValue and_value =
          output().buildAnd(orr_address, output().constIntPtr(kSmiTagMask));
      is_smi = output().buildICmp(LLVMIntNE, and_value,
                                  output().constIntPtr(kSmiTagMask));
    }
    return is_smi;
  });
  diamond.BuildLeft([&] {
    // both are smi
    return output().buildICmp(TokenKindToSmiCondition(instr->kind()),
                              impl().SmiUntag(left), impl().SmiUntag(right));
  });
  diamond.BuildRight([&] {
    impl().PushArgument(left);
    impl().PushArgument(right);
    const auto& selector = String::Handle(instr->call()->Selector());
    const auto& arguments_descriptor = Array::Handle(
        ArgumentsDescriptor::New(/*type_args_len=*/0, /*num_arguments=*/2));
    LValue boolean_object = impl().GenerateMegamorphicInstanceCall(
        instr, selector, arguments_descriptor, instr->call()->deopt_id(),
        instr->token_pos());
    LValue true_object = impl().LoadObject(Bool::True());
    return output().buildICmp(LLVMIntEQ, boolean_object, true_object);
  });
  result_ = diamond.End();
}

void ComparisonResolver::VisitCaseInsensitiveCompare(
    CaseInsensitiveCompareInstr* instr) {
  UNREACHABLE();
}

Label::Label(const char* fmt, ...) : bb_(nullptr) {
  va_list va;
  va_start(va, fmt);
  char buf[256];
  vsnprintf(buf, 256, fmt, va);
  va_end(va);
  name_.assign(buf);
}

void Label::InitBBIfNeeded(AnonImpl& impl) {
  if (bb_) return;
  bb_ = impl.NewBasicBlock(name_.c_str());
}

AssemblerResolver::AssemblerResolver(AnonImpl& impl)
    : impl_(impl), current_(impl.output().getInsertionBlock()) {
  merge_ = impl.NewBasicBlock("AssemblerResolver::Merge");
  values_modified_ = new (impl.zone())
      BitVector(impl.zone(), impl.flow_graph()->max_virtual_register_number());
  old_value_interceptor_ = impl.value_interceptor_;
  impl.value_interceptor_ = this;
}

AssemblerResolver::~AssemblerResolver() {
  EMASSERT(impl().value_interceptor_ != this);
}

void AssemblerResolver::Branch(Label& label) {
  label.InitBBIfNeeded(impl());
  output().buildBr(label.basic_block());
  AssignBlockInitialValues(label.basic_block());
  current_ = nullptr;
}

void AssemblerResolver::BranchIf(LValue cond, Label& label) {
  EMASSERT(current_);
  LBasicBlock continuation =
      impl().NewBasicBlock("AssemblerResolver::BranchIf");
  label.InitBBIfNeeded(impl());
  output().buildCondBr(cond, label.basic_block(), continuation);
  AssignBlockInitialValues(label.basic_block());
  current_ = continuation;
  output().positionToBBEnd(current_);
}

void AssemblerResolver::BranchIfNot(LValue cond, Label& label) {
  EMASSERT(current_);
  LBasicBlock continuation =
      impl().NewBasicBlock("AssemblerResolver::BranchIfNot");
  label.InitBBIfNeeded(impl());
  output().buildCondBr(cond, continuation, label.basic_block());
  AssignBlockInitialValues(label.basic_block());
  current_ = continuation;
  output().positionToBBEnd(current_);
}

void AssemblerResolver::Bind(Label& label) {
  label.InitBBIfNeeded(impl());
  current_ = label.basic_block();
  output().positionToBBEnd(current_);
  auto found_initial_vals = initial_modified_values_lookup_.find(current_);
  EMASSERT(found_initial_vals != initial_modified_values_lookup_.end());
  current_modified_ = std::move(found_initial_vals->second);
}

LValue AssemblerResolver::End() {
  output().positionToBBEnd(merge_);
  impl().SetCurrentBlockContinuation(merge_);
  if (merge_values_.empty()) return nullptr;
  EMASSERT(merge_values_.size() == merge_blocks_.size());
  LType type = typeOf(merge_values_.front());
  LValue phi = output().buildPhi(type);
  for (LValue v : merge_values_) {
    EMASSERT(type == typeOf(v));
  }
  addIncoming(phi, merge_values_.data(), merge_blocks_.data(),
              merge_values_.size());
  // reset old
  impl().value_interceptor_ = old_value_interceptor_;

  MergeModified();
  return phi;
}

void AssemblerResolver::GotoMerge() {
  merge_blocks_.emplace_back(current_);
  output().buildBr(merge_);
  AssignModifiedValueToMerge();
  current_ = nullptr;
}

void AssemblerResolver::GotoMergeIf(LValue cond) {
  merge_blocks_.emplace_back(current_);
  LBasicBlock continuation =
      impl().NewBasicBlock("AssemblerResolver::GotoMergeIf");
  output().buildCondBr(cond, merge_, continuation);
  AssignModifiedValueToMerge();
  current_ = continuation;
  output().positionToBBEnd(current_);
}

void AssemblerResolver::GotoMergeIfNot(LValue cond) {
  merge_blocks_.emplace_back(current_);
  LBasicBlock continuation =
      impl().NewBasicBlock("AssemblerResolver::GotoMergeIf");
  output().buildCondBr(cond, continuation, merge_);
  AssignModifiedValueToMerge();
  current_ = continuation;
  output().positionToBBEnd(current_);
}

void AssemblerResolver::GotoMergeWithValue(LValue v) {
  EMASSERT(current_);
  merge_values_.emplace_back(v);
  GotoMerge();
}

void AssemblerResolver::GotoMergeWithValueIf(LValue cond, LValue v) {
  EMASSERT(current_);
  LBasicBlock continuation =
      impl().NewBasicBlock("AssemblerResolver::GotoMergeWithValueIf");
  merge_values_.emplace_back(v);
  merge_blocks_.emplace_back(current_);
  output().buildCondBr(cond, merge_, continuation);
  AssignModifiedValueToMerge();
  current_ = continuation;
  output().positionToBBEnd(current_);
}

void AssemblerResolver::GotoMergeWithValueIfNot(LValue cond, LValue v) {
  EMASSERT(current_);
  LBasicBlock continuation =
      impl().NewBasicBlock("AssemblerResolver::GotoMergeWithValueIfNot");
  merge_values_.emplace_back(v);
  merge_blocks_.emplace_back(current_);
  output().buildCondBr(cond, continuation, merge_);
  AssignModifiedValueToMerge();
  current_ = continuation;
  output().positionToBBEnd(current_);
}

LValue AssemblerResolver::GetLLVMValue(int ssa_id) {
  auto found = current_modified_.find(ssa_id);
  if (found != current_modified_.end()) return found->second;
  return nullptr;
}

bool AssemblerResolver::SetLLVMValue(int ssa_id, LValue v) {
  current_modified_[ssa_id] = v;
  values_modified_->Add(ssa_id);
  return true;
}

void AssemblerResolver::AssignBlockInitialValues(LBasicBlock bb) {
  initial_modified_values_lookup_.emplace(bb, current_modified_);
}

void AssemblerResolver::AssignModifiedValueToMerge() {
  modified_values_to_merge_.emplace(current_, current_modified_);
}

void AssemblerResolver::MergeModified() {
  for (BitVector::Iterator it(values_modified_); !it.Done(); it.Advance()) {
    int need_to_merge = it.Current();
    LValue original = impl().GetLLVMValue(need_to_merge);
    LType type = typeOf(original);
    LValue phi = output().buildPhi(type);
    for (LBasicBlock b : merge_blocks_) {
      auto found_block = modified_values_to_merge_.find(b);
      EMASSERT(found_block != modified_values_to_merge_.end());
      auto& value_lookup = found_block->second;
      auto found = value_lookup.find(need_to_merge);
      if (found == value_lookup.end()) {
        addIncoming(phi, &original, &b, 1);
      } else {
        addIncoming(phi, &found->second, &b, 1);
      }
    }
  }
}
}  // namespace

struct IRTranslator::Impl : public AnonImpl {};

IRTranslator::IRTranslator(FlowGraph* flow_graph, Precompiler* precompiler)
    : FlowGraphVisitor(flow_graph->reverse_postorder()) {
  // dynamic function has multiple entry points
  // which LLVM does not suppport.
  InitLLVM();
  impl_.reset(new Impl);
  impl().flow_graph_ = flow_graph;
  impl().precompiler_ = precompiler;
  impl().thread_ = Thread::Current();
  impl().compiler_state_.reset(new CompilerState(
      String::Handle(flow_graph->zone(),
                     flow_graph->parsed_function().function().UserVisibleName())
          .ToCString()));
  impl().liveness_analysis_.reset(new LivenessAnalysis(flow_graph));
  impl().output_.reset(new Output(impl().compiler_state()));
  // init parameter desc
  RegisterParameterDesc parameter_desc;
  LType tagged_type = output().repo().tagged_type;
  for (int i = flow_graph->num_direct_parameters(); i > 0; --i) {
    parameter_desc.emplace_back(-i, tagged_type);
  }
  // init output().
  output().initializeBuild(parameter_desc);
}

IRTranslator::~IRTranslator() {}

void IRTranslator::Translate() {
#if 1
  impl().liveness().Analyze();
  VisitBlocks();
  impl().End();
#endif

  // FlowGraphPrinter::PrintGraph("IRTranslator", impl().flow_graph_);
}

Output& IRTranslator::output() {
  return impl().output();
}

void IRTranslator::VisitBlockEntry(BlockEntryInstr* block) {
  impl().SetDebugLine(block);
  impl().StartTranslate(block);
  IRTranslatorBlockImpl* block_impl = impl().current_bb_impl();
  block_impl->try_index = block->try_index();
  if (!block_impl->exception_block)
    impl().MergePredecessors(block);
  else
    impl().MergeExceptionVirtualPredecessors(block);
}

void IRTranslator::VisitBlockEntryWithInitialDefs(
    BlockEntryWithInitialDefs* block) {
  VisitBlockEntry(block);
  for (Definition* def : *block->initial_definitions()) {
    def->Accept(this);
  }
}

void IRTranslator::VisitGraphEntry(GraphEntryInstr* instr) {
  VisitBlockEntryWithInitialDefs(instr);
}

void IRTranslator::VisitJoinEntry(JoinEntryInstr* instr) {
  VisitBlockEntry(instr);
  for (PhiInstr* phi : *instr->phis()) {
    VisitPhi(phi);
  }
}

void IRTranslator::VisitTargetEntry(TargetEntryInstr* instr) {
  VisitBlockEntry(instr);
}

void IRTranslator::VisitFunctionEntry(FunctionEntryInstr* instr) {
  EMASSERT(!impl().visited_function_entry_);
  impl().visited_function_entry_ = true;
  LBasicBlock bb = impl().EnsureNativeBB(instr);
  output().buildBr(bb);
  VisitBlockEntryWithInitialDefs(instr);
}

void IRTranslator::VisitNativeEntry(NativeEntryInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitOsrEntry(OsrEntryInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitIndirectEntry(IndirectEntryInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCatchBlockEntry(CatchBlockEntryInstr* instr) {
  IRTranslatorBlockImpl* block_impl =
      impl().GetCatchBlockImplInitIfNeeded(instr);
  output().positionToBBEnd(block_impl->native_bb);
  VisitBlockEntryWithInitialDefs(instr);
}

void IRTranslator::VisitPhi(PhiInstr* instr) {
  impl().SetDebugLine(instr);
  LType phi_type = impl().GetMachineRepresentationType(instr->representation());

  LValue phi = output().buildPhi(phi_type);
  bool should_add_to_tf_phi_worklist = false;
  intptr_t predecessor_count = impl().current_bb_->PredecessorCount();
  IRTranslatorBlockImpl* block_impl = impl().current_bb_impl();

  for (intptr_t i = 0; i < predecessor_count; ++i) {
    BlockEntryInstr* pred = impl().current_bb()->PredecessorAt(i);
    int ssa_value = instr->InputAt(i)->definition()->ssa_temp_index();
    if (impl().IsBBStartedToTranslate(pred)) {
      LValue value =
          impl().EnsurePhiInputAndPosition(pred, ssa_value, phi_type);
      LBasicBlock llvmbb = impl().GetNativeBBContinuation(pred);
      addIncoming(phi, &value, &llvmbb, 1);
    } else {
      should_add_to_tf_phi_worklist = true;
      block_impl->not_merged_phis.emplace_back();
      auto& not_merged_phi = block_impl->not_merged_phis.back();
      not_merged_phi.phi = phi;
      not_merged_phi.pred = pred;
      not_merged_phi.value = ssa_value;
    }
  }
  if (should_add_to_tf_phi_worklist)
    impl().phi_rebuild_worklist_.push_back(impl().current_bb_);
  block_impl->SetLLVMValue(instr->ssa_temp_index(), phi);
}

void IRTranslator::VisitRedefinition(RedefinitionInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitParameter(ParameterInstr* instr) {
  int param_index = instr->index();
  // only stack parameters
  LValue result = output().parameter(param_index);
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitNativeParameter(NativeParameterInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitLoadIndexedUnsafe(LoadIndexedUnsafeInstr* instr) {
  EMASSERT(instr->RequiredInputRepresentation(0) == kTagged);  // It is a Smi.
  impl().SetDebugLine(instr);
  LValue index_smi = impl().GetLLVMValue(instr->index());
  EMASSERT(instr->base_reg() == FP);
  LValue offset = output().buildShl(index_smi, output().constInt32(1));
  offset = output().buildAdd(offset, output().constIntPtr(instr->offset()));
  LValue access =
      impl().BuildAccessPointer(output().fp(), offset, instr->representation());
  LValue value = output().buildLoad(access);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitStoreIndexedUnsafe(StoreIndexedUnsafeInstr* instr) {
  ASSERT(instr->RequiredInputRepresentation(
             StoreIndexedUnsafeInstr::kIndexPos) == kTagged);  // It is a Smi.
  impl().SetDebugLine(instr);
  LValue index_smi = impl().GetLLVMValue(instr->index());
  EMASSERT(instr->base_reg() == FP);
  LValue offset = output().buildShl(index_smi, output().constInt32(1));
  offset = output().buildAdd(offset, output().constIntPtr(instr->offset()));
  LValue gep =
      impl().BuildAccessPointer(output().fp(), offset, instr->representation());

  LValue value = impl().GetLLVMValue(instr->value());
  output().buildStore(value, gep);
}

void IRTranslator::VisitTailCall(TailCallInstr* instr) {
  impl().SetDebugLine(instr);
  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  callsite_info->set_type(CallSiteInfo::CallTargetType::kCodeObject);
  CallResolver::CallResolverParameter param(instr, Instr::kInstrSize,
                                            std::move(callsite_info));
  CallResolver resolver(impl(), -1, param);
  LValue code_object = impl().LoadObject(instr->code());
  resolver.SetGParameter(static_cast<int>(CODE_REG), code_object);
  // add register parameter.
  EMASSERT(impl().GetLLVMValue(instr->InputAt(0)) == output().args_desc());
  resolver.SetGParameter(static_cast<int>(ARGS_DESC_REG), output().args_desc());
  resolver.BuildCall();
}

void IRTranslator::VisitParallelMove(ParallelMoveInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitPushArgument(PushArgumentInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  impl().PushArgument(value);
}

void IRTranslator::VisitReturn(ReturnInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  output().buildRet(value);
}

void IRTranslator::VisitNativeReturn(NativeReturnInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitThrow(ThrowInstr* instr) {
  impl().SetDebugLine(instr);
  impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                             kThrowRuntimeEntry, 1, false);
  output().buildCall(output().repo().trapIntrinsic());
}

void IRTranslator::VisitReThrow(ReThrowInstr* instr) {
  impl().SetDebugLine(instr);
  impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                             kReThrowRuntimeEntry, 2, false);
  output().buildCall(output().repo().trapIntrinsic());
}

void IRTranslator::VisitStop(StopInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGoto(GotoInstr* instr) {
  impl().SetDebugLine(instr);
  JoinEntryInstr* successor = instr->successor();
  LBasicBlock bb = impl().EnsureNativeBB(successor);
  output().buildBr(bb);
  impl().EndCurrentBlock();
}

void IRTranslator::VisitIndirectGoto(IndirectGotoInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBranch(BranchInstr* instr) {
  impl().SetDebugLine(instr);
  TargetEntryInstr* true_successor = instr->true_successor();
  TargetEntryInstr* false_successor = instr->false_successor();
  impl().EnsureNativeBB(true_successor);
  impl().EnsureNativeBB(false_successor);
  ComparisonResolver resolver(impl());
  instr->comparison()->Accept(&resolver);
  LValue cmp_val = resolver.result();
  cmp_val = impl().EnsureBoolean(cmp_val);
  output().buildCondBr(cmp_val, impl().GetNativeBB(true_successor),
                       impl().GetNativeBB(false_successor));
  impl().EndCurrentBlock();
}

// FIXME: implement assertions.
void IRTranslator::VisitAssertAssignable(AssertAssignableInstr* instr) {}

void IRTranslator::VisitAssertSubtype(AssertSubtypeInstr* instr) {}

void IRTranslator::VisitAssertBoolean(AssertBooleanInstr* instr) {}

void IRTranslator::VisitSpecialParameter(SpecialParameterInstr* instr) {
  LValue val = nullptr;
  auto kind = instr->kind();
  if (kind == SpecialParameterInstr::kArgDescriptor) {
    val = output().args_desc();
  } else {
    EMASSERT(impl().current_bb_impl()->exception_block);
    switch (kind) {
      case SpecialParameterInstr::kException:
        val = impl().current_bb_impl()->exception_val;
        break;
      case SpecialParameterInstr::kStackTrace:
        val = impl().current_bb_impl()->stacktrace_val;
        break;
      default:
        UNREACHABLE();
    }
  }
  impl().SetLLVMValue(instr, val);
}

void IRTranslator::VisitClosureCall(ClosureCallInstr* instr) {
  impl().SetDebugLine(instr);
  const intptr_t argument_count =
      instr->ArgumentCount();  // Includes type args.
  LValue function = impl().GetLLVMValue(instr->InputAt(0));
  LValue code_object = impl().LoadFieldFromOffset(
      function, compiler::target::Function::code_offset());

  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  callsite_info->set_type(CallSiteInfo::CallTargetType::kCodeObject);
  callsite_info->set_token_pos(instr->token_pos());
  callsite_info->set_deopt_id(instr->deopt_id());
  callsite_info->set_stack_parameter_count(argument_count);
  CallResolver::CallResolverParameter param(instr, Instr::kInstrSize,
                                            std::move(callsite_info));
  CallResolver resolver(impl(), instr->ssa_temp_index(), param);
  const Array& arguments_descriptor =
      Array::ZoneHandle(impl().zone(), instr->GetArgumentsDescriptor());
  LValue argument_descriptor_obj = impl().LoadObject(arguments_descriptor);
  resolver.SetGParameter(ARGS_DESC_REG, argument_descriptor_obj);
  resolver.SetGParameter(CODE_REG, code_object);
  resolver.SetGParameter(kICReg, output().constTagged(0));
  for (intptr_t i = argument_count - 1; i >= 0; --i) {
    LValue param = impl().GetLLVMValue(instr->PushArgumentAt(i)->value());
    resolver.AddStackParameter(param);
  }
  LValue result = resolver.BuildCall();
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitFfiCall(FfiCallInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitInstanceCall(InstanceCallInstr* instr) {
  impl().SetDebugLine(instr);
  EMASSERT(instr->ic_data() != NULL);
  EMASSERT((FLAG_precompiled_mode && FLAG_use_bare_instructions));
  Zone* zone = impl().zone();
  ICData& ic_data = ICData::ZoneHandle(zone, instr->ic_data()->raw());
  ic_data = ic_data.AsUnaryClassChecks();

  const intptr_t argument_count =
      instr->ArgumentCount();  // Includes type args.

  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  // use kReg type.
  callsite_info->set_type(CallSiteInfo::CallTargetType::kReg);
  callsite_info->set_reg(kInstanceCallTargetReg);
  callsite_info->set_token_pos(instr->token_pos());
  callsite_info->set_deopt_id(instr->deopt_id());
  callsite_info->set_stack_parameter_count(argument_count);
  CallResolver::CallResolverParameter param(instr, Instr::kInstrSize,
                                            std::move(callsite_info));
  CallResolver resolver(impl(), instr->ssa_temp_index(), param);

  const Code& initial_stub = StubCode::UnlinkedCall();
  const UnlinkedCall& data =
      UnlinkedCall::ZoneHandle(zone, ic_data.AsUnlinkedCall());
  LValue data_val = impl().LoadObject(data, true);
  LValue initial_stub_val = impl().LoadObject(initial_stub, true);
  resolver.SetGParameter(kICReg, data_val);
  resolver.SetGParameter(kInstanceCallTargetReg, initial_stub_val);
  for (intptr_t i = argument_count - 1; i >= 0; --i) {
    LValue param = impl().GetLLVMValue(instr->PushArgumentAt(i)->value());
    resolver.AddStackParameter(param);
  }
  LValue receiver =
      resolver.GetStackParameter((ic_data.CountWithoutTypeArgs() - 1));
  resolver.SetGParameter(kReceiverReg, receiver);

  LValue result = resolver.BuildCall();
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitPolymorphicInstanceCall(
    PolymorphicInstanceCallInstr* instr) {
  impl().SetDebugLine(instr);
#if 0
  bool complete = instr->complete();
  if (complete) {
  ArgumentsInfo args_info(instr->instance_call()->type_args_len(),
                          instr->instance_call()->ArgumentCount(),
                          instr->instance_call()->argument_names());
  const Array& arguments_descriptor =
      Array::ZoneHandle(impl().flow_graph()->zone(), args_info.ToArgumentsDescriptor());
  const CallTargets& targets = instr->targets();

  static const int kNoCase = -1;
  int smi_case = kNoCase;
  int which_case_to_skip = kNoCase;

  const int length = targets.length();
  EMASSERT(length > 0);
  int non_smi_length = length;

  // Find out if one of the classes in one of the cases is the Smi class. We
  // will be handling that specially.
  for (int i = 0; i < length; i++) {
    const intptr_t start = targets[i].cid_start;
    if (start > kSmiCid) continue;
    const intptr_t end = targets[i].cid_end;
    if (end >= kSmiCid) {
      smi_case = i;
      if (start == kSmiCid && end == kSmiCid) {
        // If this case has only the Smi class then we won't need to emit it at
        // all later.
        which_case_to_skip = i;
        non_smi_length--;
      }
      break;
    }
  }

  if (smi_case != kNoCase) {
    compiler::Label after_smi_test;
    // If the call is complete and there are no other possible receiver
    // classes - then receiver can only be a smi value and we don't need
    // to check if it is a smi.
    if (!(complete && non_smi_length == 0)) {
      EmitTestAndCallSmiBranch(non_smi_length == 0 ? failed : &after_smi_test,
                               /* jump_if_smi= */ false);
    }

    // Do not use the code from the function, but let the code be patched so
    // that we can record the outgoing edges to other code.
    const Function& function = *targets.TargetAt(smi_case)->target;
    GenerateStaticDartCall(deopt_id, token_index, RawPcDescriptors::kOther,
                           locs, function, entry_kind);
    __ Drop(args_info.count_with_type_args);
    if (match_found != NULL) {
      __ Jump(match_found);
    }
    __ Bind(&after_smi_test);
  } else {
    if (!complete) {
      // Smi is not a valid class.
      EmitTestAndCallSmiBranch(failed, /* jump_if_smi = */ true);
    }
  }

  if (non_smi_length == 0) {
    // If non_smi_length is 0 then only a Smi check was needed; the Smi check
    // above will fail if there was only one check and receiver is not Smi.
    return;
  }

  bool add_megamorphic_call = false;
  int bias = 0;

  // Value is not Smi.
  EmitTestAndCallLoadCid(EmitTestCidRegister());

  int last_check = which_case_to_skip == length - 1 ? length - 2 : length - 1;

  for (intptr_t i = 0; i < length; i++) {
    if (i == which_case_to_skip) continue;
    const bool is_last_check = (i == last_check);
    const int count = targets.TargetAt(i)->count;
    if (!is_last_check && !complete && count < (total_ic_calls >> 5)) {
      // This case is hit too rarely to be worth writing class-id checks inline
      // for.  Note that we can't do this for calls with only one target because
      // the type propagator may have made use of that and expects a deopt if
      // a new class is seen at this calls site.  See IsMonomorphic.
      add_megamorphic_call = true;
      break;
    }
    compiler::Label next_test;
    if (!complete || !is_last_check) {
      bias = EmitTestAndCallCheckCid(assembler(),
                                     is_last_check ? failed : &next_test,
                                     EmitTestCidRegister(), targets[i], bias,
                                     /*jump_on_miss =*/true);
    }
    // Do not use the code from the function, but let the code be patched so
    // that we can record the outgoing edges to other code.
    const Function& function = *targets.TargetAt(i)->target;
    GenerateStaticDartCall(deopt_id, token_index, RawPcDescriptors::kOther,
                           locs, function, entry_kind);
    __ Drop(args_info.count_with_type_args);
    if (!is_last_check || add_megamorphic_call) {
      __ Jump(match_found);
    }
    __ Bind(&next_test);
  }
  if (add_megamorphic_call) {
    int try_index = kInvalidTryIndex;
    EmitMegamorphicInstanceCall(function_name, arguments_descriptor, deopt_id,
                                token_index, locs, try_index);
  }
  } else {
  }
#else
  // FIXME: implement the upper code.
  VisitInstanceCall(instr->instance_call());
#endif
}

void IRTranslator::VisitStaticCall(StaticCallInstr* instr) {
  impl().SetDebugLine(instr);
  Zone* zone = impl().zone();
  ICData& ic_data = ICData::ZoneHandle(zone, instr->ic_data()->raw());
  ArgumentsInfo args_info(instr->type_args_len(), instr->ArgumentCount(),
                          instr->argument_names());
  std::vector<LValue> arguments;
  for (intptr_t i = 0; i < args_info.count_with_type_args; ++i) {
    arguments.emplace_back(
        impl().GetLLVMValue(instr->PushArgumentAt(i)->value()));
  }
  LValue result = impl().GenerateStaticCall(
      instr, instr->ssa_temp_index(), arguments, instr->deopt_id(),
      instr->token_pos(), instr->function(), args_info, ic_data,
      instr->entry_kind());
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitLoadLocal(LoadLocalInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDropTemps(DropTempsInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitMakeTemp(MakeTempInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStoreLocal(StoreLocalInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStrictCompare(StrictCompareInstr* instr) {
  impl().SetDebugLine(instr);
  ComparisonResolver resolver(impl());
  instr->Accept(&resolver);
  LValue result = resolver.result();
  ;
  impl().SetLLVMValue(instr, impl().BooleanToObject(result));
}

void IRTranslator::VisitEqualityCompare(EqualityCompareInstr* instr) {
  impl().SetDebugLine(instr);
  ComparisonResolver resolver(impl());
  instr->Accept(&resolver);
  impl().SetLLVMValue(instr, impl().BooleanToObject(resolver.result()));
}

void IRTranslator::VisitRelationalOp(RelationalOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue cmp_value;
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));
  if (instr->operation_cid() == kSmiCid) {
    EMASSERT(typeOf(left) == output().tagged_type());
    EMASSERT(typeOf(right) == output().tagged_type());
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()),
                           impl().SmiUntag(left), impl().SmiUntag(right));
  } else if (instr->operation_cid() == kMintCid) {
    EMASSERT(typeOf(left) == output().repo().intPtr);
    EMASSERT(typeOf(right) == output().repo().intPtr);
    cmp_value =
        output().buildICmp(TokenKindToSmiCondition(instr->kind()), left, right);
  } else {
    EMASSERT(instr->operation_cid() == kDoubleCid);
    EMASSERT(typeOf(left) == output().repo().doubleType);
    EMASSERT(typeOf(right) == output().repo().doubleType);
    cmp_value = output().buildFCmp(TokenKindToDoubleCondition(instr->kind()),
                                   left, right);
  }
  impl().SetLLVMValue(instr, cmp_value);
}

void IRTranslator::VisitNativeCall(NativeCallInstr* instr) {
  impl().SetDebugLine(instr);
  EMASSERT(static_cast<intptr_t>(impl().pushed_arguments_.size()) >=
           instr->ArgumentCount());
  EMASSERT(instr->link_lazily());
  const intptr_t argument_count =
      instr->ArgumentCount();  // Includes type args.

  std::unique_ptr<CallSiteInfo> callsite_info(new CallSiteInfo);
  callsite_info->set_type(CallSiteInfo::CallTargetType::kNative);
  callsite_info->set_token_pos(instr->token_pos());
  callsite_info->set_deopt_id(instr->deopt_id());
  callsite_info->set_stack_parameter_count(argument_count);
  CallResolver::CallResolverParameter param(instr, kNativeCallInstrSize,
                                            std::move(callsite_info));
  CallResolver resolver(impl(), instr->ssa_temp_index(), param);
  for (intptr_t i = 0; i < argument_count; ++i) {
    LValue argument = impl().pushed_arguments_.back();
    impl().pushed_arguments_.pop_back();
    resolver.AddStackParameter(argument);
  }
  impl().SetLLVMValue(instr, resolver.BuildCall());
}

void IRTranslator::VisitDebugStepCheck(DebugStepCheckInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitLoadIndexed(LoadIndexedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue array = impl().GetLLVMValue(instr->array());
  LValue index = impl().GetLLVMValue(instr->index());

  const intptr_t shift =
      Utils::ShiftForPowerOfTwo(instr->index_scale()) - kSmiTagShift;
  int32_t offset =
      instr->IsExternal()
          ? 0
          : (compiler::target::Instance::DataOffsetFor(instr->class_id()) -
             kHeapObjectTag);
  if (shift < 0) {
    EMASSERT(shift == -1);
    index = output().buildSar(index, output().constIntPtr(1));
  } else {
    index = output().buildShl(index, output().constIntPtr(shift));
  }
  LValue offset_value = output().buildAdd(output().constIntPtr(offset), index);
  LValue gep =
      impl().BuildAccessPointer(array, offset_value, instr->representation());
  LValue val = output().buildLoad(gep);
  impl().SetLLVMValue(instr, val);
}

void IRTranslator::VisitLoadCodeUnits(LoadCodeUnitsInstr* instr) {
  impl().SetDebugLine(instr);
  LValue array = impl().GetLLVMValue(instr->array());
  LValue index = impl().GetLLVMValue(instr->index());
  const intptr_t shift =
      Utils::ShiftForPowerOfTwo(instr->index_scale()) - kSmiTagShift;
  int32_t offset =
      instr->IsExternal()
          ? 0
          : (compiler::target::Instance::DataOffsetFor(instr->class_id()) -
             kHeapObjectTag);
  if (shift < 0) {
    EMASSERT(shift == -1);
    index = output().buildSar(index, output().constIntPtr(1));
  } else {
    index = output().buildShl(index, output().constIntPtr(shift));
  }
  LValue offset_value = output().buildAdd(output().constIntPtr(offset), index);
  LValue value;
#if !defined(TARGET_ARCH_IS_64_BIT)
  if (instr->representation() == kUnboxedInt64) {
    LValue gep = impl().BuildAccessPointer(array, offset_value, kUnboxedUint32);
    LValue value_32 = output().buildLoad(gep);
    value = output().buildCast(LLVMZExt, value_32, output().repo().int64);
  }
#endif
  EMASSERT(instr->representation() == kTagged);
  LType type;
  switch (instr->class_id()) {
    case kOneByteStringCid:
    case kExternalOneByteStringCid:
      switch (instr->element_count()) {
        case 1:
          type = output().repo().int8;
          break;
        case 2:
          type = output().repo().int16;
          break;
        case 4:
          type = output().repo().int32;
          break;
        default:
          UNREACHABLE();
      }
      break;
    case kTwoByteStringCid:
    case kExternalTwoByteStringCid:
      switch (instr->element_count()) {
        case 1:
          type = output().repo().int16;
          break;
        case 2:
          type = output().repo().int32;
          break;
        default:
          UNREACHABLE();
      }
      break;
    default:
      UNREACHABLE();
      break;
  }
  LValue gep =
      impl().BuildAccessPointer(array, offset_value, pointerType(type));
  LValue int_value = impl().EnsureIntPtr(output().buildLoad(gep));

  if (instr->can_pack_into_smi()) {
    value = impl().SmiTag(int_value);
  } else {
#if defined(TARGET_ARCH_IS_32_BIT)
    DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
    diamond.LeftHint();
    diamond.BuildCmp([&]() {
      LValue and_value =
          output().buildAnd(int_value, output().constIntPtr(0xC0000000));
      LValue cmp =
          output().buildICmp(LLVMIntEQ, and_value, output().constIntPtr(0));
      return cmp;
    });
    diamond.BuildLeft([&]() { return impl().SmiTag(int_value); });
    diamond.BuildRight([&]() {
      BoxAllocationSlowPath allocation(instr, impl().mint_class(), impl());
      return allocation.Allocate();
    });
    value = diamond.End();
#else
    UNREACHABLE();
#endif
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitStoreIndexed(StoreIndexedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue array = impl().GetLLVMValue(instr->array());
  LValue index = impl().GetLLVMValue(instr->index());
  LValue value = impl().GetLLVMValue(instr->value());
  const intptr_t shift =
      Utils::ShiftForPowerOfTwo(instr->index_scale()) - kSmiTagShift;
  int32_t offset =
      instr->IsExternal()
          ? 0
          : (compiler::target::Instance::DataOffsetFor(instr->class_id()) -
             kHeapObjectTag);
  if (shift < 0) {
    EMASSERT(shift == -1);
    index = output().buildSar(index, output().constIntPtr(1));
  } else {
    index = output().buildShl(index, output().constIntPtr(shift));
  }
  LValue offset_value = output().buildAdd(output().constIntPtr(offset), index);
  switch (instr->class_id()) {
    case kArrayCid:
      if (instr->ShouldEmitStoreBarrier()) {
        impl().StoreIntoArray(instr, array, offset_value, value,
                              instr->CanValueBeSmi());
      } else {
        LValue gep = impl().BuildAccessPointer(
            array, offset_value, pointerType(output().tagged_type()));
        output().buildStore(value, gep);
      }
      break;
    case kTypedDataInt8ArrayCid:
    case kTypedDataUint8ArrayCid:
    case kExternalTypedDataUint8ArrayCid:
    case kOneByteStringCid: {
      EMASSERT(instr->RequiredInputRepresentation(2) == kUnboxedIntPtr);
      LValue value_to_store =
          output().buildCast(LLVMTrunc, value, output().repo().int8);
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref8);
      output().buildStore(value_to_store, gep);
      break;
    }
    case kTypedDataUint8ClampedArrayCid:
    case kExternalTypedDataUint8ClampedArrayCid: {
      EMASSERT(instr->RequiredInputRepresentation(2) == kUnboxedIntPtr);
      value = impl().EnsureIntPtr(value);
      LValue constant = output().constIntPtr(0xFF);
      LValue cmp_1 = output().buildICmp(LLVMIntSLE, value, constant);
      LValue cmp_2 = output().buildICmp(LLVMIntULE, value, constant);
      LValue value_select =
          output().buildSelect(cmp_1, output().constIntPtr(0), constant);
      value = output().buildSelect(cmp_2, value, value_select);
      LValue value_to_store =
          output().buildCast(LLVMTrunc, value, output().repo().int8);
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref8);
      output().buildStore(value_to_store, gep);
      break;
    }
    case kTypedDataInt16ArrayCid:
    case kTypedDataUint16ArrayCid: {
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref16);
      if (typeOf(value) != output().repo().int16)
        value = output().buildCast(LLVMTrunc, value, output().repo().int16);
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataInt32ArrayCid:
    case kTypedDataUint32ArrayCid: {
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref32);
      if (typeOf(value) != output().repo().int32)
        value = output().buildCast(LLVMTrunc, value, output().repo().int32);
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataInt64ArrayCid:
    case kTypedDataUint64ArrayCid: {
      LValue gep =
          impl().BuildAccessPointer(array, offset_value, output().repo().ref64);
      EMASSERT(typeOf(value) == output().repo().int64)
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataFloat32ArrayCid: {
      LValue gep = impl().BuildAccessPointer(array, offset_value,
                                             output().repo().refFloat);
      EMASSERT(typeOf(value) == output().repo().floatType)
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataFloat64ArrayCid: {
      LValue gep = impl().BuildAccessPointer(array, offset_value,
                                             output().repo().refDouble);
      EMASSERT(typeOf(value) == output().repo().doubleType)
      output().buildStore(value, gep);
      break;
    }
    case kTypedDataFloat64x2ArrayCid:
    case kTypedDataInt32x4ArrayCid:
    case kTypedDataFloat32x4ArrayCid: {
      UNREACHABLE();
      break;
    }
    default:
      UNREACHABLE();
  }
}

void IRTranslator::VisitStoreInstanceField(StoreInstanceFieldInstr* instr) {
  impl().SetDebugLine(instr);
  EMASSERT(!instr->IsUnboxedStore());
  const intptr_t offset_in_bytes = instr->OffsetInBytes();

  LValue val = impl().GetLLVMValue(instr->value());
  LValue instance = impl().GetLLVMValue(instr->instance());
  if (instr->ShouldEmitStoreBarrier()) {
    impl().StoreIntoObject(
        instr, instance, output().constIntPtr(offset_in_bytes - kHeapObjectTag),
        val, instr->CanValueBeSmi());
  } else {
    impl().StoreToOffset(instance, offset_in_bytes - kHeapObjectTag, val);
  }
}

void IRTranslator::VisitInitInstanceField(InitInstanceFieldInstr* instr) {
  impl().SetDebugLine(instr);
  LValue instance = impl().GetLLVMValue(instr->InputAt(0));
  LValue field_value =
      impl().LoadFieldFromOffset(instance, instr->field().Offset());
  LValue sentinel = impl().LoadObject(Object::sentinel());
  AssemblerResolver resolver(impl());
  LValue cmp = output().buildICmp(LLVMIntNE, field_value, sentinel);
  resolver.GotoMergeIf(impl().ExpectTrue(cmp));

  LValue field_original =
      impl().LoadObject(Field::ZoneHandle(instr->field().Original()));
  impl().PushArgument(instance);
  impl().PushArgument(field_original);
  impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                             kInitInstanceFieldRuntimeEntry, 2);
  resolver.GotoMerge();
  resolver.End();
}

void IRTranslator::VisitInitStaticField(InitStaticFieldInstr* instr) {
  impl().SetDebugLine(instr);
  Label call_runtime("call_runtime");
  LValue field_value = impl().GetLLVMValue(instr->InputAt(0));
  LValue static_value = impl().LoadFieldFromOffset(
      field_value, compiler::target::Field::static_value_offset());
  LValue sentinel = impl().LoadObject(Object::sentinel());
  AssemblerResolver resolver(impl());
  LValue cmp = output().buildICmp(LLVMIntEQ, static_value, sentinel);
  resolver.BranchIf(impl().ExpectFalse(cmp), call_runtime);

  LValue transition_sentinel = impl().LoadObject(Object::transition_sentinel());
  cmp = output().buildICmp(LLVMIntNE, static_value, transition_sentinel);
  resolver.GotoMergeIf(impl().ExpectTrue(cmp));
  resolver.Branch(call_runtime);

  resolver.Bind(call_runtime);
  impl().PushArgument(field_value);
  impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                             kInitStaticFieldRuntimeEntry, 1);
  resolver.GotoMerge();
  resolver.End();
}

void IRTranslator::VisitLoadStaticField(LoadStaticFieldInstr* instr) {
  impl().SetDebugLine(instr);
  LValue field = impl().GetLLVMValue(instr->field_value());
  LValue v = impl().LoadFieldFromOffset(
      field, compiler::target::Field::static_value_offset());
  impl().SetLLVMValue(instr, v);
}

void IRTranslator::VisitStoreStaticField(StoreStaticFieldInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->InputAt(0));
  LValue field_original = impl().LoadObject(
      Field::ZoneHandle(impl().zone(), instr->field().Original()));
  if (instr->value()->NeedsWriteBarrier()) {
    impl().StoreIntoObject(
        instr, field_original,
        output().constIntPtr(compiler::target::Field::static_value_offset() -
                             kHeapObjectTag),
        value, instr->CanValueBeSmi());
  } else {
    impl().StoreToOffset(
        field_original,
        compiler::target::Field::static_value_offset() - kHeapObjectTag, value);
  }
}

void IRTranslator::VisitBooleanNegate(BooleanNegateInstr* instr) {
  impl().SetDebugLine(instr);

  LValue true_or_false = impl().GetLLVMValue(instr->value());
  LValue true_value = impl().LoadObject(Bool::True());
  LValue false_value = impl().LoadObject(Bool::False());
  LValue cmp_value = output().buildICmp(LLVMIntEQ, true_value, true_or_false);
  LValue value = output().buildSelect(cmp_value, false_value, true_value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitInstanceOf(InstanceOfInstr* instr) {
  impl().SetDebugLine(instr);
  // FIXME: implement inline instance of
  LValue instance = impl().GetLLVMValue(instr->value());
  LValue instantiator_type_arguments =
      impl().GetLLVMValue(instr->instantiator_type_arguments());
  LValue function_type_arguments =
      impl().GetLLVMValue(instr->function_type_arguments());
  LValue null_object = impl().LoadObject(Object::null_object());
  impl().PushArgument(instance);
  impl().PushArgument(impl().LoadObject(instr->type()));
  impl().PushArgument(instantiator_type_arguments);
  impl().PushArgument(function_type_arguments);
  impl().PushArgument(null_object);  // cache
  LValue result = impl().GenerateRuntimeCall(
      instr, instr->token_pos(), instr->deopt_id(), kInstanceofRuntimeEntry, 5);
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitCreateArray(CreateArrayInstr* instr) {
  impl().SetDebugLine(instr);
  LValue length = impl().GetLLVMValue(instr->num_elements());
  LValue elem_type = impl().GetLLVMValue(instr->element_type());
  LValue result = impl().GenerateCall(
      instr, instr->token_pos(), instr->deopt_id(), StubCode::AllocateArray(),
      RawPcDescriptors::kOther, 0,
      {{kCreateArrayLengthReg, length},
       {kCreateArrayElementTypeReg, elem_type}});
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitAllocateObject(AllocateObjectInstr* instr) {
  impl().SetDebugLine(instr);

  if (instr->ArgumentCount() == 1) {
    TypeUsageInfo* type_usage_info = impl().thread()->type_usage_info();
    if (type_usage_info != nullptr) {
      RegisterTypeArgumentsUse(
          impl().flow_graph()->parsed_function().function(), type_usage_info,
          instr->cls(), instr->ArgumentAt(0));
    }
  }
  const Code& stub = Code::ZoneHandle(
      impl().zone(), StubCode::GetAllocationStubForClass(instr->cls()));

  LValue result =
      impl().GenerateCall(instr, instr->token_pos(), instr->deopt_id(), stub,
                          RawPcDescriptors::kOther, instr->ArgumentCount(), {});
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitLoadField(LoadFieldInstr* instr) {
  impl().SetDebugLine(instr);
  LValue instance = impl().GetLLVMValue(instr->instance());
  LValue boxed =
      impl().LoadFieldFromOffset(instance, instr->slot().offset_in_bytes());
  if (instr->IsUnboxedLoad()) {
    const intptr_t cid = instr->slot().field().UnboxedFieldCid();
    LValue value;
    switch (cid) {
      case kDoubleCid:
        value = impl().LoadFieldFromOffset(
            boxed, compiler::target::Double::value_offset(),
            output().repo().refDouble);
        break;
      case kFloat32x4Cid:
        EMASSERT("kFloat32x4Cid not supported" && false);
        UNREACHABLE();
      case kFloat64x2Cid:
        EMASSERT("kFloat64x2Cid not supported" && false);
        UNREACHABLE();
      default:
        UNREACHABLE();
    }
    impl().SetLLVMValue(instr, value);
    return;
  }
  if (instr->IsPotentialUnboxedLoad()) {
    // FIXME: need for continuation resolver
    // support after call implementation.
    UNREACHABLE();
  }
  impl().SetLLVMValue(instr, boxed);
}

void IRTranslator::VisitLoadUntagged(LoadUntaggedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue object = impl().GetLLVMValue(instr->object());
  LValue value;
  if (instr->object()->definition()->representation() == kUntagged) {
    value =
        impl().LoadFromOffset(object, instr->offset(), output().repo().refPtr);
  } else {
    value = impl().LoadFieldFromOffset(object, instr->offset());
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitStoreUntagged(StoreUntaggedInstr* instr) {
  impl().SetDebugLine(instr);
  LValue object = impl().GetLLVMValue(instr->object());
  LValue value = impl().GetLLVMValue(instr->value());
  impl().StoreToOffset(object, instr->offset_from_tagged(), value);
}

void IRTranslator::VisitLoadClassId(LoadClassIdInstr* instr) {
  impl().SetDebugLine(instr);
  const AbstractType& value_type = *instr->object()->Type()->ToAbstractType();
  LValue object = impl().GetLLVMValue(instr->object());
  LValue value;
  if (CompileType::Smi().IsAssignableTo(value_type) ||
      value_type.IsTypeParameter()) {
    DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
    diamond.BuildCmp([&]() { return impl().TstSmi(object); })
        .BuildLeft([&]() { return output().constInt16(kSmiCid); })
        .BuildRight([&]() { return impl().LoadClassId(object); });

    value = diamond.End();
  } else {
    value = impl().LoadClassId(object);
  }
  value = impl().SmiTag(value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitInstantiateType(InstantiateTypeInstr* instr) {
  impl().SetDebugLine(instr);
  LValue instantiator_type_args =
      impl().GetLLVMValue(instr->instantiator_type_arguments());
  LValue function_type_args =
      impl().GetLLVMValue(instr->function_type_arguments());
  impl().PushArgument(impl().LoadObject(instr->type()));
  impl().PushArgument(instantiator_type_args);
  impl().PushArgument(function_type_args);
  LValue result =
      impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                                 kInstantiateTypeRuntimeEntry, 3);
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitInstantiateTypeArguments(
    InstantiateTypeArgumentsInstr* instr) {
  impl().SetDebugLine(instr);
  LValue instantiator_type_args =
      impl().GetLLVMValue(instr->instantiator_type_arguments());
  LValue function_type_args =
      impl().GetLLVMValue(instr->function_type_arguments());
  AssemblerResolver resolver(impl());
  Label type_arguments_instantiated("type_arguments_instantiated");
  // If both the instantiator and function type arguments are null and if the
  // type argument vector instantiated from null becomes a vector of dynamic,
  // then use null as the type arguments.
  const intptr_t len = instr->type_arguments().Length();
  if (instr->type_arguments().IsRawWhenInstantiatedFromRaw(len)) {
    LValue null_object = impl().LoadObject(Object::null_object());
    LValue cmp_1 =
        output().buildICmp(LLVMIntEQ, instantiator_type_args, null_object);
    LValue cmp_2 =
        output().buildICmp(LLVMIntEQ, function_type_args, null_object);
    LValue and_val = output().buildAnd(cmp_1, cmp_2);
    resolver.GotoMergeWithValueIf(and_val, null_object);
  }
  // Lookup cache before calling runtime.
  // TODO(regis): Consider moving this into a shared stub to reduce
  // generated code size.
  LValue type_arguments_value = impl().LoadObject(instr->type_arguments());
  LValue instantiations_value = impl().LoadFieldFromOffset(
      type_arguments_value,
      compiler::target::TypeArguments::instantiations_offset());
  LValue data_value = output().buildAdd(
      impl().TaggedToWord(instantiations_value),
      output().constIntPtr(compiler::target::Array::data_offset() -
                           kHeapObjectTag));
  Label loop("loop"), next("next"), found("found"), slow_case("slow_case");
  LBasicBlock from_block = resolver.current();
  resolver.Branch(loop);
  LValue data_phi = output().buildPhi(typeOf(data_value));
  addIncoming(data_phi, &data_value, &from_block, 1);
  LValue cached_instantiator_type_args =
      output().buildLoad(impl().BuildAccessPointer(
          data_phi, output().constIntPtr(0 * compiler::target::kWordSize),
          output().tagged_type()));
  LValue cmp = output().buildICmp(LLVMIntEQ, cached_instantiator_type_args,
                                  instantiator_type_args);
  resolver.BranchIfNot(cmp, next);
  LValue cached_function_type_args =
      output().buildLoad(impl().BuildAccessPointer(
          data_phi, output().constIntPtr(1 * compiler::target::kWordSize),
          output().tagged_type()));
  cmp = output().buildICmp(LLVMIntEQ, cached_function_type_args,
                           function_type_args);
  resolver.BranchIf(cmp, found);
  resolver.Branch(next);
  resolver.Bind(next);

  data_value = output().buildAdd(
      data_phi, output().constIntPtr(StubCode::kInstantiationSizeInWords *
                                     compiler::target::kWordSize));
  cmp = output().buildICmp(
      LLVMIntEQ, cached_instantiator_type_args,
      output().constTagged(static_cast<intptr_t>(
          compiler::target::ToRawSmi(StubCode::kNoInstantiator))));
  from_block = resolver.current();
  addIncoming(data_phi, &data_value, &from_block, 1);
  resolver.BranchIfNot(impl().ExpectFalse(cmp), loop);
  resolver.Branch(slow_case);

  resolver.Bind(found);
  LValue result = output().buildLoad(impl().BuildAccessPointer(
      data_phi, output().constIntPtr(2 * compiler::target::kWordSize),
      output().tagged_type()));
  resolver.GotoMergeWithValue(result);

  resolver.Bind(slow_case);
  impl().PushArgument(impl().LoadObject(instr->type_arguments()));
  impl().PushArgument(instantiator_type_args);
  impl().PushArgument(function_type_args);
  result =
      impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                                 kInstantiateTypeArgumentsRuntimeEntry, 3);
  resolver.GotoMergeWithValue(result);
  result = resolver.End();
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitAllocateContext(AllocateContextInstr* instr) {
  impl().SetDebugLine(instr);
  LValue result = impl().GenerateCall(
      instr, instr->token_pos(), instr->deopt_id(), StubCode::AllocateContext(),
      RawPcDescriptors::kOther, 0,
      {{kAllocateContextNumOfContextVarsReg,
        output().constIntPtr(instr->num_context_variables())}});
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitAllocateUninitializedContext(
    AllocateUninitializedContextInstr* instr) {
  intptr_t instance_size =
      Context::InstanceSize(instr->num_context_variables());
  AssemblerResolver resolver(impl());
  Label slow_path("AllocateContextSlowPath:%d", instr->ssa_temp_index());
  LValue instance =
      impl().TryAllocate(resolver, slow_path, kContextCid, instance_size);
  if (instance) {
    impl().StoreToOffset(
        instance,
        compiler::target::Context::num_variables_offset() - kHeapObjectTag,
        output().constIntPtr(instr->num_context_variables()));
    resolver.GotoMergeWithValue(instance);
  }
  resolver.Bind(slow_path);
  resolver.GotoMergeWithValue(impl().GenerateCall(
      instr, instr->token_pos(), instr->deopt_id(), StubCode::AllocateContext(),
      RawPcDescriptors::kOther, 0,
      {{kAllocateContextNumOfContextVarsReg,
        output().constIntPtr(instr->num_context_variables())}}));
  LValue result = resolver.End();
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitCloneContext(CloneContextInstr* instr) {
  impl().SetDebugLine(instr);
  LValue context_value = impl().GetLLVMValue(instr->context_value());
  impl().PushArgument(context_value);
  LValue result =
      impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                                 kCloneContextRuntimeEntry, 1);
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitBinarySmiOp(BinarySmiOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().SmiUntag(impl().GetLLVMValue(instr->left()));
  LValue right = impl().SmiUntag(impl().GetLLVMValue(instr->right()));
  LValue value;
  switch (instr->op_kind()) {
    case Token::kSHL:
      value = output().buildShl(left, right);
      break;
    case Token::kADD:
      value = output().buildAdd(left, right);
      break;
    case Token::kMUL:
      value = output().buildMul(left, right);
      break;
    case Token::kTRUNCDIV:
      value = output().buildSDiv(left, right);
      break;
    case Token::kBIT_AND:
      value = output().buildAnd(left, right);
      break;
    case Token::kBIT_OR:
      value = output().buildOr(left, right);
      break;
    case Token::kBIT_XOR:
      value = output().buildXor(left, right);
      break;
    case Token::kSHR:
      value = output().buildShr(left, right);
      break;
    default:
      UNREACHABLE();
      break;
  }
  value = impl().SmiTag(value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitCheckedSmiComparison(CheckedSmiComparisonInstr* instr) {
  impl().SetDebugLine(instr);
  ComparisonResolver resolver(impl());
  instr->Accept(&resolver);
  impl().SetLLVMValue(instr, impl().BooleanToObject(resolver.result()));
}

void IRTranslator::VisitCheckedSmiOp(CheckedSmiOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().GetLLVMValue(instr->InputAt(0));
  LValue right = impl().GetLLVMValue(instr->InputAt(1));

  intptr_t left_cid = instr->left()->Type()->ToCid();
  intptr_t right_cid = instr->right()->Type()->ToCid();
  DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
  diamond.LeftHint();
  diamond.BuildCmp([&] {
    LValue is_smi;
    if (instr->left()->definition() == instr->right()->definition()) {
      is_smi = impl().TstSmi(left);
    } else if (left_cid == kSmiCid) {
      is_smi = impl().TstSmi(right);
    } else if (right_cid == kSmiCid) {
      is_smi = impl().TstSmi(left);
    } else {
      LValue left_address = impl().TaggedToWord(left);
      LValue right_address = impl().TaggedToWord(right);
      LValue orr_address = output().buildOr(left_address, right_address);
      LValue and_value =
          output().buildAnd(orr_address, output().constIntPtr(kSmiTagMask));
      is_smi = output().buildICmp(LLVMIntNE, and_value,
                                  output().constIntPtr(kSmiTagMask));
    }
    return is_smi;
  });
  diamond.BuildLeft([&] {
    LValue result;
    switch (instr->op_kind()) {
      case Token::kADD: {
#if defined(TARGET_ARCH_IS_32_BIT)
        LValue add_result = output().buildCall(
            output().repo().addWithOverflow32Intrinsic(),
            impl().TaggedToWord(left), impl().TaggedToWord(right));
#else
        LValue add_result = output().buildCall(
            output().repo().addWithOverflow64Intrinsic(),
            impl().TaggedToWord(left), impl().TaggedToWord(right));
#endif
        diamond.BranchToOtherLeafOnCondition(
            output().buildExtractValue(add_result, 1));
        result = impl().WordToTagged(output().buildExtractValue(add_result, 0));
      } break;
      case Token::kSUB: {
#if defined(TARGET_ARCH_IS_32_BIT)
        LValue sub_result = output().buildCall(
            output().repo().subWithOverflow32Intrinsic(),
            impl().TaggedToWord(left), impl().TaggedToWord(right));
#else
        LValue sub_result = output().buildCall(
            output().repo().subWithOverflow64Intrinsic(),
            impl().TaggedToWord(left), impl().TaggedToWord(right));
#endif
        diamond.BranchToOtherLeafOnCondition(
            output().buildExtractValue(sub_result, 1));
        result = impl().WordToTagged(output().buildExtractValue(sub_result, 0));
      } break;
      case Token::kMUL: {
#if defined(TARGET_ARCH_IS_32_BIT)
        LValue mul_result = output().buildCall(
            output().repo().mulWithOverflow32Intrinsic(), impl().SmiUntag(left),
            impl().TaggedToWord(right));
#else
        LValue mul_result = output().buildCall(
            output().repo().mulWithOverflow64Intrinsic(), impl().SmiUntag(left),
            impl().TaggedToWord(right));
#endif
        diamond.BranchToOtherLeafOnCondition(
            output().buildExtractValue(mul_result, 1));
        result = impl().WordToTagged(output().buildExtractValue(mul_result, 0));
      } break;
      case Token::kBIT_OR:
        result = output().buildOr(impl().TaggedToWord(left),
                                  impl().TaggedToWord(right));
        result = impl().WordToTagged(result);
        break;
      case Token::kBIT_AND:
        result = output().buildAnd(impl().TaggedToWord(left),
                                   impl().TaggedToWord(right));
        result = impl().WordToTagged(result);
        break;
      case Token::kBIT_XOR:
        result = output().buildXor(impl().TaggedToWord(left),
                                   impl().TaggedToWord(right));
        result = impl().WordToTagged(result);
        break;
      case Token::kSHL: {
        LValue right_int = impl().SmiUntag(right);
        LValue overflow = output().buildICmp(
            LLVMIntUGT, right_int,
            output().constIntPtr(compiler::target::kSmiBits));
        diamond.BranchToOtherLeafOnCondition(overflow);
        LValue left_int = impl().TaggedToWord(left);
        result = output().buildShl(left_int, right_int);
        LValue check_for_overflow = output().buildSar(result, right_int);
        diamond.BranchToOtherLeafOnCondition(
            output().buildICmp(LLVMIntEQ, check_for_overflow, result));
        result = impl().WordToTagged(result);
      } break;
      case Token::kSHR: {
        LValue right_int = impl().SmiUntag(right);
        LValue overflow = output().buildICmp(
            LLVMIntUGT, right_int,
            output().constIntPtr(compiler::target::kSmiBits));
        diamond.BranchToOtherLeafOnCondition(overflow);

        LValue left_int = impl().SmiUntag(left);
        result = output().buildSar(left_int, right_int);
        result = impl().SmiTag(result);
      } break;
      default:
        UNREACHABLE();
    }
    return result;
  });
  diamond.BuildRight([&] {
    impl().PushArgument(left);
    impl().PushArgument(right);
    const auto& selector = String::Handle(instr->call()->Selector());
    const auto& arguments_descriptor = Array::Handle(
        ArgumentsDescriptor::New(/*type_args_len=*/0, /*num_arguments=*/2));
    return impl().GenerateMegamorphicInstanceCall(
        instr, selector, arguments_descriptor, instr->call()->deopt_id(),
        instr->token_pos());
  });
  LValue final_result = diamond.End();
  impl().SetLLVMValue(instr, final_result);
}

void IRTranslator::VisitBinaryInt32Op(BinaryInt32OpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().EnsureInt32(impl().GetLLVMValue(instr->left()));
  LValue right = impl().EnsureInt32(impl().GetLLVMValue(instr->right()));
  LValue value;
  switch (instr->op_kind()) {
    case Token::kSHL:
      value = output().buildShl(left, right);
      break;
    case Token::kADD:
      value = output().buildAdd(left, right);
      break;
    case Token::kMUL:
      value = output().buildMul(left, right);
      break;
    case Token::kTRUNCDIV:
      value = output().buildSDiv(left, right);
      break;
    case Token::kBIT_AND:
      value = output().buildAnd(left, right);
      break;
    case Token::kBIT_OR:
      value = output().buildOr(left, right);
      break;
    case Token::kBIT_XOR:
      value = output().buildXor(left, right);
      break;
    case Token::kSHR:
      value = output().buildShr(left, right);
      break;
    default:
      UNREACHABLE();
      break;
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitUnarySmiOp(UnarySmiOpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnaryDoubleOp(UnaryDoubleOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue v = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(v) == output().repo().doubleType);
  impl().SetLLVMValue(instr, output().buildFNeg(v));
}

void IRTranslator::VisitCheckStackOverflow(CheckStackOverflowInstr* instr) {
  impl().SetDebugLine(instr);
  LValue stack_pointer =
      output().buildCall(output().repo().stackSaveIntrinsic());
  LValue stack_pointer_int =
      output().buildCast(LLVMPtrToInt, stack_pointer, output().repo().intPtr);

  LValue stack_limit = output().buildLoad(output().buildGEPWithByteOffset(
      output().thread(),
      output().constIntPtr(compiler::target::Thread::stack_limit_offset()),
      pointerType(output().repo().intPtr)));
  AssemblerResolver resolver(impl());
  Label slow_path("CheckStackOverflowSlowPath");
  LValue cmp = output().buildICmp(LLVMIntULE, stack_pointer_int, stack_limit);
  resolver.BranchIf(impl().ExpectFalse(cmp), slow_path);
  resolver.GotoMerge();
  resolver.Bind(slow_path);
  // FIXME: support live fpu register here.
  impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                             kStackOverflowRuntimeEntry, 0, false);
  resolver.GotoMerge();
  resolver.End();
}

void IRTranslator::VisitSmiToDouble(SmiToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue v = impl().GetLLVMValue(instr->value());
  LValue num = impl().SmiUntag(v);
  LValue dnum = output().buildCast(LLVMSIToFP, num, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitInt32ToDouble(Int32ToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue num = impl().EnsureInt32(impl().GetLLVMValue(instr->value()));
  LValue dnum = output().buildCast(LLVMSIToFP, num, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitInt64ToDouble(Int64ToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue num = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(num) == output().repo().int64);
  LValue dnum = output().buildCast(LLVMSIToFP, num, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitDoubleToInteger(DoubleToIntegerInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToSmi(DoubleToSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToDouble(DoubleToDoubleInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToFloat(DoubleToFloatInstr* instr) {
  impl().SetDebugLine(instr);
  LValue fnum = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(fnum) == output().repo().doubleType);
  LValue dnum =
      output().buildCast(LLVMFPTrunc, fnum, output().repo().floatType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitFloatToDouble(FloatToDoubleInstr* instr) {
  impl().SetDebugLine(instr);
  LValue fnum = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(fnum) == output().repo().floatType);
  LValue dnum = output().buildCast(LLVMFPExt, fnum, output().repo().doubleType);
  impl().SetLLVMValue(instr, dnum);
}

void IRTranslator::VisitCheckClass(CheckClassInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCheckClassId(CheckClassIdInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCheckSmi(CheckSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitCheckNull(CheckNullInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  AssemblerResolver resolver(impl());
  Label slow_path("CheckNullSlowPath");
  LValue null_object = impl().LoadObject(Object::null_object());
  LValue cmp = output().buildICmp(LLVMIntEQ, null_object, value);
  resolver.BranchIf(impl().ExpectFalse(cmp), slow_path);
  resolver.Bind(slow_path);
  impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                             kNullErrorRuntimeEntry, 0, false);
}

void IRTranslator::VisitCheckCondition(CheckConditionInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitConstant(ConstantInstr* instr) {
  impl().SetLazyValue(instr);
}

void IRTranslator::VisitUnboxedConstant(UnboxedConstantInstr* instr) {
  impl().SetLazyValue(instr);
}

void IRTranslator::VisitCheckEitherNonSmi(CheckEitherNonSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinaryDoubleOp(BinaryDoubleOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  EMASSERT(typeOf(left) == output().repo().doubleType);
  EMASSERT(typeOf(right) == output().repo().doubleType);
  LValue value;
  switch (instr->op_kind()) {
    case Token::kADD:
      value = output().buildFAdd(left, right);
      break;
    case Token::kSUB:
      value = output().buildFSub(left, right);
      break;
    case Token::kMUL:
      value = output().buildFMul(left, right);
      break;
    case Token::kDIV:
      value = output().buildFDiv(left, right);
      break;
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitDoubleTestOp(DoubleTestOpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(value) == output().repo().doubleType);
  LValue cmp_value;
  if (instr->op_kind() == MethodRecognizer::kDouble_getIsNaN) {
    cmp_value = output().buildFCmp(LLVMRealUNO, value, value);
  } else {
    ASSERT(instr->op_kind() == MethodRecognizer::kDouble_getIsInfinite);
    value = output().buildCall(output().repo().doubleAbsIntrinsic(), value);
    cmp_value =
        output().buildFCmp(LLVMRealOEQ, value, output().constDouble(INFINITY));
  }
  const bool is_negated = instr->kind() != Token::kEQ;
  if (is_negated) {
    cmp_value =
        output().buildICmp(LLVMIntEQ, cmp_value, output().repo().booleanFalse);
  }
  impl().SetLLVMValue(instr, cmp_value);
}

void IRTranslator::VisitMathUnary(MathUnaryInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(value) == output().repo().doubleType);
  if (instr->kind() == MathUnaryInstr::kSqrt) {
    value = output().buildCall(output().repo().doubleSqrtIntrinsic(), value);
  } else if (instr->kind() == MathUnaryInstr::kDoubleSquare) {
    value = output().buildFMul(value, value);
  } else {
    UNREACHABLE();
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitMathMinMax(MathMinMaxInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  LValue value;
  LValue cmp_value;
  const intptr_t is_min = (instr->op_kind() == MethodRecognizer::kMathMin);
  if (instr->result_cid() == kDoubleCid) {
    EMASSERT(typeOf(left) == output().repo().doubleType);
    EMASSERT(typeOf(right) == output().repo().doubleType);
    cmp_value = output().buildFCmp(LLVMRealOGT, left, right);
  } else {
    cmp_value = output().buildICmp(LLVMIntSGT, left, right);
  }
  if (is_min)
    value = output().buildSelect(cmp_value, right, left);
  else
    value = output().buildSelect(cmp_value, left, right);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitBox(BoxInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  auto BoxClassFor = [&](Representation rep) -> const Class& {
    switch (rep) {
      case kUnboxedFloat:
      case kUnboxedDouble:
        return impl().double_class();
      case kUnboxedFloat32x4:
        LLVMLOGE("unsupported kUnboxedFloat32x4");
        UNREACHABLE();
      case kUnboxedFloat64x2:
        LLVMLOGE("unsupported kUnboxedFloat64x2");
        UNREACHABLE();
      case kUnboxedInt32x4:
        LLVMLOGE("unsupported kUnboxedInt32x4");
        UNREACHABLE();
      case kUnboxedInt64:
        return impl().mint_class();
      default:
        UNREACHABLE();
        return Class::ZoneHandle();
    }
  };

  BoxAllocationSlowPath allocate(
      instr, BoxClassFor(instr->from_representation()), impl());
  LValue result = allocate.Allocate();
  switch (instr->from_representation()) {
    case kUnboxedDouble:
      impl().StoreToOffset(result, instr->ValueOffset() - kHeapObjectTag,
                           value);
      break;
    case kUnboxedFloat:
      impl().StoreToOffset(result, instr->ValueOffset() - kHeapObjectTag,
                           value);
      break;
    case kUnboxedFloat32x4:
    case kUnboxedFloat64x2:
    case kUnboxedInt32x4:
    default:
      UNREACHABLE();
      break;
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitUnbox(UnboxInstr* instr) {
  // FIXME: need to review 64 bit integer.
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  LValue result;
  auto EmitLoadFromBox = [&]() {
    switch (instr->representation()) {
      case kUnboxedInt64: {
        LValue gep = output().buildGEPWithByteOffset(
            value,
            Boxing::ValueOffset(instr->representation()) - kHeapObjectTag,
            pointerType(output().repo().int64));
        return output().buildLoad(gep);
      }

      case kUnboxedDouble: {
        LValue gep = output().buildGEPWithByteOffset(
            value,
            Boxing::ValueOffset(instr->representation()) - kHeapObjectTag,
            pointerType(output().repo().doubleType));
        return output().buildLoad(gep);
      }

      case kUnboxedFloat: {
        LValue gep = output().buildGEPWithByteOffset(
            value,
            Boxing::ValueOffset(instr->representation()) - kHeapObjectTag,
            pointerType(output().repo().doubleType));
        LValue result = output().buildLoad(gep);
        return output().buildCast(LLVMFPTrunc, result,
                                  output().repo().floatType);
      }

      case kUnboxedFloat32x4:
      case kUnboxedFloat64x2:
      case kUnboxedInt32x4: {
        UNREACHABLE();
        break;
      }

      default:
        UNREACHABLE();
        break;
    }
  };

  auto EmitLoadInt32FromBoxOrSmi = [&]() {
    DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
    diamond.BuildCmp([&]() { return impl().TstSmi(value); })
        .BuildLeft([&]() { return impl().SmiUntag(value); })
        .BuildRight([&]() {
          return impl().LoadFieldFromOffset(
              value, compiler::target::Mint::value_offset(),
              output().repo().ref32);
        });
    return diamond.End();
  };
  auto EmitSmiConversion = [&]() {
    LValue result;
    switch (instr->representation()) {
      case kUnboxedInt64: {
        result = impl().SmiUntag(value);
        result = output().buildCast(LLVMSExt, result, output().repo().int64);
        break;
      }

      case kUnboxedDouble: {
        result = impl().SmiUntag(value);
        result =
            output().buildCast(LLVMSIToFP, result, output().repo().doubleType);
        break;
      }

      default:
        UNREACHABLE();
        break;
    }
    return result;
  };
  auto EmitLoadInt64FromBoxOrSmi = [&]() {
    DiamondContinuationResolver diamond(impl(), instr->ssa_temp_index());
    diamond.BuildCmp([&]() { return impl().TstSmi(value); })
        .BuildLeft([&]() {
          LValue result = impl().SmiUntag(value);
          return output().buildCast(LLVMSExt, result, output().repo().int64);
        })
        .BuildRight([&]() { return EmitLoadFromBox(); });
    return diamond.End();
  };
  if (instr->speculative_mode() == UnboxInstr::kNotSpeculative) {
    switch (instr->representation()) {
      case kUnboxedDouble:
      case kUnboxedFloat: {
        result = EmitLoadFromBox();
      } break;

      case kUnboxedInt32:
      case kUnboxedUint32:
        result = EmitLoadInt32FromBoxOrSmi();
        break;

      case kUnboxedInt64: {
        if (instr->value()->Type()->ToCid() == kSmiCid) {
          // Smi -> int64 conversion is more efficient than
          // handling arbitrary smi/mint.
          result = EmitSmiConversion();
        } else {
          result = EmitLoadInt64FromBoxOrSmi();
        }
        break;
      }
      default:
        UNREACHABLE();
        break;
    }
  } else {
    ASSERT(instr->speculative_mode() == UnboxInstr::kGuardInputs);
    const intptr_t value_cid = instr->value()->Type()->ToCid();
    const intptr_t box_cid = Boxing::BoxCid(instr->representation());

    if (value_cid == box_cid) {
      result = EmitLoadFromBox();
    } else if (instr->CanConvertSmi() && (value_cid == kSmiCid)) {
      result = EmitSmiConversion();
    } else if (instr->representation() == kUnboxedInt32 &&
               instr->value()->Type()->IsInt()) {
      result = EmitLoadInt32FromBoxOrSmi();
    } else if (instr->representation() == kUnboxedInt64 &&
               instr->value()->Type()->IsInt()) {
      result = EmitLoadInt64FromBoxOrSmi();
    } else {
      UNREACHABLE();
    }
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitBoxInt64(BoxInt64Instr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  EMASSERT(typeOf(value) == output().repo().int64);
  LValue result;
  if (instr->ValueFitsSmi()) {
    value = output().buildCast(LLVMTrunc, value, output().repo().intPtr);
    result = impl().SmiTag(value);
  } else {
    AssemblerResolver resolver(impl());
    Label slow_path("Allocate for BoxInt64:%d", instr->ssa_temp_index());
#if defined(TARGET_ARCH_IS_32_BIT)
    LValue value_lo =
        output().buildCast(LLVMTrunc, value, output().repo().int32);
    LValue value_hi = output().buildCast(
        LLVMTrunc, output().buildShr(value, output().constInt32(32)),
        output().repo().int32);
    result = output().buildShl(value_lo, output().constInt32(kSmiTagSize));
    LValue cmp_1 = output().buildICmp(
        LLVMIntEQ, output().buildSar(result, output().constInt32(kSmiTagSize)),
        value_lo);
    LValue cmp_2 =
        output().buildICmp(LLVMIntEQ, value_hi,
                           output().buildSar(result, output().constInt32(31)));
    LValue cmp = output().buildAnd(cmp_1, cmp_2);
#else
    result = output().buildShl(value, output().constInt64(kSmiTagSize));
    LValue cmp = output().buildICmp(
        LLVMIntEQ, value,
        output().buildSar(result, output().constInt64(kSmiTagSize)));
#endif
    resolver.BranchIfNot(impl().ExpectTrue(cmp), slow_path);
    resolver.GotoMergeWithValue(impl().WordToTagged(result));

    resolver.Bind(slow_path);
    BoxAllocationSlowPath allocate(instr, impl().mint_class(), impl());
    result = allocate.Allocate();
    impl().StoreToOffset(
        result, compiler::target::Mint::value_offset() - kHeapObjectTag, value);
    resolver.GotoMergeWithValue(result);
    result = resolver.End();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitUnboxInt64(UnboxInt64Instr* instr) {
  VisitUnbox(instr);
}

void IRTranslator::VisitCaseInsensitiveCompare(
    CaseInsensitiveCompareInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinaryInt64Op(BinaryInt64OpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  EMASSERT(typeOf(left) == output().repo().int64);
  EMASSERT(typeOf(right) == output().repo().int64);
  LValue result;
  switch (instr->op_kind()) {
    case Token::kBIT_AND: {
      result = output().buildAnd(left, right);
      break;
    }
    case Token::kBIT_OR: {
      result = output().buildOr(left, right);
      break;
    }
    case Token::kBIT_XOR: {
      result = output().buildXor(left, right);
      break;
    }
    case Token::kADD: {
      result = output().buildAdd(left, right);
      break;
    }
    case Token::kSUB: {
      result = output().buildSub(left, right);
      break;
    }
    case Token::kMUL: {
      result = output().buildMul(left, right);
      break;
    }
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitShiftInt64Op(ShiftInt64OpInstr* instr) {
  impl().SetDebugLine(instr);

  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());

  // only support the constant shift now.
  EMASSERT(instr->right()->BindsToConstant());
  LValue result;

  switch (instr->op_kind()) {
    case Token::kSHR: {
      result = output().buildShr(left, right);
      break;
    }
    case Token::kSHL: {
      result = output().buildShl(left, right);
      break;
    }
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitSpeculativeShiftInt64Op(
    SpeculativeShiftInt64OpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnaryInt64Op(UnaryInt64OpInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());

  switch (instr->op_kind()) {
    case Token::kBIT_NOT:
      value = output().buildNot(value);
      break;
    case Token::kNEGATE:
      value = output().buildNeg(value);
      break;
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitCheckArrayBound(CheckArrayBoundInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGenericCheckBound(GenericCheckBoundInstr* instr) {
  impl().SetDebugLine(instr);
  LValue length = impl().GetLLVMValue(instr->length());
  LValue index = impl().GetLLVMValue(instr->index());
  const intptr_t index_cid = instr->index()->Type()->ToCid();
  AssemblerResolver resolver(impl());
  Label slow_path("GenericCheckBoundInstrSlowPath");
  if (index_cid != kSmiCid) {
    LValue is_smi = impl().TstSmi(index);
    resolver.BranchIfNot(impl().ExpectTrue(is_smi), slow_path);
  }
  LValue cmp = output().buildICmp(LLVMIntUGE, index, length);
  resolver.BranchIf(impl().ExpectFalse(cmp), slow_path);
  resolver.Bind(slow_path);
  impl().PushArgument(length);
  impl().PushArgument(index);
  impl().GenerateRuntimeCall(instr, instr->token_pos(), instr->deopt_id(),
                             kRangeErrorRuntimeEntry, 2, false);
  resolver.End();
}

void IRTranslator::VisitConstraint(ConstraintInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStringToCharCode(StringToCharCodeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitOneByteStringFromCharCode(
    OneByteStringFromCharCodeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitStringInterpolate(StringInterpolateInstr* instr) {
  impl().SetDebugLine(instr);
  const int kTypeArgsLen = 0;
  const int kNumberOfArguments = 1;
  const Array& kNoArgumentNames = Object::null_array();
  ArgumentsInfo args_info(kTypeArgsLen, kNumberOfArguments, kNoArgumentNames);
  std::vector<LValue> arguments{impl().GetLLVMValue(instr->value())};
  LValue result = impl().GenerateStaticCall(
      instr, instr->ssa_temp_index(), arguments, instr->deopt_id(),
      instr->token_pos(), instr->CallFunction(), args_info, ICData::Handle());
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitInvokeMathCFunction(InvokeMathCFunctionInstr* instr) {
  impl().SetDebugLine(instr);
  LValue runtime_entry_point = impl().LoadFromOffset(
      output().thread(),
      compiler::target::Thread::OffsetFromThread(&instr->TargetFunction()),
      pointerType(output().repo().ref8));

  LValue result;
  if (instr->ArgumentCount() == 1) {
    LType function_type =
        functionType(output().repo().doubleType, output().repo().doubleType);
    LValue function = output().buildCast(LLVMBitCast, runtime_entry_point,
                                         pointerType(function_type));
    LValue input0 = impl().GetLLVMValue(instr->InputAt(0));
    EMASSERT(typeOf(input0) == output().repo().doubleType);
    result = output().buildCall(function, input0);
  } else if (instr->ArgumentCount() == 2) {
    LType function_type =
        functionType(output().repo().doubleType, output().repo().doubleType,
                     output().repo().doubleType);
    LValue function = output().buildCast(LLVMBitCast, runtime_entry_point,
                                         pointerType(function_type));
    LValue input0 = impl().GetLLVMValue(instr->InputAt(0));
    LValue input1 = impl().GetLLVMValue(instr->InputAt(1));
    EMASSERT(typeOf(input0) == output().repo().doubleType);
    EMASSERT(typeOf(input1) == output().repo().doubleType);
    result = output().buildCall(function, input0, input1);
  } else {
    UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitTruncDivMod(TruncDivModInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldClass(GuardFieldClassInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldLength(GuardFieldLengthInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldType(GuardFieldTypeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

static bool IsPowerOfTwoKind(intptr_t v1, intptr_t v2) {
  return (Utils::IsPowerOfTwo(v1) && (v2 == 0)) ||
         (Utils::IsPowerOfTwo(v2) && (v1 == 0));
}

void IRTranslator::VisitIfThenElse(IfThenElseInstr* instr) {
  ComparisonInstr* compare = instr->comparison();
  if (!impl().current_bb_impl()->IsDefined(compare)) {
    compare->Accept(this);
  }
  EMASSERT(impl().current_bb_impl()->IsDefined(compare));
  impl().SetDebugLine(instr);
  LValue cmp_value = impl().GetLLVMValue(compare->ssa_temp_index());

  intptr_t true_value = instr->if_true();
  intptr_t false_value = instr->if_false();

  const bool is_power_of_two_kind = IsPowerOfTwoKind(true_value, false_value);

  if (is_power_of_two_kind) {
    if (true_value == 0) {
      // We need to have zero in result on true_condition.
      cmp_value = output().buildICmp(LLVMIntEQ, cmp_value,
                                     output().repo().booleanFalse);
    }
  } else {
    if (true_value == 0) {
      // Swap values so that false_value is zero.
      intptr_t temp = true_value;
      true_value = false_value;
      false_value = temp;
    } else {
      cmp_value = output().buildICmp(LLVMIntEQ, cmp_value,
                                     output().repo().booleanFalse);
    }
  }
  LValue result = output().buildSelect(cmp_value, output().constIntPtr(1),
                                       output().constIntPtr(0));

  if (is_power_of_two_kind) {
    const intptr_t shift =
        Utils::ShiftForPowerOfTwo(Utils::Maximum(true_value, false_value));
    result =
        output().buildShl(result, output().constIntPtr(shift + kSmiTagSize));
  } else {
    result = output().buildSub(result, output().constIntPtr(1));
    const int32_t val = compiler::target::ToRawSmi(true_value) -
                        compiler::target::ToRawSmi(false_value);
    result = output().buildAnd(result, output().constIntPtr(val));
    if (false_value != 0) {
      result = output().buildAdd(
          result,
          output().constIntPtr(compiler::target::ToRawSmi(false_value)));
    }
  }
  result = impl().WordToTagged(result);
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitMaterializeObject(MaterializeObjectInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitTestSmi(TestSmiInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitTestCids(TestCidsInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitExtractNthOutput(ExtractNthOutputInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitBinaryUint32Op(BinaryUint32OpInstr* instr) {
  impl().SetDebugLine(instr);

  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());
  EMASSERT(typeOf(left) == output().repo().int32);
  EMASSERT(typeOf(right) == output().repo().int32);
  LValue result;
  switch (instr->op_kind()) {
    case Token::kBIT_AND:
      result = output().buildAnd(left, right);
      break;
    case Token::kBIT_OR:
      result = output().buildOr(left, right);
      break;
    case Token::kBIT_XOR:
      result = output().buildXor(left, right);
      break;
    case Token::kADD:
      result = output().buildAdd(left, right);
      break;
    case Token::kSUB:
      result = output().buildSub(left, right);
      break;
    case Token::kMUL:
      result = output().buildMul(left, right);
      break;
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitShiftUint32Op(ShiftUint32OpInstr* instr) {
  impl().SetDebugLine(instr);

  LValue left = impl().GetLLVMValue(instr->left());
  LValue right = impl().GetLLVMValue(instr->right());

  // only support the constant shift now.
  EMASSERT(instr->right()->BindsToConstant());
  LValue result;

  switch (instr->op_kind()) {
    case Token::kSHR: {
      result = output().buildShr(left, right);
      break;
    }
    case Token::kSHL: {
      result = output().buildShl(left, right);
      break;
    }
    default:
      UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitSpeculativeShiftUint32Op(
    SpeculativeShiftUint32OpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnaryUint32Op(UnaryUint32OpInstr* instr) {
  impl().SetDebugLine(instr);
  EMASSERT(instr->op_kind() == Token::kBIT_NOT);
  LValue value = impl().GetLLVMValue(instr->value());

  value = output().buildNot(value);
  impl().SetLLVMValue(instr, value);
}

void IRTranslator::VisitBoxUint32(BoxUint32Instr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  LValue result = impl().BoxInteger32(instr, value, instr->ValueFitsSmi(),
                                      instr->from_representation());
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitUnboxUint32(UnboxUint32Instr* instr) {
  VisitUnbox(instr);
}

void IRTranslator::VisitBoxInt32(BoxInt32Instr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  LValue result = impl().BoxInteger32(instr, value, instr->ValueFitsSmi(),
                                      instr->from_representation());
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitUnboxInt32(UnboxInt32Instr* instr) {
  VisitUnbox(instr);
}

void IRTranslator::VisitIntConverter(IntConverterInstr* instr) {
  impl().SetDebugLine(instr);
  LValue value = impl().GetLLVMValue(instr->value());
  LValue result;

  if (instr->from() == kUnboxedInt32 && instr->to() == kUnboxedUint32) {
    result = value;
  } else if (instr->from() == kUnboxedUint32 && instr->to() == kUnboxedInt32) {
    result = value;
  } else if (instr->from() == kUnboxedInt64) {
    if (instr->to() == kUnboxedInt32) {
      result = output().buildCast(LLVMTrunc, value, output().repo().int32);
    } else {
      result = output().buildCast(LLVMTrunc, value, output().repo().int32);
    }
  } else if (instr->to() == kUnboxedInt64) {
    if (instr->from() == kUnboxedUint32) {
      result = output().buildCast(LLVMZExt, value, output().repo().int64);
    } else {
      result = output().buildCast(LLVMSExt, value, output().repo().int64);
    }
  } else {
    UNREACHABLE();
  }
  impl().SetLLVMValue(instr, result);
}

void IRTranslator::VisitBitCast(BitCastInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitUnboxedWidthExtender(UnboxedWidthExtenderInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitDeoptimize(DeoptimizeInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}

void IRTranslator::VisitSimdOp(SimdOpInstr* instr) {
  LLVMLOGE("unsupported IR: %s\n", __FUNCTION__);
  UNREACHABLE();
}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
