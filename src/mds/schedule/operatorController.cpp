/*
 *  Copyright (c) 2020 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
* Created Date: Mon Nov 19 2018
 * Author: lixiaocui
 */

#include <glog/logging.h>
#include <memory>
#include "src/mds/schedule/operatorController.h"
#include "src/mds/schedule/scheduleMetrics.h"

namespace curve {
namespace mds {
namespace schedule {
OperatorController::OperatorController(
    int concurrent, std::shared_ptr<ScheduleMetrics> metrics) {
    this->operatorConcurrent_ = concurrent;
    this->metrics_ = metrics;
}

bool OperatorController::AddOperator(const Operator &op) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto exist = operators_.find(op.copysetID);
    // no operator exist
    if (exist == operators_.end()) {
        // concurrency exceed
        if (!AddOpInfluencePreJudgeLocked(op)) {
            LOG(INFO) << "add operator " << op.OpToString()
                      << " fail because of oncurrency exceed";
            return false;
        }
        operators_[op.copysetID] = op;
        UpdateAddOpInfluenceLocked(op);
        metrics_->UpdateAddMetric(op);
        return true;
    }

    // operator priority is higher, just replace the low-pri operator
    // this is safe, because if the low-pri operator is executing,
    // the replaced-high-pri operator will be failed and removed, otherwise the
    // replaced-high-pri operator will be executed.
    if (exist->second.priority < op.priority) {
        if (!ReplaceOpInfluencePreJudgeLocked(exist->second, op)) {
            LOG(ERROR) << "replace operator on copyset("
                       << op.copysetID.first << ","
                       << op.copysetID.second
                       << ") fail because do new operator "
                          "do not satisfy opInfluence condition";
            return false;
        } else {
            operators_[op.copysetID] = op;
            UpdateReplaceOpInfluenceLocked(exist->second, op);
            metrics_->UpdateAddMetric(op);
            return true;
        }
    }

    return false;
}

void OperatorController::RemoveOperator(const CopySetKey &key) {
    std::lock_guard<std::mutex> guard(mutex_);
    RemoveOperatorLocked(key);
}

void OperatorController::RemoveOperatorLocked(const CopySetKey &key) {
    auto exist = operators_.find(key);
    if (exist == operators_.end()) {
        return;
    }
    UpdateRemoveOpInfluenceLocked(exist->second);
    metrics_->UpdateRemoveMetric(exist->second);
    operators_.erase(key);
}

std::vector<Operator> OperatorController::GetOperators() {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Operator> ops;
    for (auto &op : operators_) {
        ops.emplace_back(op.second);
    }
    return ops;
}

bool OperatorController::GetOperatorById(const CopySetKey &id, Operator *op) {
    assert(op != nullptr);

    std::lock_guard<std::mutex> guard(mutex_);
    if (operators_.find(id) == operators_.end()) {
        return false;
    }
    *op = operators_[id];
    return true;
}

bool OperatorController::ChunkServerExceed(ChunkServerIdType id) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (opInfluence_.find(id) == opInfluence_.end()) {
        return false;
    }
    return opInfluence_[id] >= operatorConcurrent_;
}

bool OperatorController::ApplyOperator(const CopySetInfo &originInfo,
                                       CopySetConf *newConf) {
    assert(newConf != nullptr);

    std::lock_guard<std::mutex> guard(mutex_);
    // no operator
    if (operators_.find(originInfo.id) == operators_.end()) {
        return false;
    }

    // operator timeout or finish
    if (operators_[originInfo.id].IsTimeout()) {
        LOG(ERROR) << "apply operator: "
                   << operators_[originInfo.id].OpToString()
                   << " on " << originInfo.CopySetInfoStr()
                   << " fail, operator is timeout";
        RemoveOperatorLocked(originInfo.id);
        return false;
    } else {
        auto res = operators_[originInfo.id].Apply(originInfo, newConf);
        switch (res) {
            case ApplyStatus::Failed:
                LOG(ERROR) << "apply operator"
                           << operators_[originInfo.id].OpToString()
                           << " on " << originInfo.CopySetInfoStr()
                           << " failed";
                RemoveOperatorLocked(originInfo.id);
                return false;
            case ApplyStatus::Finished:
                LOG(INFO) << "apply operator "
                          << operators_[originInfo.id].OpToString()
                          << " on "<< originInfo.CopySetInfoStr() << " ok";
                RemoveOperatorLocked(originInfo.id);
                return false;
            case ApplyStatus::Ordered:return true;
            case ApplyStatus::OnGoing:return false;
        }
    }
}

void OperatorController::UpdateReplaceOpInfluenceLocked(const Operator &oldOp,
                                                        const Operator &newOp) {
    UpdateRemoveOpInfluenceLocked(oldOp);
    UpdateAddOpInfluenceLocked(newOp);
}

void OperatorController::UpdateAddOpInfluenceLocked(const Operator &op) {
    for (auto csId : op.AffectedChunkServers()) {
        opInfluence_[csId]++;
    }
}

void OperatorController::UpdateRemoveOpInfluenceLocked(const Operator &op) {
    for (auto csId : op.AffectedChunkServers()) {
        opInfluence_[csId]--;
    }
}

bool OperatorController::ReplaceOpInfluencePreJudgeLocked(
    const Operator &oldOp, const Operator &newOp) {
    std::map<ChunkServerIdType, int> influenceList;
    for (auto csId : oldOp.AffectedChunkServers()) {
        if (influenceList.find(csId) == influenceList.end()) {
            influenceList[csId] = -1;
        } else {
            influenceList[csId] -= 1;
        }
    }

    for (auto csId : newOp.AffectedChunkServers()) {
         influenceList[csId]++;
    }

    for (auto csId : influenceList) {
        if (opInfluence_[csId.first] + csId.second > operatorConcurrent_) {
            return false;
        }
    }
    return true;
}

bool OperatorController::AddOpInfluencePreJudgeLocked(const Operator &op) {
    std::map<ChunkServerIdType, int> influenceList;
    for (auto csId : op.AffectedChunkServers()) {
         influenceList[csId]++;
    }
    for (auto csId : influenceList) {
        if (opInfluence_[csId.first] + csId.second > operatorConcurrent_) {
            return false;
        }
    }
    return true;
}
}  // namespace schedule
}  // namespace mds
}  // namespace curve
