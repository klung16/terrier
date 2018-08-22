#include <queue>
#include <utility>
#include "transaction/transaction_manager.h"

namespace terrier::transaction {
TransactionContext *TransactionManager::BeginTransaction() {
  common::ReaderWriterLatch::ScopedReaderLatch guard(&commit_latch_);
  timestamp_t id = time_++;
  // TODO(Tianyu):
  // Maybe embed this into the data structure, or use an object pool?
  // Doing this with std::map or other data structure is risky though, as they may not
  // guarantee that the iterator or underlying pointer is stable across operations.
  // (That is, they may change as concurrent inserts and deletes happen)
  auto *result = new TransactionContext(id, id + INT64_MIN, buffer_pool_);
  table_latch_.Lock();
  auto ret UNUSED_ATTRIBUTE = curr_running_txns_.emplace(result->StartTime(), result);
  TERRIER_ASSERT(ret.second, "commit start time should be globally unique");
  table_latch_.Unlock();
  return result;
}

timestamp_t TransactionManager::Commit(TransactionContext *const txn) {
  common::ReaderWriterLatch::ScopedWriterLatch guard(&commit_latch_);
  const timestamp_t commit_time = time_++;
  // Flip all timestamps to be committed
  storage::UndoBuffer &undos = txn->GetUndoBuffer();
  for (auto &it : undos) it.Timestamp().store(commit_time);
  table_latch_.Lock();
  const timestamp_t start_time = txn->StartTime();
  size_t result UNUSED_ATTRIBUTE = curr_running_txns_.erase(start_time);
  TERRIER_ASSERT(result == 1, "committed transaction did not exist in global transactions table");
  txn->TxnId().store(commit_time);
  if (gc_enabled_) completed_txns_.push(txn);
  table_latch_.Unlock();
  return commit_time;
}

void TransactionManager::Abort(TransactionContext *const txn) {
  // no latch required on undo since all operations are transaction-local
  storage::UndoBuffer &undos = txn->GetUndoBuffer();
  timestamp_t txn_id = txn->TxnId().load();  // will not change
  for (auto &it : undos) Rollback(txn_id, it);
  table_latch_.Lock();
  const timestamp_t start_time = txn->StartTime();
  size_t ret UNUSED_ATTRIBUTE = curr_running_txns_.erase(start_time);
  TERRIER_ASSERT(ret == 1, "aborted transaction did not exist in global transactions table");
  if (gc_enabled_) completed_txns_.push(txn);
  table_latch_.Unlock();
}

timestamp_t TransactionManager::OldestTransactionStartTime() const {
  table_latch_.Lock();
  auto oldest_txn = curr_running_txns_.begin();
  timestamp_t result = (oldest_txn != curr_running_txns_.end()) ? oldest_txn->second->StartTime() : time_.load();
  table_latch_.Unlock();
  return result;
}

std::queue<TransactionContext *> TransactionManager::CompletedTransactionsForGC() {
  table_latch_.Lock();
  std::queue<transaction::TransactionContext *> hand_to_gc(std::move(completed_txns_));
  TERRIER_ASSERT(completed_txns_.empty(), "TransactionManager's queue should now be empty.");
  table_latch_.Unlock();
  return hand_to_gc;
}

void TransactionManager::Rollback(timestamp_t txn_id, const storage::UndoRecord &record) {
  storage::DataTable *table = record.Table();
  storage::TupleSlot slot = record.Slot();
  storage::UndoRecord
      *version_ptr = table->AtomicallyReadVersionPtr(slot, table->accessor_);
  // We do not hold the lock. Should just return
  if (version_ptr == nullptr || version_ptr->Timestamp().load() != txn_id) return;
  // Re-apply the before image
  for (uint16_t i = 0; i < version_ptr->Delta()->NumColumns(); i++)
    storage::StorageUtil::CopyAttrFromProjection(table->accessor_, slot, *(version_ptr->Delta()), i);
  // Remove this delta record from the version chain, effectively releasing the lock. At this point, the tuple
  // has been restored to its original form. No CAS needed since we still hold the write lock at time of the atomic
  // write.
  table->AtomicallyWriteVersionPtr(slot, table->accessor_, version_ptr->Next());
}
}  // namespace terrier::transaction
