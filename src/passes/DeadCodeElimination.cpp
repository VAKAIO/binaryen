/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Removes dead, i.e. unreachable, code.
//
// We keep a record of when control flow is reachable. When it isn't, we
// kill (turn into unreachable). We then fold away entire unreachable
// expressions.
//
// When dead code causes an operation to not happen, like a store, a call
// or an add, we replace with a block with a list of what does happen.
// That isn't necessarily smaller, but blocks are friendlier to other
// optimizations: blocks can be merged and eliminated, and they clearly
// have no side effects.
//

#include <wasm.h>
#include <pass.h>
#include <ast_utils.h>
#include <wasm-builder.h>
#include <ast/block-utils.h>
#include <ast/type-updating.h>

namespace wasm {

struct DeadCodeElimination : public WalkerPass<PostWalker<DeadCodeElimination>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new DeadCodeElimination; }

  // as we remove code, we must keep the types of other nodes valid
  TypeUpdater typeUpdater;

  Expression* replaceCurrent(Expression* expression) {
    auto* old = getCurrent();
    WalkerPass<PostWalker<DeadCodeElimination>>::replaceCurrent(expression);
    // also update the type updater
    typeUpdater.noteReplacement(old, expression);
    return expression;
  }

  // whether the current code is actually reachable
  bool reachable;

  void doWalkFunction(Function* func) {
    reachable = true;
    typeUpdater.walk(func->body);
    walk(func->body);
  }

  std::set<Name> reachableBreaks;

  void addBreak(Name name) {
    // we normally have already reduced unreachable code into (unreachable)
    // nodes, so we would not get to this function at all anyhow, the breaking
    // instruction itself would be removed. However, an exception are things
    // like  (block (result i32) (call $x) (unreachable)) , which has type i32
    // despite not being exited.
    // TODO: optimize such cases
    if (reachable) {
      reachableBreaks.insert(name);
    }
  }

  // if a child exists and is unreachable, we can replace ourselves with it
  bool isDead(Expression* child) {
    return child && child->type == unreachable;
  }

  // a similar check, assumes the child exists
  bool isUnreachable(Expression* child) {
    return child->type == unreachable;
  }

  // things that stop control flow

  void visitBreak(Break* curr) {
    if (isDead(curr->value)) {
      // the condition is evaluated last, so if the value was unreachable, the whole thing is
      replaceCurrent(curr->value);
      return;
    }
    if (isDead(curr->condition)) {
      if (curr->value) {
        auto* block = getModule()->allocator.alloc<Block>();
        block->list.resize(2);
        block->list[0] = drop(curr->value);
        block->list[1] = curr->condition;
        // if we previously returned a value, then this block
        // must have the same type, so it fits in the ast
        // properly. it ends in an unreachable
        // anyhow, so that is ok.
        block->finalize(curr->type);
        replaceCurrent(block);
      } else {
        replaceCurrent(curr->condition);
      }
      return;
    }
    addBreak(curr->name);
    if (!curr->condition) {
      reachable = false;
    }
  }

  void visitSwitch(Switch* curr) {
    if (isDead(curr->value)) {
      replaceCurrent(curr->value);
      return;
    }
    if (isUnreachable(curr->condition)) {
      if (curr->value) {
        auto* block = getModule()->allocator.alloc<Block>();
        block->list.resize(2);
        block->list[0] = drop(curr->value);
        block->list[1] = curr->condition;
        block->finalize(curr->type);
        replaceCurrent(block);
      } else {
        replaceCurrent(curr->condition);
      }
      return;
    }
    for (auto target : curr->targets) {
      addBreak(target);
    }
    addBreak(curr->default_);
    reachable = false;
  }

  void visitReturn(Return* curr) {
    if (isDead(curr->value)) {
      replaceCurrent(curr->value);
      return;
    }
    reachable = false;
  }

  void visitUnreachable(Unreachable* curr) {
    reachable = false;
  }

  void visitBlock(Block* curr) {
    auto& list = curr->list;
    // if we are currently unreachable (before we take into account
    // breaks to the block) then a child may be unreachable, and we
    // can shorten
    if (!reachable && list.size() > 1) {
      // to do here: nothing to remove after it)
      for (Index i = 0; i < list.size() - 1; i++) {
        if (list[i]->type == unreachable) {
          list.resize(i + 1);
          break;
        }
      }
    }
    if (curr->name.is()) {
      reachable = reachable || reachableBreaks.count(curr->name);
      reachableBreaks.erase(curr->name);
    }
    if (list.size() == 1 && isUnreachable(list[0])) {
      replaceCurrent(BlockUtils::simplifyToContentsWithPossibleTypeChange(curr, this));
    } else {
      // the block may have had a type, but can now be unreachable, which allows more reduction outside
      typeUpdater.maybeUpdateTypeToUnreachable(curr);
    }
  }

  void visitLoop(Loop* curr) {
    if (curr->name.is()) {
      reachableBreaks.erase(curr->name);
    }
    if (isUnreachable(curr->body) && !BreakSeeker::has(curr->body, curr->name)) {
      replaceCurrent(curr->body);
      return;
    }
  }

  // ifs need special handling

  std::vector<bool> ifStack; // stack of reachable state, for forking and joining

  static void doAfterIfCondition(DeadCodeElimination* self, Expression** currp) {
    self->ifStack.push_back(self->reachable);
  }

  static void doAfterIfElseTrue(DeadCodeElimination* self, Expression** currp) {
    ASSERT_THROW((*currp)->cast<If>()->ifFalse);
    bool reachableBefore = self->ifStack.back();
    self->ifStack.pop_back();
    self->ifStack.push_back(self->reachable);
    self->reachable = reachableBefore;
  }

  void visitIf(If* curr) {
    // the ifStack has the branch that joins us, either from before if just an if, or the ifTrue if an if-else
    reachable = reachable || ifStack.back();
    ifStack.pop_back();
    if (isUnreachable(curr->condition)) {
      replaceCurrent(curr->condition);
    }
    // the if may have had a type, but can now be unreachable, which allows more reduction outside
    curr->finalize();
  }

  static void scan(DeadCodeElimination* self, Expression** currp) {
    if (!self->reachable) {
      // convert to an unreachable. do this without UB, even though we have no destructors on AST nodes
      #define DELEGATE(CLASS_TO_VISIT) { \
        self->typeUpdater.noteRecursiveRemoval(*currp); \
        ExpressionManipulator::convert<CLASS_TO_VISIT, Unreachable>(static_cast<CLASS_TO_VISIT*>(*currp)); \
        break; \
      }
      switch ((*currp)->_id) {
        case Expression::Id::BlockId: DELEGATE(Block);
        case Expression::Id::IfId: DELEGATE(If);
        case Expression::Id::LoopId: DELEGATE(Loop);
        case Expression::Id::BreakId: DELEGATE(Break);
        case Expression::Id::SwitchId: DELEGATE(Switch);
        case Expression::Id::CallId: DELEGATE(Call);
        case Expression::Id::CallImportId: DELEGATE(CallImport);
        case Expression::Id::CallIndirectId: DELEGATE(CallIndirect);
        case Expression::Id::GetLocalId: DELEGATE(GetLocal);
        case Expression::Id::SetLocalId: DELEGATE(SetLocal);
        case Expression::Id::GetGlobalId: DELEGATE(GetGlobal);
        case Expression::Id::SetGlobalId: DELEGATE(SetGlobal);
        case Expression::Id::LoadId: DELEGATE(Load);
        case Expression::Id::StoreId: DELEGATE(Store);
        case Expression::Id::ConstId: DELEGATE(Const);
        case Expression::Id::UnaryId: DELEGATE(Unary);
        case Expression::Id::BinaryId: DELEGATE(Binary);
        case Expression::Id::SelectId: DELEGATE(Select);
        case Expression::Id::DropId: DELEGATE(Drop);
        case Expression::Id::ReturnId: DELEGATE(Return);
        case Expression::Id::HostId: DELEGATE(Host);
        case Expression::Id::NopId: DELEGATE(Nop);
        case Expression::Id::UnreachableId: break;
        case Expression::Id::InvalidId:
        default: WASM_UNREACHABLE();
      }
      #undef DELEGATE
      return;
    }
    auto* curr =* currp;
    if (curr->is<If>()) {
      self->pushTask(DeadCodeElimination::doVisitIf, currp);
      if (curr->cast<If>()->ifFalse) {
        self->pushTask(DeadCodeElimination::scan, &curr->cast<If>()->ifFalse);
        self->pushTask(DeadCodeElimination::doAfterIfElseTrue, currp);
      }
      self->pushTask(DeadCodeElimination::scan, &curr->cast<If>()->ifTrue);
      self->pushTask(DeadCodeElimination::doAfterIfCondition, currp);
      self->pushTask(DeadCodeElimination::scan, &curr->cast<If>()->condition);
    } else {
      WalkerPass<PostWalker<DeadCodeElimination>>::scan(self, currp);
    }
  }

  // other things

  // we don't need to drop unreachable nodes
  Expression* drop(Expression* toDrop) {
    if (toDrop->type == unreachable) return toDrop;
    return Builder(*getModule()).makeDrop(toDrop);
  }

  template<typename T>
  Expression* handleCall(T* curr) {
    for (Index i = 0; i < curr->operands.size(); i++) {
      if (isUnreachable(curr->operands[i])) {
        if (i > 0) {
          auto* block = getModule()->allocator.alloc<Block>();
          Index newSize = i + 1;
          block->list.resize(newSize);
          Index j = 0;
          for (; j < newSize; j++) {
            block->list[j] = drop(curr->operands[j]);
          }
          block->finalize(curr->type);
          return replaceCurrent(block);
        } else {
          return replaceCurrent(curr->operands[i]);
        }
      }
    }
    return curr;
  }

  void visitCall(Call* curr) {
    handleCall(curr);
  }

  void visitCallImport(CallImport* curr) {
    handleCall(curr);
  }

  void visitCallIndirect(CallIndirect* curr) {
    if (handleCall(curr) != curr) return;
    if (isUnreachable(curr->target)) {
      auto* block = getModule()->allocator.alloc<Block>();
      for (auto* operand : curr->operands) {
        block->list.push_back(drop(operand));
      }
      block->list.push_back(curr->target);
      block->finalize(curr->type);
      replaceCurrent(block);
    }
  }

  void visitSetLocal(SetLocal* curr) {
    if (isUnreachable(curr->value)) {
      replaceCurrent(curr->value);
    }
  }

  void visitLoad(Load* curr) {
    if (isUnreachable(curr->ptr)) {
      replaceCurrent(curr->ptr);
    }
  }

  void visitStore(Store* curr) {
    if (isUnreachable(curr->ptr)) {
      replaceCurrent(curr->ptr);
      return;
    }
    if (isUnreachable(curr->value)) {
      auto* block = getModule()->allocator.alloc<Block>();
      block->list.resize(2);
      block->list[0] = drop(curr->ptr);
      block->list[1] = curr->value;
      block->finalize(curr->type);
      replaceCurrent(block);
    }
  }

  void visitUnary(Unary* curr) {
    if (isUnreachable(curr->value)) {
      replaceCurrent(curr->value);
    }
  }

  void visitBinary(Binary* curr) {
    if (isUnreachable(curr->left)) {
      replaceCurrent(curr->left);
      return;
    }
    if (isUnreachable(curr->right)) {
      auto* block = getModule()->allocator.alloc<Block>();
      block->list.resize(2);
      block->list[0] = drop(curr->left);
      block->list[1] = curr->right;
      block->finalize(curr->type);
      replaceCurrent(block);
    }
  }

  void visitSelect(Select* curr) {
    if (isUnreachable(curr->ifTrue)) {
      replaceCurrent(curr->ifTrue);
      return;
    }
    if (isUnreachable(curr->ifFalse)) {
      auto* block = getModule()->allocator.alloc<Block>();
      block->list.resize(2);
      block->list[0] = drop(curr->ifTrue);
      block->list[1] = curr->ifFalse;
      block->finalize(curr->type);
      replaceCurrent(block);
      return;
    }
    if (isUnreachable(curr->condition)) {
      auto* block = getModule()->allocator.alloc<Block>();
      block->list.resize(3);
      block->list[0] = drop(curr->ifTrue);
      block->list[1] = drop(curr->ifFalse);
      block->list[2] = curr->condition;
      block->finalize(curr->type);
      replaceCurrent(block);
      return;
    }
  }

  void visitDrop(Drop* curr) {
    if (isUnreachable(curr->value)) {
      replaceCurrent(curr->value);
    }
  }

  void visitHost(Host* curr) {
    handleCall(curr);
  }

  void visitFunction(Function* curr) {
    ASSERT_THROW(reachableBreaks.size() == 0);
  }
};

Pass *createDeadCodeEliminationPass() {
  return new DeadCodeElimination();
}

} // namespace wasm

