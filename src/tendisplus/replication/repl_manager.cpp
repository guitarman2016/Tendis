#include <list>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <string>
#include <set>
#include <map>
#include <limits>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/error/en.h"
#include "glog/logging.h"
#include "tendisplus/replication/repl_manager.h"
#include "tendisplus/storage/record.h"
#include "tendisplus/commands/command.h"
#include "tendisplus/utils/scopeguard.h"
#include "tendisplus/utils/redis_port.h"
#include "tendisplus/utils/invariant.h"
#include "tendisplus/utils/string.h"
#include "tendisplus/utils/rate_limiter.h"
#include "tendisplus/lock/lock.h"

namespace tendisplus {

ReplManager::ReplManager(std::shared_ptr<ServerEntry> svr, std::shared_ptr<ServerParams> cfg)
    :_isRunning(false),
     _svr(svr),
     _rateLimiter(std::make_unique<RateLimiter>(64*1024*1024)),
     _incrPaused(false),
     _clientIdGen(0),
     _dumpPath(cfg->dumpPath),
     _fullPushMatrix(std::make_shared<PoolMatrix>()),
     _incrPushMatrix(std::make_shared<PoolMatrix>()),
     _fullReceiveMatrix(std::make_shared<PoolMatrix>()),
     _incrCheckMatrix(std::make_shared<PoolMatrix>()),
     _logRecycleMatrix(std::make_shared<PoolMatrix>()) {
}

Status ReplManager::startup() {
    std::lock_guard<std::mutex> lk(_mutex);
    Catalog *catalog = _svr->getCatalog();
    INVARIANT(catalog != nullptr);

    for (uint32_t i = 0; i < KVStore::INSTANCE_NUM; i++) {
        Expected<std::unique_ptr<StoreMeta>> meta = catalog->getStoreMeta(i);
        if (meta.ok()) {
            _syncMeta.emplace_back(std::move(meta.value()));
        } else if (meta.status().code() == ErrorCodes::ERR_NOTFOUND) {
            auto pMeta = std::unique_ptr<StoreMeta>(
                new StoreMeta(i, "", 0, -1,
                    Transaction::TXNID_UNINITED, ReplState::REPL_NONE));
            Status s = catalog->setStoreMeta(*pMeta);
            if (!s.ok()) {
                return s;
            }
            _syncMeta.emplace_back(std::move(pMeta));
        } else {
            return meta.status();
        }
    }

    INVARIANT(_syncMeta.size() == KVStore::INSTANCE_NUM);

    for (size_t i = 0; i < _syncMeta.size(); ++i) {
        if (i != _syncMeta[i]->id) {
            std::stringstream ss;
            ss << "meta:" << i << " has id:" << _syncMeta[i]->id;
            return {ErrorCodes::ERR_INTERNAL, ss.str()};
        }
    }

    for (uint32_t i = 0; i < KVStore::INSTANCE_NUM; i++) {
        _syncStatus.emplace_back(
            std::unique_ptr<SPovStatus>(
                new SPovStatus {
                    isRunning: false,
                    sessionId: std::numeric_limits<uint64_t>::max(),
                    nextSchedTime: SCLOCK::now(),
                    lastSyncTime: SCLOCK::now(),
                }));
    }

    // TODO(deyukong): configure
    size_t cpuNum = std::thread::hardware_concurrency();
    if (cpuNum == 0) {
        return {ErrorCodes::ERR_INTERNAL, "cpu num cannot be detected"};
    }

    _incrPusher = std::make_unique<WorkerPool>(
            "repl-minc", _incrPushMatrix);
    Status s = _incrPusher->startup(INCR_POOL_SIZE);
    if (!s.ok()) {
        return s;
    }

    _fullPusher = std::make_unique<WorkerPool>(
            "repl-mfull", _fullPushMatrix);
    s = _fullPusher->startup(MAX_FULL_PARAL);
    if (!s.ok()) {
        return s;
    }

    _fullReceiver = std::make_unique<WorkerPool>(
            "repl-sfull", _fullReceiveMatrix);
    s = _fullReceiver->startup(MAX_FULL_PARAL);
    if (!s.ok()) {
        return s;
    }

    _incrChecker = std::make_unique<WorkerPool>(
            "repl-scheck", _incrCheckMatrix);
    s = _incrChecker->startup(2);
    if (!s.ok()) {
        return s;
    }

    _logRecycler = std::make_unique<WorkerPool>(
            "log-recyc", _logRecycleMatrix);
    s = _logRecycler->startup(INCR_POOL_SIZE);
    if (!s.ok()) {
        return s;
    }

    // init master's pov, incrpush status
    for (uint32_t i = 0; i < KVStore::INSTANCE_NUM; i++) {
        _pushStatus.emplace_back(
            std::map<uint64_t, std::unique_ptr<MPovStatus>>());
    }

    for (uint32_t i = 0; i < KVStore::INSTANCE_NUM; i++) {
        // here we are starting up, dont acquire a storelock.
        auto expdb = _svr->getSegmentMgr()->getDb(nullptr, i, mgl::LockMode::LOCK_NONE);
        INVARIANT(expdb.ok());
        auto store = std::move(expdb.value().store);
        INVARIANT(store != nullptr);

        if (_syncMeta[i]->syncFromHost == "") {
            store->setMode(KVStore::StoreMode::READ_WRITE);
        } else {
            store->setMode(KVStore::StoreMode::REPLICATE_ONLY);
        }
        Expected<uint32_t> efileSeq = maxDumpFileSeq(i);
        if (!efileSeq.ok()) {
            return efileSeq.status();
        }

        auto recBinlogStat = std::unique_ptr<RecycleBinlogStatus>(
            new RecycleBinlogStatus {
                isRunning: false,
                nextSchedTime: SCLOCK::now(),
                firstBinlogId: Transaction::TXNID_UNINITED,
                fileSeq: efileSeq.value(),
                fileCreateTime: SCLOCK::now(),
                fileSize: 0,
                fs: nullptr,
            });

        auto ptxn = store->createTransaction();
        if (!ptxn.ok()) {
            return ptxn.status();
        }
        auto txn = std::move(ptxn.value());
        std::unique_ptr<BinlogCursor> cursor =
            txn->createBinlogCursor(Transaction::MIN_VALID_TXNID);
        Expected<ReplLog> explog = cursor->next();
        if (explog.ok()) {
            const auto& rlk = explog.value().getReplLogKey();
            recBinlogStat->firstBinlogId = rlk.getTxnId();
            _logRecycStatus.emplace_back(std::move(recBinlogStat));
        } else {
            if (explog.status().code() == ErrorCodes::ERR_EXHAUST) {
                // void compiler ud-link about static constexpr
                uint64_t v = Transaction::TXNID_UNINITED;
                recBinlogStat->firstBinlogId = v;
                _logRecycStatus.emplace_back(std::move(recBinlogStat));
            } else {
                return explog.status();
            }
        }
        LOG(INFO) << "store:" << i
                  << ",_firstBinlogId:" << _logRecycStatus.back()->firstBinlogId;
    }

    INVARIANT(_logRecycStatus.size() == KVStore::INSTANCE_NUM);

    _isRunning.store(true, std::memory_order_relaxed);
    _controller = std::make_unique<std::thread>(std::move([this]() {
        controlRoutine();
    }));

    return {ErrorCodes::ERR_OK, ""};
}

void ReplManager::changeReplStateInLock(const StoreMeta& storeMeta,
                                        bool persist) {
    // TODO(deyukong): mechanism to INVARIANT mutex held
    if (persist) {
        Catalog *catalog = _svr->getCatalog();
        Status s = catalog->setStoreMeta(storeMeta);
        if (!s.ok()) {
            LOG(FATAL) << "setStoreMeta failed:" << s.toString();
        }
    }
    _syncMeta[storeMeta.id] = std::move(storeMeta.copy());
}

Expected<uint32_t> ReplManager::maxDumpFileSeq(uint32_t storeId) {
    std::string subpath = _dumpPath + "/" + std::to_string(storeId);
    try {
        if (!filesystem::exists(_dumpPath)) {
            filesystem::create_directory(_dumpPath);
        }
        if (!filesystem::exists(subpath)) {
            filesystem::create_directory(subpath);
        }
    } catch (const std::exception& ex) {
        LOG(ERROR) << "create dir:" << _dumpPath
                    << " or " << subpath << " failed reason:" << ex.what();
        return {ErrorCodes::ERR_INTERNAL, ex.what()};
    }
    uint32_t maxFno = 0;
    try {
        for (auto& p : filesystem::recursive_directory_iterator(subpath)) {
            const filesystem::path& path = p.path();
            if (!filesystem::is_regular_file(p)) {
                LOG(INFO) << "maxDumpFileSeq ignore:" << p.path();
                continue;
            }
            // assert path with dir prefix
            INVARIANT(path.string().find(subpath) == 0);
            std::string relative = path.string().erase(0, subpath.size());
            if (relative.substr(0, 6) != "binlog") {
                LOG(INFO) << "maxDumpFileSeq ignore:" << relative;
            }
            size_t i = 0, start = 0, end = 0, first = 0;
            for (; i < relative.size(); ++i) {
                if (relative[i] == '-') {
                    first += 1;
                    if (first == 2) {
                        start = i+1;
                    }
                    if (first == 3) {
                        end = i;
                        break;
                    }
                }
            }
            Expected<uint64_t> fno = ::tendisplus::stoul(relative.substr(start, end-start));
            if (!fno.ok()) {
                LOG(ERROR) << "parse fileno:" << relative << " failed:" << fno.status().toString();
                return fno.value();
            }
            if (fno.value() >= std::numeric_limits<uint32_t>::max()) {
                LOG(ERROR) << "invalid fileno:" << fno.value();
                return {ErrorCodes::ERR_INTERNAL, "invalid fileno"};
            }
            maxFno = std::max(maxFno, (uint32_t)fno.value());
        }
    } catch (const std::exception& ex) {
        LOG(ERROR) << "store:" << storeId << " get fileno failed:" << ex.what();
        return {ErrorCodes::ERR_INTERNAL, "parse fileno failed"};
    }
    return maxFno;
}

void ReplManager::changeReplState(const StoreMeta& storeMeta,
                                        bool persist) {
    std::lock_guard<std::mutex> lk(_mutex);
    changeReplStateInLock(storeMeta, persist);
}

std::shared_ptr<BlockingTcpClient> ReplManager::createClient(
                    const StoreMeta& metaSnapshot) {
    std::shared_ptr<BlockingTcpClient> client =
        std::move(_svr->getNetwork()->createBlockingClient(64*1024*1024));
    Status s = client->connect(
        metaSnapshot.syncFromHost,
        metaSnapshot.syncFromPort,
        std::chrono::seconds(3));
    if (!s.ok()) {
        LOG(WARNING) << "connect " << metaSnapshot.syncFromHost
            << ":" << metaSnapshot.syncFromPort << " failed:"
            << s.toString();
        return nullptr;
    }

    std::shared_ptr<std::string> masterauth = _svr->masterauth();
    if (*masterauth != "") {
        std::stringstream ss;
        ss << "AUTH " << *masterauth;
        client->writeLine(ss.str(), std::chrono::seconds(1));
        Expected<std::string> s = client->readLine(std::chrono::seconds(1));
        if (!s.ok()) {
            LOG(WARNING) << "fullSync auth error:" << s.status().toString();
            return nullptr;
        }
        if (s.value().size() == 0 || s.value()[0] == '-') {
            LOG(INFO) << "fullSync auth failed:" << s.value();
            return nullptr;
        }
    }
    return std::move(client);
}

void ReplManager::controlRoutine() {
    using namespace std::chrono_literals;  // (NOLINT)
    auto schedSlaveInLock = [this](const SCLOCK::time_point& now) {
        // slave's POV
        bool doSth = false;
        for (size_t i = 0; i < _syncStatus.size(); i++) {
            if (_syncStatus[i]->isRunning
                    || now < _syncStatus[i]->nextSchedTime
                    || _syncMeta[i]->replState == ReplState::REPL_NONE) {
                continue;
            }
            doSth = true;
            // NOTE(deyukong): we dispatch fullsync/incrsync jobs into
            // different pools.
            if (_syncMeta[i]->replState == ReplState::REPL_CONNECT) {
                _syncStatus[i]->isRunning = true;
                _fullReceiver->schedule([this, i]() {
                    slaveSyncRoutine(i);
                });
            } else if (_syncMeta[i]->replState == ReplState::REPL_CONNECTED) {
                _syncStatus[i]->isRunning = true;
                _incrChecker->schedule([this, i]() {
                    slaveSyncRoutine(i);
                });
            } else if (_syncMeta[i]->replState == ReplState::REPL_TRANSFER) {
                LOG(FATAL) << "sync store:" << i
                    << " REPL_TRANSFER should not be visitable";
            } else {  // REPL_NONE
                // nothing to do with REPL_NONE
            }
        }
        return doSth;
    };
    auto schedMasterInLock = [this](const SCLOCK::time_point& now) {
        // master's POV
        bool doSth = false;
        for (size_t i = 0; i < _pushStatus.size(); i++) {
            for (auto& mpov : _pushStatus[i]) {
                if (mpov.second->isRunning
                        || now < mpov.second->nextSchedTime) {
                    continue;
                }

                doSth = true;
                mpov.second->isRunning = true;
                uint64_t clientId = mpov.first;
                _incrPusher->schedule([this, i, clientId]() {
                    masterPushRoutine(i, clientId);
                });
            }
        }
        return doSth;
    };
    auto schedRecycLogInLock = [this](const SCLOCK::time_point& now) {
        bool doSth = false;
        for (size_t i = 0; i < _logRecycStatus.size(); ++i) {
            if (_logRecycStatus[i]->isRunning
                    || now < _logRecycStatus[i]->nextSchedTime) {
                continue;
            }
            doSth = true;
            bool saveLogs = (_pushStatus[i].size() == 0);
            _logRecycStatus[i]->isRunning = true;
            uint64_t endLogId = std::numeric_limits<uint64_t>::max();
            uint64_t oldFirstBinlog = _logRecycStatus[i]->firstBinlogId;
            for (auto& mpov : _pushStatus[i]) {
                endLogId = std::min(endLogId, mpov.second->binlogPos);
            }
            _logRecycler->schedule([this, i, oldFirstBinlog, endLogId, saveLogs]() {
                recycleBinlog(i, oldFirstBinlog, endLogId, saveLogs);
            });
        }
        return doSth;
    };
    while (_isRunning.load(std::memory_order_relaxed)) {
        bool doSth = false;
        auto now = SCLOCK::now();
        {
            std::lock_guard<std::mutex> lk(_mutex);
            doSth = schedSlaveInLock(now);
            doSth = schedMasterInLock(now) || doSth;
            doSth = schedRecycLogInLock(now) || doSth;
        }
        if (doSth) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(10ms);
        }
    }
    LOG(INFO) << "repl controller exits";
}

