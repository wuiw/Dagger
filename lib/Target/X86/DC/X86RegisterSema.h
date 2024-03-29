//===-- X86RegisterSema.h - X86 DC Register Semantics -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_DC_X86REGISTERSEMA_H
#define LLVM_LIB_TARGET_X86_DC_X86REGISTERSEMA_H

#include "X86InstrInfo.h"
#include "llvm/DC/DCRegisterSema.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {
namespace X86 {
  enum StatusFlag {
    CF = 0,
    PF = 2,
    AF = 4,
    ZF = 6,
    SF = 7,
    OF = 11,
    MAX_FLAGS = OF
  };
} // end namespace X86

class Value;
class Function;

class X86RegisterSema : public DCRegisterSema {
public:
  X86RegisterSema(const MCRegisterInfo &MRI,
                  const MCInstrInfo &MII);

  // Update EFLAGS with the result of comparing LHS to RHS.
  // If they are float values, this is an unordered comparison (UCOMI).
  Value *getEFLAGSforCMP(Value *LHS, Value *RHS);

  void updateEFLAGS(Value *Def, bool IsINCDEC = false);
  Value *testCondCode(unsigned CondCode);

  void insertInitRegSetCode(Function *InitFn) override;
  void insertFiniRegSetCode(Function *FiniFn) override;

private:
  bool doesSubRegIndexClearSuper(unsigned SubRegIdx) const override;

  void onRegisterGet(unsigned RegNo) override;
  void onRegisterSet(unsigned RegNo, Value *RegVal) override;

  void clearCCSF();

  void FinalizeBasicBlock() override;
  void FinalizeFunction(BasicBlock *ExitBB) override;

  // Valid only inside a Basic Block.
  // This is set to the last definition that should update EFLAGS.
  // This is used to lazily compute each status flag when EFLAGS is actually
  // needed.
  Value *LastEFLAGSChangingDef;
  Value *LastEFLAGSDef;
  // Whether the last EFLAGS def was an INC/DEC, and shouldn't update CF.
  bool LastEFLAGSDefWasPartialINCDEC;
  SmallVector<Value *, 16> SFVals;
  SmallVector<unsigned, 16> SFAssignments;
  SmallVector<Value *, 16> CCVals;
  SmallVector<unsigned, 16> CCAssignments;

  void setSF(X86::StatusFlag SF, Value *Val);
  Value *getSF(X86::StatusFlag SF);

  void setCC(X86::CondCode CC, Value *Val);
  Value *getCC(X86::CondCode CC);

  Value *computeEFLAGSForDef(Value *Def, Value *OldEFLAGS,
                             bool DontUpdateCF = false);
  Value *createEFLAGSFromSFs(Value *OldEFLAGS);

  void insertExternalWrapperAsm(BasicBlock *WrapperBB,
                                Function *ExtFn) override;
};

} // end namespace llvm

#endif
