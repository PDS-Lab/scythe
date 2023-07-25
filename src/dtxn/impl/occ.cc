#include "occ.h"

#include <cstdint>
#include <memory>

#include "common.h"
#include "coroutine_pool/scheduler.h"
#include "dtxn/dtxn.h"
#include "dtxn/takeout_lock.h"
#include "dtxn/tso.h"
#include "proto/rdma.h"
#include "proto/rpc.h"
#include "rrpc/rdma_cm.h"
#include "rrpc/rrpc.h"
#include "storage/object.h"
#include "util/logging.h"
#include "util/timer.h"

TxnStatus OCC::Read(TxnObjPtr obj) {
  if (obj->op() & TxnObj::READ) {
    return TxnStatus::OK;
  }
  obj->set_read();
  read_set_.push_back(obj);
  // issue RDMA read
  auto rkt = GetRocket(0);
  auto ctx = ReadReqCtx{TxnStatus::INTERNAL, obj.get(), this_coroutine::current()};
  auto req = rkt->gen_request<ReadReq>(sizeof(ReadReq), READ, read_service_cb, &ctx);
  req->obj_id = obj->id();
  req->size = obj->size();
  req->ts = begin_ts_;
  req->mode = Mode::COLD;
  rkt->batching();
  this_coroutine::co_wait();
  return ctx.rc;
};

TxnStatus OCC::Read(const std::vector<TxnObjPtr> &objs) {
  auto rkt = GetRocket(0);
  auto num = objs.size();
  ReadReqCtx ctxs[num];
  int cnt = 0;
  for (size_t i = 0; i < num; i++) {
    auto obj = objs[i];
    if (obj->op() & TxnObj::READ) {
      continue;
    }
    obj->set_read();
    read_set_.push_back(obj);
    // issue RDMA read
    ctxs[cnt] = {TxnStatus::INTERNAL, obj.get(), this_coroutine::current()};
    auto req = rkt->gen_request<ReadReq>(sizeof(ReadReq), READ, read_service_cb, &ctxs[cnt]);
    req->obj_id = obj->id();
    req->size = obj->size();
    req->ts = begin_ts_;
    req->mode = Mode::COLD;
    rkt->batching();
    cnt++;
  }
  this_coroutine::co_wait(cnt);
  auto rc = TxnStatus::OK;
  for (size_t i = 0; i < cnt; i++) {
    if (ctxs[i].rc == TxnStatus::SWITCH) {
      return TxnStatus::SWITCH;
    }
    if (ctxs[i].rc == TxnStatus::RETRY) {
      rc = TxnStatus::RETRY;
    }
  }
  return rc;
};

TxnStatus OCC::Write(TxnObjPtr obj) {
  if (obj->op() & TxnObj::WRITE) {
    return TxnStatus::OK;
  }
  obj->set_write();
  write_set_.push_back(obj);
  // delayed write
  return TxnStatus::OK;
};

TxnStatus OCC::Write(const std::vector<TxnObjPtr> &objs) {
  for (auto &obj : objs) {
    Write(obj);
  }
  return TxnStatus::OK;
};

TxnStatus OCC::Commit() {
  TxnStatus rc;
  if (write_set_.empty()) {
    // pure read
    return TxnStatus::OK;
  }
  // lock
  rc = lock();
  if (rc != TxnStatus::OK) {
    Rollback();
    return rc;
  }

  commit_ts_ = TSO::get_ts();
  rc = validate();
  if (rc != TxnStatus::OK) {
    Rollback();
    return rc;
  }
  rc = issue_write();
  ENSURE(rc == TxnStatus::OK, "issue write failed");

  rc = unlock();
  ENSURE(rc == TxnStatus::OK, "unlock failed");
  return TxnStatus::OK;
};

TxnStatus OCC::Commit(struct timeval& lock_start_tv,struct timeval& lock_end_tv,struct timeval& validation_start_tv,struct timeval& validation_end_tv,struct timeval& write_start_tv,struct timeval& write_end_tv,struct timeval& commit_start_tv,struct timeval& commit_end_tv)
{
  TxnStatus rc;
  if (write_set_.empty()) {
    // pure read
    return TxnStatus::OK;
  }
  // lock
  gettimeofday(&lock_start_tv,NULL);
  rc = lock();
  gettimeofday(&lock_end_tv,NULL);
  if (rc != TxnStatus::OK) {
    Rollback();
    return rc;
  }

  commit_ts_ = TSO::get_ts();
  gettimeofday(&validation_start_tv,NULL);
  rc = validate();
  gettimeofday(&validation_end_tv,NULL);
  if (rc != TxnStatus::OK) {
    Rollback();
    return rc;
  }
  gettimeofday(&write_start_tv,NULL);
  rc = issue_write();
  gettimeofday(&write_end_tv,NULL);
  ENSURE(rc == TxnStatus::OK, "issue write failed");

  gettimeofday(&commit_start_tv,NULL);
  rc = unlock();
  gettimeofday(&commit_end_tv,NULL);
  ENSURE(rc == TxnStatus::OK, "unlock failed");
  return TxnStatus::OK;
}

