// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/heap/pointer_block.h"

#include "platform/assert.h"
#include "vm/isolate.h"
#include "vm/lockers.h"
#include "vm/thread.h"

namespace dart {

template <int BlockSize>
typename BlockStack<BlockSize>::EmptyShard*
    BlockStack<BlockSize>::empty_shards_ = nullptr;

template <int BlockSize>
void BlockStack<BlockSize>::Init() {
  if (empty_shards_ == nullptr) {
    empty_shards_ = new EmptyShard[kNumEmptyShards];
  }
}

template <int BlockSize>
void BlockStack<BlockSize>::Cleanup() {
  for (intptr_t i = 0; i < kNumEmptyShards; i++) {
    EmptyShard* shard = &empty_shards_[i];
    MutexLocker ml(&shard->mutex);
    while (!shard->list.IsEmpty()) {
      delete shard->list.Pop();
    }
  }
}

static intptr_t CurrentThreadShardIndex() {
  static RelaxedAtomic<intptr_t> next_index = 0;
  static thread_local intptr_t index = -1;
  if (index < 0) {
    index = next_index.fetch_add(1);
  }
  return index;
}

template <int BlockSize>
typename BlockStack<BlockSize>::EmptyShard*
BlockStack<BlockSize>::CurrentEmptyShard() {
  return &empty_shards_[CurrentThreadShardIndex() % kNumEmptyShards];
}

template <int BlockSize>
void BlockStack<BlockSize>::PushEmptyBlocksLocked(EmptyShard* shard,
                                                  Block* blocks) {
  DEBUG_ASSERT(shard->mutex.IsOwnedByCurrentThread());
  while (blocks != nullptr) {
    Block* next = blocks->next_;
    blocks->Reset();
    if (shard->list.length() < kMaxEmptyPerShard) {
      shard->list.Push(blocks);
    } else {
      delete blocks;
    }
    blocks = next;
  }
}

template <int BlockSize>
BlockStack<BlockSize>::BlockStack() : monitor_() {}

template <int BlockSize>
BlockStack<BlockSize>::~BlockStack() {
  Reset();
}

template <int BlockSize>
void BlockStack<BlockSize>::Reset() {
  MonitorLocker local_mutex_locker(&monitor_);
  {
    // Empty all blocks and move them to the global cache.
    EmptyShard* shard = CurrentEmptyShard();
    MutexLocker shard_locker(&shard->mutex);
    PushEmptyBlocksLocked(shard, full_.PopAll());
    PushEmptyBlocksLocked(shard, partial_.PopAll());
  }
}

template <int BlockSize>
typename BlockStack<BlockSize>::Block* BlockStack<BlockSize>::PopAll() {
  MonitorLocker ml(&monitor_);
  while (!partial_.IsEmpty()) {
    full_.Push(partial_.Pop());
  }
  return full_.PopAll();
}

template <int BlockSize>
void BlockStack<BlockSize>::PushAll(Block* block) {
  while (block != nullptr) {
    Block* next = block->next();
    block->set_next(nullptr);
    PushBlockImpl(block);
    block = next;
  }
}

template <int BlockSize>
void BlockStack<BlockSize>::PushBlockImpl(Block* block) {
  ASSERT(block->next() == nullptr);  // Should be just a single block.
  if (block->IsFull()) {
    MonitorLocker ml(&monitor_);
    bool was_empty = IsEmptyLocked();
    full_.Push(block);
    if (was_empty) ml.Notify();
  } else if (block->IsEmpty()) {
    EmptyShard* shard = CurrentEmptyShard();
    MutexLocker ml(&shard->mutex);
    PushEmptyBlocksLocked(shard, block);
  } else {
    MonitorLocker ml(&monitor_);
    bool was_empty = IsEmptyLocked();
    partial_.Push(block);
    if (was_empty) ml.Notify();
  }
}

template <int BlockSize>
typename BlockStack<BlockSize>::Block* BlockStack<BlockSize>::WaitForWork(
    RelaxedAtomic<uintptr_t>* num_busy,
    bool abort) {
  MonitorLocker ml(&monitor_);
  if (num_busy->fetch_sub(1u) == 1 /* 1 is before subtraction */) {
    // This is the last worker, wake the others now that we know no further work
    // will come.
    ml.NotifyAll();
    return nullptr;
  }
  if (abort) {
    return nullptr;
  }
  for (;;) {
    if (!full_.IsEmpty()) {
      num_busy->fetch_add(1u);
      return full_.Pop();
    }
    if (!partial_.IsEmpty()) {
      num_busy->fetch_add(1u);
      return partial_.Pop();
    }
    ml.Wait();
    if (num_busy->load() == 0) {
      return nullptr;
    }
  }
}

template <int Size>
void PointerBlock<Size>::VisitObjectPointers(ObjectPointerVisitor* visitor) {
  // Generated code appends to store buffers; tell MemorySanitizer.
  MSAN_UNPOISON(this, sizeof(*this));
  visitor->VisitPointers(&pointers_[0], top_);
}

void StoreBuffer::PushBlock(Block* block, ThresholdPolicy policy) {
  BlockStack<Block::kSize>::PushBlockImpl(block);
  if ((policy == kCheckThreshold) && Overflowed()) {
    MonitorLocker ml(&monitor_);
    Thread* thread = Thread::Current();
    // Sanity check: it makes no sense to schedule the GC in another isolate
    // group.
    // (If Isolate ever gets multiple store buffers, we should avoid this
    // coupling by passing in an explicit callback+parameter at construction.)
    ASSERT(thread->isolate_group()->store_buffer() == this);
    thread->ScheduleInterrupts(Thread::kVMInterrupt);
  }
}

template <int BlockSize>
typename BlockStack<BlockSize>::Block*
BlockStack<BlockSize>::PopNonFullBlock() {
  {
    MonitorLocker ml(&monitor_);
    if (!partial_.IsEmpty()) {
      return partial_.Pop();
    }
  }
  return PopEmptyBlock();
}

template <int BlockSize>
typename BlockStack<BlockSize>::Block* BlockStack<BlockSize>::PopEmptyBlock() {
  const intptr_t start = CurrentThreadShardIndex();
  for (intptr_t i = 0; i < kNumEmptyShards; i++) {
    EmptyShard* shard = &empty_shards_[(start + i) % kNumEmptyShards];
    // Racy check to avoid locking empty shards.
    if (shard->list.length() == 0) continue;
    MutexLocker ml(&shard->mutex);
    if (!shard->list.IsEmpty()) {
      return shard->list.Pop();
    }
  }
  return new Block();
}

template <int BlockSize>
typename BlockStack<BlockSize>::Block*
BlockStack<BlockSize>::PopNonEmptyBlock() {
  MonitorLocker ml(&monitor_);
  if (!full_.IsEmpty()) {
    return full_.Pop();
  } else if (!partial_.IsEmpty()) {
    return partial_.Pop();
  } else {
    return nullptr;
  }
}

template <int BlockSize>
bool BlockStack<BlockSize>::IsEmpty() {
  MonitorLocker ml(&monitor_);
  return IsEmptyLocked();
}

template <int BlockSize>
bool BlockStack<BlockSize>::IsEmptyLocked() {
  return full_.IsEmpty() && partial_.IsEmpty();
}

template <int BlockSize>
BlockStack<BlockSize>::List::~List() {
  while (!IsEmpty()) {
    delete Pop();
  }
}

template <int BlockSize>
typename BlockStack<BlockSize>::Block* BlockStack<BlockSize>::List::Pop() {
  Block* result = head_;
  head_ = head_->next_;
  --length_;
  result->next_ = nullptr;
  return result;
}

template <int BlockSize>
typename BlockStack<BlockSize>::Block* BlockStack<BlockSize>::List::PopAll() {
  Block* result = head_;
  head_ = nullptr;
  length_ = 0;
  return result;
}

template <int BlockSize>
void BlockStack<BlockSize>::List::Push(Block* block) {
  ASSERT(block->next_ == nullptr);
  block->next_ = head_;
  head_ = block;
  ++length_;
}

bool StoreBuffer::Overflowed() {
  MonitorLocker ml(&monitor_);
  return (full_.length() + partial_.length()) > kMaxNonEmpty;
}

intptr_t StoreBuffer::Size() {
  ASSERT(Thread::Current()->OwnsGCSafepoint());  // No lock needed.
  return full_.length() + partial_.length();
}

template <int BlockSize>
void BlockStack<BlockSize>::VisitObjectPointers(ObjectPointerVisitor* visitor) {
  for (Block* block = full_.Peek(); block != nullptr; block = block->next()) {
    block->VisitObjectPointers(visitor);
  }
  for (Block* block = partial_.Peek(); block != nullptr;
       block = block->next()) {
    block->VisitObjectPointers(visitor);
  }
}

template class PointerBlock<kStoreBufferBlockSize>;
template class PointerBlock<kMarkingStackBlockSize>;

template class BlockStack<kStoreBufferBlockSize>;
template class BlockStack<kMarkingStackBlockSize>;

}  // namespace dart
