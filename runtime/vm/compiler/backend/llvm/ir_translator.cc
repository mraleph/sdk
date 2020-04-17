#include "vm/compiler/backend/llvm/ir_translator.h"
#if defined(DART_ENABLE_LLVM_COMPILER)
#include <unordered_map>

#include "vm/compiler/backend/llvm/compiler_state.h"
#include "vm/compiler/backend/llvm/liveness_analysis.h"
#include "vm/compiler/backend/llvm/output.h"

namespace dart {
namespace dart_llvm {
namespace {
struct NotMergedPhiDesc {
  BlockEntryInstr* pred;
  int value;
  LValue phi;
};

struct GCRelocateDesc {
  int value;
  int where;
  GCRelocateDesc(int v, int w) : value(v), where(w) {}
};

using GCRelocateDescList = std::vector<GCRelocateDesc>;

struct PredecessorCallInfo {
  GCRelocateDescList gc_relocates;
};

enum class ValueType { LLVMValue, RelativeCallTarget };

struct ValueDesc {
  ValueType type;
  union {
    LValue llvm_value;
    int64_t relative_call_target;
  };
};

struct IRTranslatorBlockImpl {
  std::vector<NotMergedPhiDesc> not_merged_phis;
  std::unordered_map<int, ValueDesc> values_;
  PredecessorCallInfo call_info;

  LBasicBlock native_bb = nullptr;
  LBasicBlock continuation = nullptr;
  LValue landing_pad = nullptr;

  bool started = false;
  bool ended = false;
  bool exception_block = false;

  inline void SetLLVMValue(int nid, LValue value) {
    values_[nid] = {ValueType::LLVMValue, {value}};
  }

  inline void SetValue(int nid, const ValueDesc& value) {
    values_[nid] = value;
  }

  inline LValue GetLLVMValue(int nid) const {
    auto found = values_.find(nid);
    EMASSERT(found != values_.end());
    EMASSERT(found->second.type == ValueType::LLVMValue);
    return found->second.llvm_value;
  }

  const ValueDesc& GetValue(int nid) const {
    auto found = values_.find(nid);
    EMASSERT(found != values_.end());
    return found->second;
  }

  inline std::unordered_map<int, ValueDesc>& values() { return values_; }

  inline void StartBuild() {
    EMASSERT(!started);
    EMASSERT(!ended);
    started = true;
  }

  inline void EndBuild() {
    EMASSERT(started);
    EMASSERT(!ended);
    ended = true;
  }
};

class AnonImpl {
 public:
  LBasicBlock GetNativeBB(BlockEntryInstr* bb);
  void EnsureNativeBB(BlockEntryInstr* bb, Output& output);
  IRTranslatorBlockImpl* GetTranslatorBlockImpl(BlockEntryInstr* bb);
  LBasicBlock GetNativeBBContinuation(BlockEntryInstr* bb);
  bool IsBBStartedToBuild(BlockEntryInstr* bb);
  bool IsBBEndedToBuild(BlockEntryInstr* bb);
  void StartBuild(BlockEntryInstr* bb, Output& output);

