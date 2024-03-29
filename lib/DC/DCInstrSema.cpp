//===-- lib/DC/DCInstrSema.cpp - DC Instruction Semantics -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DC/DCInstrSema.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/DC/DCRegisterSema.h"
#include "llvm/DC/DCTranslatedInstTracker.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCAnalysis/MCFunction.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

#define DEBUG_TYPE "dc-sema"

static cl::opt<bool>
EnableRegSetDiff("enable-dc-regset-diff", cl::desc(""), cl::init(false));

static cl::opt<bool>
EnableInstAddrSave("enable-dc-pc-save", cl::desc(""), cl::init(false));

DCInstrSema::DCInstrSema(const unsigned *OpcodeToSemaIdx,
                         const unsigned *SemanticsArray,
                         const uint64_t *ConstantArray, DCRegisterSema &DRS)
    : OpcodeToSemaIdx(OpcodeToSemaIdx), SemanticsArray(SemanticsArray),
      ConstantArray(ConstantArray), DynTranslateAtCBPtr(0),
      Ctx(0), TheModule(0), DRS(DRS), FuncType(0),
      TheFunction(0), TheMCFunction(0), BBByAddr(), ExitBB(0), CallBBs(),
      TheBB(0), TheMCBB(0), Builder(), Idx(0), ResEVT(), Opcode(0), Vals(),
      CurrentInst(0) {}

DCInstrSema::~DCInstrSema() {}

Function *DCInstrSema::FinalizeFunction() {
  for (auto *CallBB : CallBBs) {
    assert(CallBB->size() == 2 &&
           "Call basic block has wrong number of instructions!");
    auto CallI = CallBB->begin();
    DRS.saveAllLocalRegs(CallBB, CallI);
    DRS.restoreLocalRegs(CallBB, ++CallI);
  }
  DRS.FinalizeFunction(ExitBB);
  CallBBs.clear();
  BBByAddr.clear();
  Function *Fn = TheFunction;
  TheFunction = nullptr;
  TheMCFunction = nullptr;
  return Fn;
}

void DCInstrSema::FinalizeBasicBlock() {
  if (!TheBB->getTerminator())
    BranchInst::Create(getOrCreateBasicBlock(getBasicBlockEndAddress()),
                       TheBB);
  DRS.FinalizeBasicBlock();
  TheBB = nullptr;
  TheMCBB = nullptr;
}

Function *DCInstrSema::getOrCreateMainFunction(Function *EntryFn) {
  Type *MainArgs[] = { Builder->getInt32Ty(),
                       Builder->getInt8PtrTy()->getPointerTo() };
  Function *IRMain = cast<Function>(
      TheModule->getOrInsertFunction(
          "main", FunctionType::get(Builder->getInt32Ty(), MainArgs, false)));

  if (!IRMain->empty())
    return IRMain;

  IRBuilderBase::InsertPointGuard IPG(*Builder);
  Builder->SetInsertPoint(BasicBlock::Create(*Ctx, "", IRMain));

  Value *Regset = Builder->CreateAlloca(DRS.getRegSetType());

  // allocate a small local array to serve as a test stack.
  Value *StackPtr =
      Builder->CreateAlloca(ArrayType::get(Builder->getInt8Ty(), 8192));
  Value *StackSize = Builder->getInt32(8192);
  Value *Idx[2] = { Builder->getInt32(0), Builder->getInt32(0) };
  StackPtr = Builder->CreateInBoundsGEP(StackPtr, Idx);

  Function::arg_iterator ArgI = IRMain->getArgumentList().begin();
  Value *ArgC = ArgI++;
  Value *ArgV = ArgI++;

  Function *InitFn = getOrCreateInitRegSetFunction();
  Function *FiniFn = getOrCreateFiniRegSetFunction();

  Builder->CreateCall5(InitFn, Regset, StackPtr, StackSize, ArgC, ArgV);
  Builder->CreateCall(EntryFn, Regset);
  Builder->CreateRet(Builder->CreateCall(FiniFn, Regset));
  return IRMain;
}