TxnStatus OCC::lock() {
  auto rkt = GetRocket(0);
  QueuingCtx ctxs[write_set_.size()];
  int sz = 0;
  // queueing
  for (auto obj : write_set_) {
    if (obj->put_new_) {
      // 新创建的对象不用加锁
      continue;
    }
    ctxs[sz].obj = obj.get();
    ctxs[sz].self = this_coroutine::current();

    auto req = rkt->gen_request<QueuingReq>(sizeof(QueuingReq), QUEUING, queuing_service_cb, &ctxs[sz]);
    req->obj_id = obj->id();
    req->ts = begin_ts_;
    req->mode = Mode::COLD;
    rkt->batching();
    sz++;
  }

  this_coroutine::co_wait(sz);
  for (int i = 0; i < sz; i++) {
    LOG_INFO("[%d] [obj %lu] lock info {lower:%lu, upper:%lu, ts:%lu}", this_coroutine::current()->id(),
             ctxs[i].obj->id(), ctxs[i].obj->lock_proxy->tl.lower, ctxs[i].obj->lock_proxy->tl.upper,
             ctxs[i].obj->lock_proxy->tl.queued_ts);
  }
  // handle lock
  int delegate_num = 0;
  bool early_abort = false;
  bool switch_mode = false;
  for (int i = 0; i < sz; i++) {
    if (ctxs[i].is_queued && !ctxs[i].obj->lock_proxy->tl.ready()) {
      auto &lock_proxy = ctxs[i].obj->lock_proxy;
      lock_proxy->us_since_poll = RdtscTimer::instance().us();
      lock_proxy->hangout = RTT * lock_proxy->tl.queued_num();
      this_coroutine::scheduler_delegate(lock_proxy);
      delegate_num++;
    } else if (!ctxs[i].is_queued) {
      early_abort = true;
      switch_mode = (switch_mode || ctxs[i].obj->lock_proxy->tl.queued_num() > kColdWatermark);
    } else {
      ctxs[i].obj->hold_lock = true;
    }
  }

  if (switch_mode || early_abort) {
    return switch_mode ? TxnStatus::SWITCH : TxnStatus::RETRY;
  }

  this_coroutine::co_wait(delegate_num);
  // lock success
  return TxnStatus::OK;
};

TxnStatus OCC::validate() {
  // one side validate
  auto rkt = GetRocket(0);
  ValidateCtx ctx;
  ctx.self = this_coroutine::current();
  for (auto obj : read_set_) {
    // sequential validate
    auto raw_buf = std::unique_ptr<char[]>(new char[sizeof(object)]);
    rkt->remote_read(raw_buf.get(), sizeof(object), obj->obj_addr, obj->rkey, validate_cb, &ctx);
    this_coroutine::co_wait();
    // check lock
    object *remote_obj = reinterpret_cast<object *>(raw_buf.get());
    if (!obj->hold_lock && remote_obj->lock.queued_num() != 0) {
      return TxnStatus::VALIDATE_FAIL;
    }
    // check value
    if (reinterpret_cast<uint64_t>(remote_obj->latest) != obj->ptr_val) {
      return TxnStatus::VALIDATE_FAIL;
    }
  }
  return TxnStatus::OK;
};

TxnStatus OCC::unlock() {
  // one side unlock
  int sz = 0;
  for (auto obj : write_set_) {
    if (obj->hold_lock) {
      obj->lock_proxy->unlock();
      sz++;
    }
  }
  this_coroutine::co_wait(sz);
  return TxnStatus::OK;
}

TxnStatus OCC::Rollback() { return unlock(); };

TxnStatus OCC::issue_write() {
  auto rkt = GetRocket(0);
  auto ctxs = std::unique_ptr<WriteCtx[]>(new WriteCtx[write_set_.size()]);

  int sz = 0;
  for (auto obj : write_set_) {
    ctxs[sz] = {DbStatus::UNEXPECTED_ERROR, this_coroutine::current()};
    auto req = rkt->gen_request<WriteReq>(sizeof(WriteReq) + obj->size(), WRITE, write_service_cb, &ctxs[sz]);
    req->obj_id = obj->id();
    req->size = obj->size();
    req->ts = commit_ts_;
    req->create = obj->put_new_;
    memcpy(req->data, obj->data(), obj->size());
    rkt->batching();
    sz++;
  }
  this_coroutine::co_wait(sz);

  for (int i = 0; i < sz; i++) {
    if (ctxs[i].status != DbStatus::OK) {
      return TxnStatus::INTERNAL;
    }
  }
  return TxnStatus::OK;
}