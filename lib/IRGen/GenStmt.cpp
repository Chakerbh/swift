//===--- GenStmt.cpp - IR Generation for Statements -----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for Swift statements.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Expr.h"
#include "swift/AST/Stmt.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/ErrorHandling.h"
#include "Condition.h"
#include "Scope.h"
#include "GenType.h"
#include "IRGenFunction.h"
#include "JumpDest.h"
#include "LValue.h"

using namespace swift;
using namespace irgen;

void IRGenFunction::emitStmt(Stmt *S) {
  switch (S->getKind()) {
  case StmtKind::Error: assert(0 && "Invalid programs shouldn't get here");
  case StmtKind::Semi:
    // Nothing to do.
    return;

  case StmtKind::Assign:
    return emitAssignStmt(cast<AssignStmt>(S));

  case StmtKind::Brace:
    return emitBraceStmt(cast<BraceStmt>(S));

  case StmtKind::Return:
    return emitReturnStmt(cast<ReturnStmt>(S));

  case StmtKind::If:
    return emitIfStmt(cast<IfStmt>(S));

  case StmtKind::While:
    return emitWhileStmt(cast<WhileStmt>(S));
  }
  llvm_unreachable("bad statement kind!");
}

void IRGenFunction::emitBraceStmt(BraceStmt *BS) {
  // Enter a new scope.
  Scope BraceScope(*this);

  for (auto Elt : BS->getElements()) {
    assert(Builder.hasValidIP());

    if (Expr *E = Elt.dyn_cast<Expr*>()) {
      FullExpr scope(*this);
      emitIgnored(E);
    } else if (Stmt *S = Elt.dyn_cast<Stmt*>()) {
      emitStmt(S);

      // If we ever reach an unreachable point, stop emitting statements.
      // This will need revision if we ever add goto.
      if (!Builder.hasValidIP()) return;
    } else {
      emitLocal(Elt.get<Decl*>());
    }
  }
}

/// Emit an assignment statement.
void IRGenFunction::emitAssignStmt(AssignStmt *S) {
  const TypeInfo &type = getFragileTypeInfo(S->getSrc()->getType());
  LValue LV = emitLValue(S->getDest());
  emitAssign(S->getSrc(), LV, type);
}

void IRGenFunction::emitIfStmt(IfStmt *S) {
  Condition cond = emitCondition(S->getCond(), S->getElseStmt() != nullptr);
  if (cond.hasTrue()) {
    cond.enterTrue(*this);
    emitStmt(S->getThenStmt());
    cond.exitTrue(*this);
  }

  if (cond.hasFalse()) {
    assert(S->getElseStmt());
    cond.enterFalse(*this);
    emitStmt(S->getElseStmt());
    cond.exitFalse(*this);
  }

  cond.complete(*this);
}

void IRGenFunction::emitReturnStmt(ReturnStmt *S) {
  // The expression is evaluated in a full-expression context.
  FullExpr fullExpr(*this);

  // If this function takes no return value, ignore the result of the
  // expression.
  if (!ReturnSlot.isValid()) {
    emitIgnored(S->getResult());
  } else {
    const TypeInfo &resultType = getFragileTypeInfo(S->getResult()->getType());
    emitInit(S->getResult(), ReturnSlot, resultType);
  }

  // Leave the full-expression.
  fullExpr.pop();

  // In either case, branch to the return block.
  emitBranch(JumpDest(ReturnBB, Cleanups.stable_end()));
  Builder.ClearInsertionPoint();
}

void IRGenFunction::emitWhileStmt(WhileStmt *S) {
  // Create a new basic block and jump into it.
  llvm::BasicBlock *loopBB = createBasicBlock("while");
  Builder.CreateBr(loopBB);
  Builder.emitBlock(loopBB);

  // Evaluate the condition with the false edge leading directly
  // to the continuation block.
  Condition cond = emitCondition(S->getCond(), /*hasFalseCode*/ false);

  // If there's a true edge, emit the body in it.
  if (cond.hasTrue()) {
    cond.enterTrue(*this);
    emitStmt(S->getBody());
    if (Builder.hasValidIP()) {
      Builder.CreateBr(loopBB);
      Builder.ClearInsertionPoint();
    }
    cond.exitTrue(*this);
  }

  // Complete the conditional execution.
  cond.complete(*this);
}