void ReplManager::recycleBinlog(uint32_t storeId, uint64_t start, uint64_t end, bool saveLogs) {
    SCLOCK::time_point nextSched = SCLOCK::now();
    auto guard = MakeGuard([this, &nextSched, &start, storeId] {
        std::lock_guard<std::mutex> lk(_mutex);
        auto& v = _logRecycStatus[storeId];
        INVARIANT(v->isRunning);
        v->isRunning = false;
        v->nextSchedTime = nextSched;
        v->firstBinlogId = start;
        // currently nothing waits for recycleBinlog's complete
        // _cv.notify_all();
    });
    LocalSessionGuard sg(_svr);
    sg.getSession()->getCtx()->setArgsBrief(
        {"truncatelog",
         std::to_string(storeId),
         std::to_string(start),
         std::to_string(end)});

    auto segMgr = _svr->getSegmentMgr();
    INVARIANT(segMgr != nullptr);
    auto expdb = segMgr->getDb(sg.getSession(), storeId, mgl::LockMode::LOCK_IX);
    INVARIANT(expdb.ok());
    auto kvstore = std::move(expdb.value().store);
    auto ptxn = kvstore->createTransaction();
    if (!ptxn.ok()) {
        LOG(ERROR) << "recycleBinlog create txn failed:" << ptxn.status().toString();
        return;
    }
    auto txn = std::move(ptxn.value());

    auto toDel = kvstore->getTruncateLog(start, end, txn.get());
    if (!toDel.ok()) {
        LOG(ERROR) << "get to be truncated binlog store:" << storeId
                    << "start:" << start
                    << ",end:" << end
                    << ",failed:" << toDel.status().toString();
        return;
    }
    if (start == toDel.value().first) {
        INVARIANT(toDel.value().second.size() == 0);
        nextSched = nextSched + std::chrono::seconds(1);
        return;
    }

    if (saveLogs) {
        auto s = saveBinlogs(storeId, toDel.value().second);
        if (!s.ok()) {
            LOG(ERROR) << "save binlog store:" << storeId
                        << "failed:" << s.toString();
            return;
        }
    }
    auto s = kvstore->truncateBinlog(toDel.value().second, txn.get());
    if (!s.ok()) {
        LOG(ERROR) << "truncate binlog store:" << storeId
                    << "failed:" << s.toString();
        return;
    }
    auto commitStat = txn->commit();
    if (!commitStat.ok()) {
        LOG(ERROR) << "truncate binlog store:" << storeId
                    << "commit failed:" << commitStat.status().toString();
        return;
    }
    LOG(INFO) << "truncate binlog from:" << start
                 << " to end:" << toDel.value().first << " success";
    start = toDel.value().first;
}