 private:
  std::unordered_map<BlockEntryInstr*, IRTranslatorBlockImpl> block_map_;
};

IRTranslatorBlockImpl* AnonImpl::GetTranslatorBlockImpl(BlockEntryInstr* bb) {
  auto& ref = block_map_[bb];
  return &ref;
}

void AnonImpl::EnsureNativeBB(BlockEntryInstr* bb, Output& output) {
  IRTranslatorBlockImpl* impl = GetTranslatorBlockImpl(bb);
  if (impl->native_bb) return;
  char buf[256];
  snprintf(buf, 256, "B%d", static_cast<int>(bb->block_id()));
  LBasicBlock native_bb = output.appendBasicBlock(buf);
  impl->native_bb = native_bb;
  impl->continuation = native_bb;
}

LBasicBlock AnonImpl::GetNativeBB(BlockEntryInstr* bb) {
  return GetTranslatorBlockImpl(bb)->native_bb;
}

LBasicBlock AnonImpl::GetNativeBBContinuation(BlockEntryInstr* bb) {
  return GetTranslatorBlockImpl(bb)->continuation;
}

bool AnonImpl::IsBBStartedToBuild(BlockEntryInstr* bb) {
  auto impl = GetTranslatorBlockImpl(bb);
  if (!impl) return false;
  return impl->started;
}

bool AnonImpl::IsBBEndedToBuild(BlockEntryInstr* bb) {
  auto impl = GetTranslatorBlockImpl(bb);
  if (!impl) return false;
  return impl->ended;
}

void AnonImpl::StartBuild(BlockEntryInstr* bb, Output& output) {
  EnsureNativeBB(bb, output);
  GetTranslatorBlockImpl(bb)->StartBuild();
  output.positionToBBEnd(GetNativeBB(bb));
}
}  // namespace

struct IRTranslator::Impl : public AnonImpl {};

IRTranslator::IRTranslator(FlowGraph* flow_graph)
    : FlowGraphVisitor(flow_graph->reverse_postorder()),
      flow_graph_(flow_graph) {
  compiler_state_.reset(new CompilerState(
      String::Handle(flow_graph->zone(),
                     flow_graph->parsed_function().function().UserVisibleName())
          .ToCString()));
  liveness_analysis_.reset(new LivenessAnalysis(flow_graph));
  output_.reset(new Output(compiler_state()));
  // init parameter desc
  RegisterParameterDesc parameter_desc;
  LType tagged_type = output().repo().tagged_type;
  for (int i = 1; i <= flow_graph->num_direct_parameters(); ++i) {
    parameter_desc.emplace_back(-i, tagged_type);
  }
  // init output.
  output().initializeBuild(parameter_desc);
  impl_.reset(new Impl);
}

IRTranslator::~IRTranslator() {}

void IRTranslator::Translate() {
  liveness_analysis().Analyze();
  VisitBlocks();
}

void IRTranslator::VisitGraphEntry(GraphEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitJoinEntry(JoinEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitTargetEntry(TargetEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitFunctionEntry(FunctionEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitNativeEntry(NativeEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitOsrEntry(OsrEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitIndirectEntry(IndirectEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCatchBlockEntry(CatchBlockEntryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitPhi(PhiInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitRedefinition(RedefinitionInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitParameter(ParameterInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitNativeParameter(NativeParameterInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadIndexedUnsafe(LoadIndexedUnsafeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreIndexedUnsafe(StoreIndexedUnsafeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitTailCall(TailCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitParallelMove(ParallelMoveInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitPushArgument(PushArgumentInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitReturn(ReturnInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitNativeReturn(NativeReturnInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitThrow(ThrowInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitReThrow(ReThrowInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStop(StopInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitGoto(GotoInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitIndirectGoto(IndirectGotoInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBranch(BranchInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAssertAssignable(AssertAssignableInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAssertSubtype(AssertSubtypeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAssertBoolean(AssertBooleanInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitSpecialParameter(SpecialParameterInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitClosureCall(ClosureCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitFfiCall(FfiCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInstanceCall(InstanceCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitPolymorphicInstanceCall(
    PolymorphicInstanceCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStaticCall(StaticCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadLocal(LoadLocalInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDropTemps(DropTempsInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitMakeTemp(MakeTempInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreLocal(StoreLocalInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStrictCompare(StrictCompareInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitEqualityCompare(EqualityCompareInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitRelationalOp(RelationalOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitNativeCall(NativeCallInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDebugStepCheck(DebugStepCheckInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadIndexed(LoadIndexedInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadCodeUnits(LoadCodeUnitsInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreIndexed(StoreIndexedInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreInstanceField(StoreInstanceFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInitInstanceField(InitInstanceFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInitStaticField(InitStaticFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadStaticField(LoadStaticFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreStaticField(StoreStaticFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBooleanNegate(BooleanNegateInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInstanceOf(InstanceOfInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCreateArray(CreateArrayInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAllocateObject(AllocateObjectInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadField(LoadFieldInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadUntagged(LoadUntaggedInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStoreUntagged(StoreUntaggedInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitLoadClassId(LoadClassIdInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInstantiateType(InstantiateTypeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInstantiateTypeArguments(
    InstantiateTypeArgumentsInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAllocateContext(AllocateContextInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitAllocateUninitializedContext(
    AllocateUninitializedContextInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCloneContext(CloneContextInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBinarySmiOp(BinarySmiOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckedSmiComparison(CheckedSmiComparisonInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckedSmiOp(CheckedSmiOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBinaryInt32Op(BinaryInt32OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnarySmiOp(UnarySmiOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnaryDoubleOp(UnaryDoubleOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckStackOverflow(CheckStackOverflowInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitSmiToDouble(SmiToDoubleInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInt32ToDouble(Int32ToDoubleInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInt64ToDouble(Int64ToDoubleInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToInteger(DoubleToIntegerInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToSmi(DoubleToSmiInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToDouble(DoubleToDoubleInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDoubleToFloat(DoubleToFloatInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitFloatToDouble(FloatToDoubleInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckClass(CheckClassInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckClassId(CheckClassIdInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckSmi(CheckSmiInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckNull(CheckNullInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckCondition(CheckConditionInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitConstant(ConstantInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnboxedConstant(UnboxedConstantInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckEitherNonSmi(CheckEitherNonSmiInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBinaryDoubleOp(BinaryDoubleOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDoubleTestOp(DoubleTestOpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitMathUnary(MathUnaryInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitMathMinMax(MathMinMaxInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBox(BoxInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnbox(UnboxInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBoxInt64(BoxInt64Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnboxInt64(UnboxInt64Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCaseInsensitiveCompare(
    CaseInsensitiveCompareInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBinaryInt64Op(BinaryInt64OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitShiftInt64Op(ShiftInt64OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitSpeculativeShiftInt64Op(
    SpeculativeShiftInt64OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnaryInt64Op(UnaryInt64OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitCheckArrayBound(CheckArrayBoundInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitGenericCheckBound(GenericCheckBoundInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitConstraint(ConstraintInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStringToCharCode(StringToCharCodeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitOneByteStringFromCharCode(
    OneByteStringFromCharCodeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitStringInterpolate(StringInterpolateInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitInvokeMathCFunction(InvokeMathCFunctionInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitTruncDivMod(TruncDivModInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldClass(GuardFieldClassInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldLength(GuardFieldLengthInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitGuardFieldType(GuardFieldTypeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitIfThenElse(IfThenElseInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitMaterializeObject(MaterializeObjectInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitTestSmi(TestSmiInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitTestCids(TestCidsInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitExtractNthOutput(ExtractNthOutputInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBinaryUint32Op(BinaryUint32OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitShiftUint32Op(ShiftUint32OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitSpeculativeShiftUint32Op(
    SpeculativeShiftUint32OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnaryUint32Op(UnaryUint32OpInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBoxUint32(BoxUint32Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnboxUint32(UnboxUint32Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBoxInt32(BoxInt32Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnboxInt32(UnboxInt32Instr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitIntConverter(IntConverterInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitBitCast(BitCastInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitUnboxedWidthExtender(UnboxedWidthExtenderInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitDeoptimize(DeoptimizeInstr* instr) {
  UNREACHABLE();
}

void IRTranslator::VisitSimdOp(SimdOpInstr* instr) {
  UNREACHABLE();
}
}  // namespace dart_llvm
}  // namespace dart
#endif  // DART_ENABLE_LLVM_COMPILER
