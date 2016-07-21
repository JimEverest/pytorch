#include "caffe2/core/workspace.h"

#include <algorithm>
#include <ctime>

#include "caffe2/core/logging.h"
#include "caffe2/core/operator.h"
#include "caffe2/core/net.h"
#include "caffe2/core/timer.h"
#include "caffe2/proto/caffe2.pb.h"

namespace caffe2 {

namespace {
// Returns a function that returns `true` if we should continue
// iterating, given the current iteration count.
std::function<bool(int)> getContinuationTest(
    Workspace* ws,
    const ExecutionStep& step) {
  if (step.has_criteria_network()) {
    CHECK(!step.has_num_iter())
        << "Must not specify num_iter if critera_network is set";
  }

  if (!step.has_criteria_network()) {
    int iterations = step.has_num_iter() ? step.num_iter() : 1;
    VLOG(1) << "Executing step for " << iterations << " iterations.";
    return [=](int i) { return i < iterations; };
  }
  auto* criteria_network = ws->GetNet(step.criteria_network());
  CHECK_NOTNULL(criteria_network);
  CHECK_EQ(criteria_network->external_output().size(), 1);
  const auto& criteria_output = criteria_network->external_output().front();
  VLOG(1) << "Executing step controlled by criteria output: "
                << criteria_output;
  return [=](int) {
    criteria_network->Run();
    const auto& blob = ws->GetBlob(criteria_output)->Get<TensorCPU>();
    CHECK_EQ(blob.size(), 1);
    CHECK(blob.IsType<bool>());
    return blob.template data<bool>()[0] > 0;
  };
};
}  // namespace

vector<string> Workspace::Blobs() {
  vector<string> names;
  for (auto& entry : blob_map_) {
    names.push_back(entry.first);
  }
  if (shared_) {
    vector<string> shared_blobs = shared_->Blobs();
    names.insert(names.end(), shared_blobs.begin(), shared_blobs.end());
  }
  return names;
}

Blob* Workspace::CreateBlob(const string& name) {
  if (HasBlob(name)) {
    VLOG(1) << "Blob " << name << " already exists. Skipping.";
  } else {
    VLOG(1) << "Creating blob " << name;
    blob_map_[name] = unique_ptr<Blob>(new Blob());
  }
  return GetBlob(name);
}

const Blob* Workspace::GetBlob(const string& name) const {
  if (blob_map_.count(name)) {
    return blob_map_.at(name).get();
  } else if (shared_ && shared_->HasBlob(name)) {
    return shared_->GetBlob(name);
  } else {
    LOG(WARNING) << "Blob " << name << " not in the workspace.";
    // TODO(Yangqing): do we want to always print out the list of blobs here?
    // LOG(WARNING) << "Current blobs:";
    // for (const auto& entry : blob_map_) {
    //   LOG(WARNING) << entry.first;
    // }
    return nullptr;
  }
}

Blob* Workspace::GetBlob(const string& name) {
  return const_cast<Blob*>(
      static_cast<const Workspace*>(this)->GetBlob(name));
}

NetBase* Workspace::CreateNet(const NetDef& net_def) {
  CHECK(net_def.has_name()) << "Net definition should have a name.";
  if (net_map_.count(net_def.name()) > 0) {
    LOG(WARNING) << "Overwriting existing network of the same name.";
    // Note(Yangqing): Why do we explicitly erase it here? Some components of
    // the old network, such as a opened LevelDB, may prevent us from creating a
    // new network before the old one is deleted. Thus we will need to first
    // erase the old one before the new one can be constructed.
    net_map_.erase(net_def.name());
  }
  // Create a new net with its name.
  LOG(INFO) << "Initializing network " << net_def.name();
  net_map_[net_def.name()] =
      unique_ptr<NetBase>(caffe2::CreateNet(net_def, this));
  if (net_map_[net_def.name()].get() == nullptr) {
    LOG(ERROR) << "Error when creating the network.";
    net_map_.erase(net_def.name());
    return nullptr;
  }
  if (!net_map_[net_def.name()]->Verify()) {
    LOG(ERROR) << "Error when setting up network " << net_def.name();
    net_map_.erase(net_def.name());
    return nullptr;
  }
  return net_map_[net_def.name()].get();
}

NetBase* Workspace::GetNet(const string& name) {
  if (!net_map_.count(name)) {
    return nullptr;
  } else {
    return net_map_[name].get();
  }
}

void Workspace::DeleteNet(const string& name) {
  if (net_map_.count(name)) {
    net_map_.erase(name);
  }
}

bool Workspace::RunNet(const string& name) {
  if (!net_map_.count(name)) {
    LOG(ERROR) << "Network " << name << " does not exist yet.";
    return false;
  }
  return net_map_[name]->Run();
}

bool Workspace::RunOperatorOnce(const OperatorDef& op_def) {
  std::unique_ptr<OperatorBase> op(CreateOperator(op_def, this));
  if (op.get() == nullptr) {
    LOG(ERROR) << "Cannot create operator of type " << op_def.type();
    return false;
  }
  if (!op->Run()) {
    LOG(ERROR) << "Error when running operator " << op_def.type();
    return false;
  }
  return true;
}
bool Workspace::RunNetOnce(const NetDef& net_def) {
  std::unique_ptr<NetBase> net(caffe2::CreateNet(net_def, this));
  if (!net->Verify()) {
    LOG(ERROR) << "Error when setting up network " << net_def.name();
    return false;
  }
  if (!net->Run()) {
    LOG(ERROR) << "Error when running network " << net_def.name();
    return false;
  }
  return true;
}

bool Workspace::RunPlan(const PlanDef& plan,
                        ShouldContinue shouldContinue) {
  LOG(INFO) << "Started executing plan.";
  if (plan.execution_step_size() == 0) {
    LOG(WARNING) << "Nothing to run - did you define a correct plan?";
    // We will do nothing, but the plan is still legal so we will return true.
    return true;
  }
  LOG(INFO) << "Initializing networks.";

  for (const NetDef& net_def : plan.network()) {
    if (!CreateNet(net_def)) {
      LOG(ERROR) << "Failed initializing the networks.";
      return false;
    }
  }
  Timer plan_timer;
  for (const ExecutionStep& step : plan.execution_step()) {
    Timer step_timer;
    if (!ExecuteStepRecursive(step, shouldContinue)) {
      LOG(ERROR) << "Failed initializing step " << step.name();
      return false;
    }
    LOG(INFO) << "Step " << step.name() << " took " << step_timer.Seconds()
                   << " seconds.";
  }
  LOG(INFO) << "Total plan took " << plan_timer.Seconds() << " seconds.";
  LOG(INFO) << "Plan executed successfully.";
  return true;
}

namespace {

struct Reporter {
  void start(NetBase* net, int reportInterval) {
    auto interval = std::chrono::seconds(reportInterval);
    auto reportWorker = [=]() {
      std::unique_lock<std::mutex> lk(report_mutex);
      do {
        report_cv.wait_for(lk, interval);
        if (!net->Run()) {
          LOG(WARNING) << "Error running report_net.";
        }
      } while (!done);
    };
    report_thread = std::thread(reportWorker);
  }