// changeReplSource should be called with LOCK_X held
Status ReplManager::changeReplSource(uint32_t storeId, std::string ip,
            uint32_t port, uint32_t sourceStoreId) {
    LOG(INFO) << "wait for store:" << storeId << " to yield work";
    std::unique_lock<std::mutex> lk(_mutex);
    // NOTE(deyukong): we must wait for the target to stop before change meta,
    // or the meta may be rewrited
    if (!_cv.wait_for(lk, std::chrono::seconds(1),
            [this, storeId] { return !_syncStatus[storeId]->isRunning; })) {
        return {ErrorCodes::ERR_TIMEOUT, "wait for yeild failed"};
    }
    LOG(INFO) << "wait for store:" << storeId << " to yield work succ";
    INVARIANT(!_syncStatus[storeId]->isRunning);

    if (storeId >= _syncMeta.size()) {
        return {ErrorCodes::ERR_INTERNAL, "invalid storeId"};
    }
    auto segMgr = _svr->getSegmentMgr();
    INVARIANT(segMgr != nullptr);
    auto expdb = segMgr->getDb(nullptr, storeId, mgl::LockMode::LOCK_NONE);
    INVARIANT(expdb.ok());
    auto kvstore = std::move(expdb.value().store);

    auto newMeta = _syncMeta[storeId]->copy();
    if (ip != "") {
        if (_syncMeta[storeId]->syncFromHost != "") {
            return {ErrorCodes::ERR_BUSY,
                    "explicit set sync source empty before change it"};
        }

        Status s = kvstore->setMode(KVStore::StoreMode::REPLICATE_ONLY);
        if (!s.ok()) {
            return s;
        }
        newMeta->syncFromHost = ip;
        newMeta->syncFromPort = port;
        newMeta->syncFromId = sourceStoreId;
        newMeta->replState = ReplState::REPL_CONNECT;
        newMeta->binlogId = Transaction::TXNID_UNINITED;
        LOG(INFO) << "change store:" << storeId
                  << " syncSrc from no one to " << newMeta->syncFromHost
                  << ":" << newMeta->syncFromPort
                  << ":" << newMeta->syncFromId;
        changeReplStateInLock(*newMeta, true);
        return {ErrorCodes::ERR_OK, ""};
    } else {  // ip == ""
        if (newMeta->syncFromHost == "") {
            return {ErrorCodes::ERR_OK, ""};
        }
        LOG(INFO) << "change store:" << storeId
                  << " syncSrc:" << newMeta->syncFromHost
                  << " to no one";
        Status closeStatus =
            _svr->cancelSession(_syncStatus[storeId]->sessionId);
        if (!closeStatus.ok()) {
            // this error does not affect much, just log and continue
            LOG(WARNING) << "cancel store:" << storeId << " session failed:"
                        << closeStatus.toString();
        }
        _syncStatus[storeId]->sessionId = std::numeric_limits<uint64_t>::max();

        Status s = kvstore->setMode(KVStore::StoreMode::READ_WRITE);
        if (!s.ok()) {
            return s;
        }

        newMeta->syncFromHost = ip;
        INVARIANT(port == 0 && sourceStoreId == 0);
        newMeta->syncFromPort = port;
        newMeta->syncFromId = sourceStoreId;
        newMeta->replState = ReplState::REPL_NONE;
        newMeta->binlogId = Transaction::TXNID_UNINITED;
        changeReplStateInLock(*newMeta, true);
        return {ErrorCodes::ERR_OK, ""};
    }
}

