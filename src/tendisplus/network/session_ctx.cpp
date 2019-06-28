#include <utility>
#include <string>
#include <algorithm>
#include <list>
#include <vector>

#include "tendisplus/network/session_ctx.h"
#include "tendisplus/utils/invariant.h"

namespace tendisplus {

SessionCtx::SessionCtx(Session* sess)
    :_authed(false),
     _dbId(0),
     _waitlockStore(0),
     _waitlockMode(mgl::LockMode::LOCK_NONE),
     _waitlockKey(""),
     _processPacketStart(0),
     _timestamp(TSEP_UNINITED),
     _version(VERSIONEP_UNINITED),
     _extendProtocol(false),
     _replOnly(false),
     _session(sess),
     _isMonitor(false) {
}

void SessionCtx::setProcessPacketStart(uint64_t start) {
    _processPacketStart = start;
}

uint64_t SessionCtx::getProcessPacketStart() const {
    return _processPacketStart;
}

bool SessionCtx::authed() const {
    return _authed;
}

uint32_t SessionCtx::getDbId() const {
    return _dbId;
}

void SessionCtx::setDbId(uint32_t dbid) {
    _dbId = dbid;
}

void SessionCtx::setAuthed() {
    _authed = true;
}

void SessionCtx::addLock(ILock *lock) {
    std::lock_guard<std::mutex> lk(_mutex);
    _locks.push_back(lock);
}

void SessionCtx::removeLock(ILock *lock) {
    std::lock_guard<std::mutex> lk(_mutex);
    for (auto it = _locks.begin(); it != _locks.end(); ++it) {
        if (*it == lock) {
            _locks.erase(it);
            return;
        }
    }
    INVARIANT(0);
}

std::vector<std::string> SessionCtx::getArgsBrief() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _argsBrief;
}

void SessionCtx::setArgsBrief(const std::vector<std::string>& v) {
    std::lock_guard<std::mutex> lk(_mutex);
    constexpr size_t MAX_SIZE = 8;
    for (size_t i = 0; i < std::min(v.size(), MAX_SIZE); ++i) {
        _argsBrief.push_back(v[i]);
    }
}

void SessionCtx::clearRequestCtx() {
    std::lock_guard<std::mutex> lk(_mutex);
    _txnMap.clear();
    _argsBrief.clear();
    _timestamp = -1;
    _version = -1;
}

Expected<Transaction*> SessionCtx::createTransaction(const PStore& kvstore) {
    Transaction* txn = nullptr;
    if (_txnMap.count(kvstore->dbId()) > 0) {
        txn = _txnMap[kvstore->dbId()].get();
    }
    else {
        auto ptxn = kvstore->createTransaction(_session);
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        _txnMap[kvstore->dbId()] = std::move(ptxn.value());
        txn = _txnMap[kvstore->dbId()].get();
    }

    return txn;
}

Status SessionCtx::commitAll(const std::string& cmd) {
    std::lock_guard<std::mutex> lk(_mutex);
    Status s;
    for (auto& txn : _txnMap) {
        Expected<uint64_t> exptCommit = txn.second->commit();
        if (!exptCommit.ok()) {
            LOG(ERROR) << cmd << " commit error at kvstore " << txn.first
                << ". It lead to partial success.";
            s = exptCommit.status();
        }
    }
    _txnMap.clear();
    return s;
}

Status SessionCtx::rollbackAll() {
    std::lock_guard<std::mutex> lk(_mutex);
    Status s = {ErrorCodes::ERR_OK, ""};
    for (auto& txn : _txnMap) {
        s = txn.second->rollback();
        if (!s.ok()) {
            LOG(ERROR) << "rollback error at kvstore " << txn.first
                << ". It maybe lead to partial success.";
        }
    }
    _txnMap.clear();
    return s;
}

void SessionCtx::setWaitLock(uint32_t storeId, const std::string& key, mgl::LockMode mode) {
    _waitlockStore = storeId;
    _waitlockMode = mode;
    _waitlockKey = key;
}

SLSP SessionCtx::getWaitlock() const {
    return std::tuple<uint32_t, std::string, mgl::LockMode>(
            _waitlockStore, _waitlockKey, _waitlockMode);
}

std::list<SLSP> SessionCtx::getLockStates() const {
    std::lock_guard<std::mutex> lk(_mutex);
    std::list<SLSP> result;
    for (auto& lk : _locks) {
        result.push_back(
            SLSP(lk->getStoreId(), lk->getKey(), lk->getMode()));
    }
    return result;
}

void SessionCtx::setExtendProtocol(bool v) {
    _extendProtocol = v;
}
void SessionCtx::setExtendProtocolValue(uint64_t ts, uint64_t version) {
    _timestamp = ts;
    _version = version;
}

uint32_t SessionCtx::getIsMonitor() const {
    return _isMonitor;
}

void SessionCtx::setIsMonitor(bool in) {
    _isMonitor = in;
}

void SessionCtx::setKeylock(const std::string& key, mgl::LockMode mode) {
    _keylockmap[key] = mode;
}

void SessionCtx::unsetKeylock(const std::string& key) {
    INVARIANT(_keylockmap.count(key) > 0);
    _keylockmap.erase(key);
}

bool SessionCtx::isLockedByMe(const std::string &key, mgl::LockMode mode) {
    if (_keylockmap.count(key) > 0) {
        // TODO(comboqiu): Here, lock can't upgrade or downgrade.
        // If a key lock twice in one session , it can't lock a bigger lock.
        // assert temporary.
        INVARIANT(mgl::enum2Int(mode) <= mgl::enum2Int(_keylockmap[key]));
        return true;
    }
    return false;
}

}  // namespace tendisplus