Function *DCInstrSema::getOrCreateInitRegSetFunction() {
  StructType *RegSetType = DRS.getRegSetType();

  Type *InitArgs[] = {RegSetType->getPointerTo(), Builder->getInt8PtrTy(),
                      Builder->getInt32Ty(), Builder->getInt32Ty(),
                      Builder->getInt8PtrTy()->getPointerTo()};
  Function *InitFn = cast<Function>(TheModule->getOrInsertFunction(
      "main_init_regset",
      FunctionType::get(Builder->getVoidTy(), InitArgs, false)));
  if (InitFn->empty())
    DRS.insertInitRegSetCode(InitFn);

  // If we need to diff regsets, now's a good time to insert the function.
  // FIXME: bad hijack
  if (EnableRegSetDiff)
    DRS.getOrCreateRegSetDiffFunction(/*Definition=*/ true);

  return InitFn;
}

Function *DCInstrSema::getOrCreateFiniRegSetFunction() {
  StructType *RegSetType = DRS.getRegSetType();

  Type *FiniArgs[] = {RegSetType->getPointerTo()};
  Function *FiniFn = cast<Function>(TheModule->getOrInsertFunction(
      "main_fini_regset",
      FunctionType::get(Builder->getInt32Ty(), FiniArgs, false)));

  if (FiniFn->empty())
    DRS.insertFiniRegSetCode(FiniFn);
  return FiniFn;
}

void DCInstrSema::createExternalWrapperFunction(uint64_t Addr, StringRef Name) {
  Function *ExtFn =
      cast<Function>(TheModule->getOrInsertFunction(
                         Name, FunctionType::get(Builder->getVoidTy(), false)));

  Function *Fn = getFunction(Addr);
  if (!Fn->isDeclaration())
    return;

  BasicBlock *BB = BasicBlock::Create(*Ctx, "", Fn);
  DRS.insertExternalWrapperAsm(BB, ExtFn);
  ReturnInst::Create(*Ctx, BB);
}

void DCInstrSema::createExternalTailCallBB(uint64_t Addr) {
  // First create a basic block for the tail call.
  SwitchToBasicBlock(Addr);
  // Now do the call to that function.
  insertCallBB(getFunction(Addr));
  // Finally, return directly, bypassing the ExitBB.
  Builder->CreateRetVoid();
}

void DCInstrSema::SwitchToModule(Module *M) {
  TheModule = M;
  Ctx = &TheModule->getContext();
  DRS.SwitchToModule(TheModule);
  FuncType = FunctionType::get(Type::getVoidTy(*Ctx),
                               DRS.getRegSetType()->getPointerTo(), false);
  Builder.reset(new DCIRBuilder(*Ctx));
}

extern "C" uintptr_t __llvm_dc_current_fn = 0;
extern "C" uintptr_t __llvm_dc_current_bb = 0;
extern "C" uintptr_t __llvm_dc_current_instr = 0;