void ReplManager::appendJSONStat(
        rapidjson::Writer<rapidjson::StringBuffer>& w) const {
    std::lock_guard<std::mutex> lk(_mutex);
    INVARIANT(_pushStatus.size() == KVStore::INSTANCE_NUM);
    INVARIANT(_syncStatus.size() == KVStore::INSTANCE_NUM);
    for (size_t i = 0; i < KVStore::INSTANCE_NUM; ++i) {
        std::stringstream ss;
        ss << i;
        w.Key(ss.str().c_str());
        w.StartObject();

        w.Key("first_binlog");
        w.Uint64(_logRecycStatus[i]->firstBinlogId);

        w.Key("incr_paused");
        w.Uint64(_incrPaused);

        w.Key("sync_dest");
        w.StartObject();
        // sync to
        for (auto& mpov : _pushStatus[i]) {
            std::stringstream ss;
            ss << "client_" << mpov.second->clientId;
            w.Key(ss.str().c_str());
            w.StartObject();
            w.Key("is_running");
            w.Uint64(mpov.second->isRunning);
            w.Key("dest_store_id");
            w.Uint64(mpov.second->dstStoreId);
            w.Key("binlog_pos");
            w.Uint64(mpov.second->binlogPos);
            w.Key("remote_host");
            if (mpov.second->client != nullptr) {
                w.String(mpov.second->client->getRemoteRepr());
            } else {
                w.String("???");
            }
            w.EndObject();
        }
        w.EndObject();

        // sync from
        w.Key("sync_source");
        ss.str("");
        ss << _syncMeta[i]->syncFromHost << ":"
           << _syncMeta[i]->syncFromPort << ":"
           << _syncMeta[i]->syncFromId;
        w.String(ss.str().c_str());
        w.Key("binlog_id");
        w.Uint64(_syncMeta[i]->binlogId);
        w.Key("repl_state");
        w.Uint64(static_cast<uint64_t>(_syncMeta[i]->replState));
        w.Key("last_sync_time");
        w.String(timePointRepr(_syncStatus[i]->lastSyncTime));

        w.EndObject();
    }
}

void ReplManager::stop() {
    LOG(WARNING) << "repl manager begins stops...";
    _isRunning.store(false, std::memory_order_relaxed);
    _controller->join();

    // make sure all workpool has been stopped; otherwise calling
    // the destructor of a std::thread that is running will crash
    _fullPusher->stop();
    _incrPusher->stop();
    _fullReceiver->stop();
    _incrChecker->stop();
    _logRecycler->stop();

    LOG(WARNING) << "repl manager stops succ";
}

}  // namespace tendisplus