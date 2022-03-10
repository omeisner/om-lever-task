#include "lever_system.hpp"
#include <cassert>

namespace om {

namespace {

using namespace lever;

LeverSystem::LocalInstance* find_local_instance(LeverSystem* sys, SerialLeverHandle handle) {
  for (auto& inst : sys->local_instances) {
    if (inst->handle == handle) {
      return inst.get();
    }
  }
  return nullptr;
}

LeverMessageData make_set_force_message(int force) {
  LeverMessageData result{};
  result.type = LeverMessageType::SetForce;
  result.force = force;
  return result;
}

LeverMessageData make_open_port_message(std::string&& port) {
  LeverMessageData result{};
  result.type = LeverMessageType::OpenPort;
  result.port = std::move(port);
  return result;
}

LeverMessageData make_close_port_message() {
  LeverMessageData result{};
  result.type = LeverMessageType::ClosePort;
  return result;
}

LeverMessageData make_port_status_message(SerialLeverHandle handle,
                                          SerialLeverError error, bool is_open) {
  LeverMessageData result{};
  result.type = LeverMessageType::PortStatus;
  result.handle = handle;
  result.error = error;
  result.is_open = is_open;
  return result;
}

LeverMessageData make_share_state_message(const LeverSystem::RemoteInstance& remote,
                                          SerialLeverHandle handle) {
  LeverMessageData message{};
  message.type = LeverMessageType::ShareState;
  message.force = remote.force;
  message.state = remote.state;
  message.handle = handle;
  message.is_open = is_open(remote.serial_context);
  return message;
}

bool process_remote_message(LeverSystem::RemoteInstance& remote, LeverMessageData&& data) {
  switch (data.type) {
    case LeverMessageType::SetForce: {
      remote.commanded_force = data.force.value();
      return false;
    }

    case LeverMessageType::OpenPort: {
      assert(!remote.open_response);
      remote = {};
      auto serial_res = om::make_context(
        data.port, om::default_baud_rate(), om::default_read_write_timeout());
#if 0
      std::this_thread::sleep_for(std::chrono::seconds(1));
      remote.open_response = SerialLeverError::FailedToOpen;
#else
      if (serial_res) {
        remote.serial_context = std::move(serial_res.value());
        remote.open_response = SerialLeverError::None;
      } else {
        remote.open_response = SerialLeverError::FailedToOpen;
      }
#endif
      return true;
    }

    case LeverMessageType::ClosePort: {
      remote = {};
      return true;
    }

    default: {
      assert(false);
      return false;
    }
  }
}

void process_remote_instance(LeverSystem* system, LeverSystem::RemoteInstance& remote,
                             LeverSystem::LocalInstance& local) {
  if (auto data = read(&local.message)) {
    if (process_remote_message(remote, std::move(data.value()))) {
      remote.need_send_state = true;
    }
  }

  const bool open = is_open(remote.serial_context);
  if (remote.open_response) {
    auto message = make_port_status_message(local.handle, remote.open_response.value(), open);
    if (system->read_remote.maybe_write(message)) {
      remote.open_response = std::nullopt;
    }
  }

  if (open) {
    remote.need_send_state = true;

    if (auto resp = om::set_force_grams(remote.serial_context, remote.commanded_force)) {
      remote.force = remote.commanded_force;
    } else {
      remote.force = std::nullopt;
    }

    if (auto state = om::read_state(remote.serial_context)) {
      remote.state = state.value();
    } else {
      remote.state = std::nullopt;
    }
  }

  if (remote.need_send_state) {
    auto message = make_share_state_message(remote, local.handle);
    if (system->read_remote.maybe_write(message)) {
      remote.need_send_state = false;
    }
  }
}

void worker(LeverSystem* system) {
  while (system->keep_processing.load()) {
    for (int i = 0; i < int(system->remote_instances.size()); i++) {
      process_remote_instance(
        system, *system->remote_instances[i], *system->local_instances[i]);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

std::unique_ptr<LeverSystem::LocalInstance> make_local_instance(SerialLeverHandle handle) {
  auto result = std::make_unique<LeverSystem::LocalInstance>();
  result->handle = handle;
  return result;
}

std::unique_ptr<LeverSystem::RemoteInstance> make_remote_instance() {
  return std::make_unique<LeverSystem::RemoteInstance>();
}

} //  anon

std::vector<SerialLeverHandle> lever::initialize(LeverSystem* sys, int max_num_levers) {
  std::vector<SerialLeverHandle> result;

  for (int i = 0; i < max_num_levers; i++) {
    SerialLeverHandle handle{sys->instance_id++};
    sys->local_instances.emplace_back() = make_local_instance(handle);
    sys->remote_instances.emplace_back() = make_remote_instance();
    result.push_back(handle);
  }

  sys->keep_processing.store(true);
  sys->worker_thread = std::thread{[sys]() {
    worker(sys);
  }};

  return result;
}

void lever::terminate(LeverSystem* sys) {
  sys->keep_processing.store(false);
  if (sys->worker_thread.joinable()) {
    sys->worker_thread.join();
  }
  sys->local_instances.clear();
  sys->remote_instances.clear();
}

void lever::update(LeverSystem* system) {
  for (auto& inst : system->local_instances) {
    if (inst->message.awaiting_read) {
      (void) acknowledged(&inst->message);
    }

    if (inst->pending_open_port && !inst->message.awaiting_read) {
      auto data = make_open_port_message(std::move(inst->pending_open_port.value()));
      publish(&inst->message, std::move(data));
      inst->pending_open_port = std::nullopt;
    }

    if (inst->pending_close_port && !inst->message.awaiting_read) {
      publish(&inst->message, make_close_port_message());
      inst->pending_close_port = false;
    }

    if (inst->pending_canonical_force && !inst->message.awaiting_read) {
      auto data = make_set_force_message(inst->pending_canonical_force.value());
      publish(&inst->message, std::move(data));
      inst->pending_canonical_force = std::nullopt;
    }
  }

  const int num_read = system->read_remote.size();
  for (int i = 0; i < num_read; i++) {
    auto response = system->read_remote.read();
    if (response.type == LeverMessageType::ShareState) {
      if (auto* inst = find_local_instance(system, response.handle)) {
        inst->canonical_force = response.force;
        inst->state = response.state;
        inst->is_open = response.is_open;
      }

    } else if (response.type == LeverMessageType::PortStatus) {
      if (auto* inst = find_local_instance(system, response.handle)) {
        assert(inst->awaiting_open);
        inst->awaiting_open = false;
        inst->is_open = response.is_open;
      }
    }
  }
}

void lever::set_force(LeverSystem* system, SerialLeverHandle instance, int grams) {
  if (auto* inst = find_local_instance(system, instance)) {
    inst->pending_canonical_force = grams;
    inst->commanded_force = grams;
  } else {
    assert(false);
  }
}

void lever::open_connection(LeverSystem* system, SerialLeverHandle handle,
                            const std::string& port) {
  if (auto* inst = find_local_instance(system, handle)) {
    inst->pending_open_port = port;
    inst->awaiting_open = true;
  } else {
    assert(false);
  }
}

bool lever::is_pending_open(LeverSystem* system, SerialLeverHandle handle) {
  if (auto* inst = find_local_instance(system, handle)) {
    return inst->awaiting_open;
  } else {
    assert(false);
    return false;
  }
}

bool lever::is_open(LeverSystem* system, SerialLeverHandle handle) {
  if (auto* inst = find_local_instance(system, handle)) {
    return inst->is_open;
  } else {
    assert(false);
    return false;
  }
}

void lever::close_connection(LeverSystem* system, SerialLeverHandle handle) {
  if (auto* inst = find_local_instance(system, handle)) {
    inst->pending_close_port = true;
  } else {
    assert(false);
  }
}

int lever::get_commanded_force(LeverSystem* system, SerialLeverHandle instance) {
  if (auto* inst = find_local_instance(system, instance)) {
    return inst->commanded_force;
  } else {
    assert(false);
    return 0;
  }
}

std::optional<int> lever::get_canonical_force(LeverSystem* system, SerialLeverHandle instance) {
  if (auto* inst = find_local_instance(system, instance)) {
    return inst->canonical_force;
  } else {
    assert(false);
    return std::nullopt;
  }
}

std::optional<LeverState> lever::get_state(LeverSystem* system, SerialLeverHandle instance) {
  if (auto* inst = find_local_instance(system, instance)) {
    return inst->state;
  } else {
    assert(false);
    return std::nullopt;
  }
}

}