void DCInstrSema::SwitchToFunction(const MCFunction *MCFN) {
  assert(!MCFN->empty() && "Trying to translate empty MC function");
  const uint64_t StartAddr = MCFN->getEntryBlock()->getStartAddr();

  TheFunction = getFunction(StartAddr);
  TheFunction->setDoesNotAlias(1);
  TheFunction->setDoesNotCapture(1);

  // Create the entry and exit basic blocks.
  TheBB =
      BasicBlock::Create(*Ctx, "entry_fn_" + utohexstr(StartAddr), TheFunction);
  ExitBB =
      BasicBlock::Create(*Ctx, "exit_fn_" + utohexstr(StartAddr), TheFunction);

  // From now on we insert in the entry basic block.
  Builder->SetInsertPoint(TheBB);

  if (EnableRegSetDiff) {
    Value *SavedRegSet = Builder->CreateAlloca(DRS.getRegSetType());
    Value *RegSetArg = &TheFunction->getArgumentList().front();

    // First, save the previous regset in the entry block.
    Builder->CreateStore(Builder->CreateLoad(RegSetArg), SavedRegSet);

    // Second, insert a call to the diff function, in a separate exit block.
    // Move the return to that block, and branch to it from ExitBB.
    BasicBlock *DiffExitBB = BasicBlock::Create(
        *Ctx, "diff_exit_fn_" + utohexstr(StartAddr), TheFunction);

    DCIRBuilder ExitBBBuilder(DiffExitBB);

    Value *FnAddr = ExitBBBuilder.CreateIntToPtr(
        ExitBBBuilder.getInt64(reinterpret_cast<uint64_t>(StartAddr)),
        ExitBBBuilder.getInt8PtrTy());

    ExitBBBuilder.CreateCall3(DRS.getOrCreateRegSetDiffFunction(), FnAddr,
                              SavedRegSet, RegSetArg);
    ReturnInst::Create(*Ctx, DiffExitBB);
    BranchInst::Create(DiffExitBB, ExitBB);
  } else {
    // Create a ret void in the exit basic block.
    ReturnInst::Create(*Ctx, ExitBB);
  }

  // Create a br from the entry basic block to the first basic block, at StartAddr.
  Builder->CreateBr(getOrCreateBasicBlock(StartAddr));

  DRS.SwitchToFunction(TheFunction);
}

void DCInstrSema::prepareBasicBlockForInsertion(BasicBlock *BB) {
  assert((BB->size() == 2 && isa<UnreachableInst>(++BB->begin())) &&
         "Several BBs at the same address?");
  BB->begin()->eraseFromParent();
  BB->begin()->eraseFromParent();
}

void DCInstrSema::SwitchToBasicBlock(const MCBasicBlock *MCBB) {
  TheMCBB = MCBB;
  SwitchToBasicBlock(getBasicBlockStartAddress());
}

void DCInstrSema::SwitchToBasicBlock(uint64_t BeginAddr) {
  TheBB = getOrCreateBasicBlock(BeginAddr);
  prepareBasicBlockForInsertion(TheBB);

  Builder->SetInsertPoint(TheBB);

  DRS.SwitchToBasicBlock(TheBB);
  // FIXME: we need to keep the unreachable+trap when the basic block is 0-inst.

  // The PC at the start of the basic block is known, just set it.
  unsigned PC = DRS.MRI.getProgramCounter();
  setReg(PC, ConstantInt::get(DRS.getRegType(PC), BeginAddr));
}

uint64_t DCInstrSema::getBasicBlockStartAddress() const {
  assert(TheMCBB && "Getting start address without an MC BasicBlock");
  return TheMCBB->getStartAddr();
}

uint64_t DCInstrSema::getBasicBlockEndAddress() const {
  assert(TheMCBB && "Getting end address without an MC BasicBlock");
  return TheMCBB->getEndAddr();
}

Function *DCInstrSema::getFunction(uint64_t Addr) {
  std::string Name = "fn_" + utohexstr(Addr);
  TheModule->getOrInsertFunction(Name, FuncType);
  return TheModule->getFunction(Name);
}

BasicBlock *DCInstrSema::getOrCreateBasicBlock(uint64_t Addr) {
  BasicBlock *&BB = BBByAddr[Addr];
  if (!BB) {
    BB = BasicBlock::Create(*Ctx, "bb_" + utohexstr(Addr), TheFunction);
    DCIRBuilder BBBuilder(BB);
    BBBuilder
        .CreateCall(Intrinsic::getDeclaration(TheModule, Intrinsic::trap));
    BBBuilder.CreateUnreachable();
  }
  return BB;
}