  ~Reporter() {
    if (!report_thread.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lk(report_mutex);
      done = true;
    }
    report_cv.notify_all();
    report_thread.join();
  }

 private:
  std::mutex report_mutex;
  std::condition_variable report_cv;
  bool done{false};
  std::thread report_thread;
};

}

bool Workspace::ExecuteStepRecursive(
      const ExecutionStep& step,
      ShouldContinue externalShouldContinue) {
  LOG(INFO) << "Running execution step " << step.name();
  if (!(step.substep_size() == 0 || step.network_size() == 0)) {
    LOG(ERROR) << "An ExecutionStep should either have substep or networks "
               << "but not both.";
    return false;
  }

  Reporter reporter;
  if (step.has_report_net()) {
    CAFFE_ENFORCE(
        step.has_report_interval(),
        "A report_interval must be provided if report_net is set.");
    if (net_map_.count(step.report_net()) == 0) {
      LOG(ERROR) << "Report net " << step.report_net() << " not found.";
    }
    reporter.start(net_map_[step.report_net()].get(), step.report_interval());
  }

  const auto netShouldContinue = getContinuationTest(this, step);
  const auto shouldContinue = [&](int iter) {
    return externalShouldContinue(iter) && netShouldContinue(iter);
  };
  if (step.substep_size()) {
    for (int iter = 0; shouldContinue(iter); ++iter) {
      // we assume that, if we have substeps, each substep is going to take a
      // reasonable amount of time, so logging here is fine
      LOG(INFO) << "Execution step " << step.name()
                << ": Starting iteration " << iter;
      std::atomic<int> next_substep{0};
      std::atomic<bool> got_failure{false};
      auto substepShouldContinue = [&, externalShouldContinue](int iter) {
        return !got_failure && externalShouldContinue(iter);
      };
      auto worker = [&]() {
        while (true) {
          int substep_id = next_substep++;
          if (got_failure || (substep_id >= step.substep().size())) {
            break;
          }
          if (!ExecuteStepRecursive(step.substep().Get(substep_id),
                                    substepShouldContinue)) {
            got_failure = true;
          }
        }
      };
      if (!step.concurrent_substeps() || step.substep().size() <= 1) {
        worker();
      } else {
        std::vector<std::thread> threads;
        for (int i = 0; i < step.substep().size(); ++i) {
          threads.emplace_back(worker);
        }
        for (auto& thread: threads) {
          thread.join();
        }
      }
      if (got_failure) {
        return false;
      }
    }
    return true;
  } else {
    // If this ExecutionStep just contains nets, we can directly run it.
    vector<NetBase*> networks;
    // Collect the networks to run.
    for (const string& network_name : step.network()) {
      if (!net_map_.count(network_name)) {
        LOG(ERROR) << "Network " << network_name << " not found.";
        return false;
      }
      VLOG(1) << "Going to execute network " << network_name;
      networks.push_back(net_map_[network_name].get());
    }
    for (int iter = 0; shouldContinue(iter); ++iter) {
      VLOG(1) << "Executing network iteration " << iter;
      for (NetBase* network : networks) {
        if (!network->Run()) {
          return false;
        }
      }
    }
  }
  return true;
}

}  // namespace caffe2