BasicBlock *DCInstrSema::insertCallBB(Value *Target) {
  BasicBlock *CallBB =
      BasicBlock::Create(*Ctx, TheBB->getName() + "_call", TheFunction);
  Value *RegSetArg = &TheFunction->getArgumentList().front();
  DCIRBuilder CallBuilder(CallBB);
  CallBuilder.CreateCall(Target, RegSetArg);
  Builder->CreateBr(CallBB);
  assert(Builder->GetInsertPoint() == TheBB->end() &&
         "Call basic blocks can't be inserted at the middle of a basic block!");
  StringRef BBName = TheBB->getName();
  BBName = BBName.substr(0, BBName.find_first_of("_c"));
  TheBB = BasicBlock::Create(
      *Ctx, BBName + "_c" + utohexstr(CurrentInst->Address), TheFunction);
  DRS.FinalizeBasicBlock();
  DRS.SwitchToBasicBlock(TheBB);
  Builder->SetInsertPoint(TheBB);
  CallBuilder.CreateBr(TheBB);
  CallBBs.push_back(CallBB);
  // FIXME: Insert return address checking, to unwind back to the translator if
  // the call returned to an unexpected address.
  return CallBB;
}

Value *DCInstrSema::insertTranslateAt(Value *OrigTarget) {
  void* CBPtr = DynTranslateAtCBPtr;
  if (!CBPtr) {
    // If we don't have access to the dynamic translate_at function, jump to
    // some bad address.
    // FIXME: A better target would be a dedicated IR function with just trap+
    // unreachable, which would make it possible to debug more easily.
    // FIXME: We should be able generate a table with all possible call targets
    // from the symbol table.
    CBPtr = reinterpret_cast<void*>(0xDEAD);
  }

  FunctionType *CallbackType = FunctionType::get(
      FuncType->getPointerTo(), Builder->getInt8PtrTy(), false);
  return Builder->CreateCall(
      DRS.getCallTargetForExtFn(CallbackType, CBPtr),
      Builder->CreateIntToPtr(OrigTarget, Builder->getInt8PtrTy()));
}

void DCInstrSema::insertCall(Value *CallTarget) {
  if (ConstantInt *CI = dyn_cast<ConstantInt>(CallTarget)) {
    uint64_t Target = CI->getValue().getZExtValue();
    CallTarget = getFunction(Target);
  } else {
    CallTarget = insertTranslateAt(CallTarget);
  }
  insertCallBB(CallTarget);
}

void DCInstrSema::translateBinOp(Instruction::BinaryOps Opc) {
  Value *V1 = getNextOperand();
  Value *V2 = getNextOperand();
  if (Instruction::isShift(Opc) && V2->getType() != V1->getType())
    V2 = Builder->CreateZExt(V2, V1->getType());
  registerResult(Builder->CreateBinOp(Opc, V1, V2));
}

void DCInstrSema::translateCastOp(Instruction::CastOps Opc) {
  Type *ResType = ResEVT.getTypeForEVT(*Ctx);
  Value *Val = getNextOperand();
  registerResult(Builder->CreateCast(Opc, Val, ResType));
}

bool
DCInstrSema::translateInst(const MCDecodedInst &DecodedInst,
                           DCTranslatedInst &TranslatedInst) {
  CurrentInst = &DecodedInst;
  CurrentTInst = &TranslatedInst;
  DRS.SwitchToInst(DecodedInst);

  if (EnableInstAddrSave) {
    ConstantInt *CurIVal =
        Builder->getInt64(reinterpret_cast<uint64_t>(CurrentInst->Address));
    Value *CurIPtr = ConstantExpr::getIntToPtr(
        Builder->getInt64(reinterpret_cast<uint64_t>(&__llvm_dc_current_instr)),
        Builder->getInt64Ty()->getPointerTo());
    Builder->CreateStore(CurIVal, CurIPtr, true);
  }

  Idx = OpcodeToSemaIdx[CurrentInst->Inst.getOpcode()];
  if (!translateTargetInst()) {
    if (Idx == 0)
      return false;

    {
      // Increment the PC before anything.
      Value *OldPC = getReg(DRS.MRI.getProgramCounter());
      setReg(DRS.MRI.getProgramCounter(),
             Builder->CreateAdd(
                 OldPC, ConstantInt::get(OldPC->getType(), CurrentInst->Size)));
    }

    while ((Opcode = Next()) != DCINS::END_OF_INSTRUCTION)
      translateOpcode(Opcode);
  }

  Vals.clear();
  CurrentInst = nullptr;
  CurrentTInst = nullptr;
  return true;
}

void DCInstrSema::translateOpcode(unsigned Opcode) {
  ResEVT = NextVT();
  if (Opcode >= ISD::BUILTIN_OP_END && Opcode < DCINS::DC_OPCODE_START) {
    translateTargetOpcode();
    return;
  }
  switch(Opcode) {
  case ISD::ADD  : translateBinOp(Instruction::Add ); break;
  case ISD::FADD : translateBinOp(Instruction::FAdd); break;
  case ISD::SUB  : translateBinOp(Instruction::Sub ); break;
  case ISD::FSUB : translateBinOp(Instruction::FSub); break;
  case ISD::MUL  : translateBinOp(Instruction::Mul ); break;
  case ISD::FMUL : translateBinOp(Instruction::FMul); break;
  case ISD::UDIV : translateBinOp(Instruction::UDiv); break;
  case ISD::SDIV : translateBinOp(Instruction::SDiv); break;
  case ISD::FDIV : translateBinOp(Instruction::FDiv); break;
  case ISD::UREM : translateBinOp(Instruction::URem); break;
  case ISD::SREM : translateBinOp(Instruction::SRem); break;
  case ISD::FREM : translateBinOp(Instruction::FRem); break;
  case ISD::SHL  : translateBinOp(Instruction::Shl ); break;
  case ISD::SRL  : translateBinOp(Instruction::LShr); break;
  case ISD::SRA  : translateBinOp(Instruction::AShr); break;
  case ISD::AND  : translateBinOp(Instruction::And ); break;
  case ISD::OR   : translateBinOp(Instruction::Or  ); break;
  case ISD::XOR  : translateBinOp(Instruction::Xor ); break;

  case ISD::TRUNCATE    : translateCastOp(Instruction::Trunc   ); break;
  case ISD::BITCAST     : translateCastOp(Instruction::BitCast ); break;
  case ISD::ZERO_EXTEND : translateCastOp(Instruction::ZExt    ); break;
  case ISD::SIGN_EXTEND : translateCastOp(Instruction::SExt    ); break;
  case ISD::FP_TO_UINT  : translateCastOp(Instruction::FPToUI  ); break;
  case ISD::FP_TO_SINT  : translateCastOp(Instruction::FPToSI  ); break;
  case ISD::UINT_TO_FP  : translateCastOp(Instruction::UIToFP  ); break;
  case ISD::SINT_TO_FP  : translateCastOp(Instruction::SIToFP  ); break;
  case ISD::FP_ROUND    : translateCastOp(Instruction::FPTrunc ); break;
  case ISD::FP_EXTEND   : translateCastOp(Instruction::FPExt   ); break;

  case ISD::FSQRT: {
    Value *V = getNextOperand();
    registerResult(
        Builder->CreateCall(Intrinsic::getDeclaration(
                                TheModule, Intrinsic::sqrt, V->getType()),
                            V));
    break;
  }

  case ISD::SCALAR_TO_VECTOR: {
    Type *ResType = ResEVT.getTypeForEVT(*Ctx);
    Type *ResEltType = ResType->getVectorElementType();
    Value *NullVect = Constant::getNullValue(ResType);
    Value *Val = getNextOperand();
    if (Val->getType()->isFloatingPointTy())
      Val = Builder->CreateFPCast(Val, ResEltType);
    else
      Val = Builder->CreateZExtOrBitCast(Val, ResEltType);
    registerResult(
        Builder->CreateInsertElement(NullVect, Val, Builder->getInt32(0)));
    break;
  }

  case ISD::SMUL_LOHI: {
    EVT Re2EVT = NextVT();
    IntegerType *LoResType = cast<IntegerType>(ResEVT.getTypeForEVT(*Ctx));
    IntegerType *HiResType = cast<IntegerType>(Re2EVT.getTypeForEVT(*Ctx));
    IntegerType *ResType = IntegerType::get(
        *Ctx, LoResType->getBitWidth() + HiResType->getBitWidth());
    Value *Op1 = getNextOperand(), *Op2 = getNextOperand();
    Value *Full = Builder->CreateMul(Builder->CreateSExt(Op1, ResType),
                                     Builder->CreateSExt(Op2, ResType));
    registerResult(Builder->CreateTrunc(Full, LoResType));
    registerResult(
        Builder->CreateTrunc(
            Builder->CreateLShr(Full, LoResType->getBitWidth()), HiResType));
    break;
  }
  case ISD::UMUL_LOHI: {
    EVT Re2EVT = NextVT();
    IntegerType *LoResType = cast<IntegerType>(ResEVT.getTypeForEVT(*Ctx));
    IntegerType *HiResType = cast<IntegerType>(Re2EVT.getTypeForEVT(*Ctx));
    IntegerType *ResType = IntegerType::get(
        *Ctx, LoResType->getBitWidth() + HiResType->getBitWidth());
    Value *Op1 = getNextOperand(), *Op2 = getNextOperand();
    Value *Full = Builder->CreateMul(Builder->CreateZExt(Op1, ResType),
                                     Builder->CreateZExt(Op2, ResType));
    registerResult(Builder->CreateTrunc(Full, LoResType));
    registerResult(
        Builder->CreateTrunc(
            Builder->CreateLShr(Full, LoResType->getBitWidth()), HiResType));
    break;
  }
  case ISD::LOAD: {
    Type *ResType = ResEVT.getTypeForEVT(*Ctx);
    Value *Ptr = getNextOperand();
    if (!Ptr->getType()->isPointerTy())
      Ptr = Builder->CreateIntToPtr(Ptr, ResType->getPointerTo());
    assert(Ptr->getType()->getPointerElementType() == ResType &&
           "Mismatch between a LOAD's address operand and return type!");
    registerResult(Builder->CreateLoad(Ptr));
    break;
  }
  case ISD::STORE: {
    Value *Val = getNextOperand();
    Value *Ptr = getNextOperand();
    Type *ValPtrTy = Val->getType()->getPointerTo();
    Type *PtrTy = Ptr->getType();
    if (!PtrTy->isPointerTy())
      Ptr = Builder->CreateIntToPtr(Ptr, ValPtrTy);
    else if (PtrTy != ValPtrTy)
      Ptr = Builder->CreateBitCast(Ptr, ValPtrTy);
    Builder->CreateStore(Val, Ptr);
    break;
  }
  case ISD::BRIND: {
    Value *Op1 = getNextOperand();
    setReg(DRS.MRI.getProgramCounter(), Op1);
    insertCall(Op1);
    Builder->CreateBr(ExitBB);
    break;
  }
  case ISD::BR: {
    Value *Op1 = getNextOperand();
    uint64_t Target = cast<ConstantInt>(Op1)->getValue().getZExtValue();
    setReg(DRS.MRI.getProgramCounter(), Op1);
    Builder->CreateBr(getOrCreateBasicBlock(Target));
    break;
  }
  case ISD::TRAP: {
    Builder->CreateCall(Intrinsic::getDeclaration(TheModule, Intrinsic::trap));
    break;
  }
  case DCINS::PUT_RC: {
    unsigned MIOperandNo = Next();
    unsigned RegNo = getRegOp(MIOperandNo);
    Value *Res = getNextOperand();
    Type *RegType = DRS.getRegType(RegNo);
    if (Res->getType()->isPointerTy())
      Res = Builder->CreatePtrToInt(Res, RegType);
    if (!Res->getType()->isIntegerTy())
      Res = Builder->CreateBitCast(
          Res,
          IntegerType::get(*Ctx, Res->getType()->getPrimitiveSizeInBits()));
    if (Res->getType()->getPrimitiveSizeInBits() <
        RegType->getPrimitiveSizeInBits())
      Res = DRS.insertBitsInValue(getReg(RegNo), Res);
    assert(Res->getType() == RegType);
    setReg(RegNo, Res);
    CurrentTInst->addRegOpDef(MIOperandNo, Res);
    break;
  }
  case DCINS::PUT_REG: {
    unsigned RegNo = Next();
    Value *Res = getNextOperand();
    setReg(RegNo, Res);
    CurrentTInst->addImpDef(RegNo, Res);
    break;
  }
  case DCINS::GET_RC: {
    unsigned MIOperandNo = Next();
    Type *ResType = ResEVT.getTypeForEVT(*Ctx);
    Value *Reg = getReg(getRegOp(MIOperandNo));
    if (ResType->getPrimitiveSizeInBits() <
        Reg->getType()->getPrimitiveSizeInBits())
      Reg = Builder->CreateTrunc(
          Reg, IntegerType::get(*Ctx, ResType->getPrimitiveSizeInBits()));
    if (!ResType->isIntegerTy())
      Reg = Builder->CreateBitCast(Reg, ResType);
    registerResult(Reg);
    CurrentTInst->addRegOpUse(MIOperandNo, Reg);
    break;
  }
  case DCINS::GET_REG: {
    unsigned RegNo = Next();
    Value *RegVal = getReg(RegNo);
    registerResult(RegVal);
    CurrentTInst->addImpUse(RegNo, RegVal);
    break;
  }
  case DCINS::CUSTOM_OP: {
    unsigned OperandType = Next(), MIOperandNo = Next();
    translateOperand(OperandType, MIOperandNo);
    CurrentTInst->addOpUse(MIOperandNo, OperandType, Vals.back());
    break;
  }
  case DCINS::CONSTANT_OP: {
    unsigned MIOperandNo = Next();
    Type *ResType = ResEVT.getTypeForEVT(*Ctx);
    Value *Cst =
        ConstantInt::get(cast<IntegerType>(ResType), getImmOp(MIOperandNo));
    registerResult(Cst);
    CurrentTInst->addImmOpUse(MIOperandNo, Cst);
    break;
  }
  case DCINS::MOV_CONSTANT: {
    uint64_t ValIdx = Next();
    Type *ResType = nullptr;
    if (ResEVT.getSimpleVT() == MVT::iPTR)
      // FIXME: what should we do here? Maybe use DL's intptr type?
      ResType = Builder->getInt64Ty();
    else
      ResType = ResEVT.getTypeForEVT(*Ctx);
    registerResult(ConstantInt::get(ResType, ConstantArray[ValIdx]));
    break;
  }
  case DCINS::IMPLICIT: {
    translateImplicit(Next());
    break;
  }
  case ISD::INTRINSIC_VOID: {
    Value *IndexV = getNextOperand();
    // FIXME: the intrinsics sdnodes have variable numbers of arguments.
    // FIXME: handle overloaded intrinsics, but how?
    if (ConstantInt *IndexCI = dyn_cast<ConstantInt>(IndexV)) {
      uint64_t IntID = IndexCI->getZExtValue();
      Value *IntDecl =
          Intrinsic::getDeclaration(TheModule, Intrinsic::ID(IntID));
      registerResult(Builder->CreateCall(IntDecl));
    } else {
      llvm_unreachable("Unable to translate non-constant intrinsic ID");
    }
    break;
  }
  case ISD::BSWAP: {
    Type *ResType = ResEVT.getTypeForEVT(*Ctx);
    Value *Op = getNextOperand();
    Value *IntDecl =
          Intrinsic::getDeclaration(TheModule, Intrinsic::bswap, ResType);
    registerResult(Builder->CreateCall(IntDecl, Op));
    break;
  }
  default:
    llvm_unreachable(
        ("Unknown opcode found in semantics: " + utostr(Opcode)).c_str());
  }
}

void DCInstrSema::translateOperand(unsigned OperandType, unsigned MIOperandNo) {
  // FIXME: We don't have target-independent operand types yet.
  translateCustomOperand(OperandType, MIOperandNo);
}
