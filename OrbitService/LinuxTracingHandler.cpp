// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "LinuxTracingHandler.h"

#include "absl/flags/flag.h"
#include "llvm/Demangle/Demangle.h"

namespace orbit_service {

using orbit_grpc_protos::AddressInfo;
using orbit_grpc_protos::Callstack;
using orbit_grpc_protos::CallstackSample;
using orbit_grpc_protos::CaptureEvent;
using orbit_grpc_protos::CaptureOptions;
using orbit_grpc_protos::FunctionCall;
using orbit_grpc_protos::GpuJob;
using orbit_grpc_protos::IntrospectionScope;
using orbit_grpc_protos::SchedulingSlice;
using orbit_grpc_protos::ThreadName;
using orbit_grpc_protos::ThreadStateSlice;

void LinuxTracingHandler::Start(CaptureOptions capture_options) {
  CHECK(tracer_ == nullptr);
  CHECK(!sender_thread_.joinable());
  bool enable_introspection = capture_options.enable_introspection();

  {
    // Protect tracer_ with event_buffer_mutex_ so that we can use tracer_ in
    // Conditions for Await/LockWhen (specifically, in SenderThread).
    absl::MutexLock lock{&event_buffer_mutex_};
    tracer_ = std::make_unique<LinuxTracing::Tracer>(std::move(capture_options));
  }
  tracer_->SetListener(this);
  tracer_->Start();

  sender_thread_ = std::thread{[this] { SenderThread(); }};

  if (enable_introspection) {
    SetupIntrospection();
  }
}

void LinuxTracingHandler::SetupIntrospection() {
  orbit_tracing_listener_ =
      std::make_unique<orbit::tracing::Listener>([this](const orbit::tracing::Scope& scope) {
        IntrospectionScope introspection_scope;
        introspection_scope.set_pid(getpid());
        introspection_scope.set_tid(scope.tid);
        introspection_scope.set_begin_timestamp_ns(scope.begin);
        introspection_scope.set_end_timestamp_ns(scope.end);
        introspection_scope.set_depth(scope.depth);
        introspection_scope.mutable_registers()->Reserve(6);
        introspection_scope.add_registers(scope.encoded_event.args[0]);
        introspection_scope.add_registers(scope.encoded_event.args[1]);
        introspection_scope.add_registers(scope.encoded_event.args[2]);
        introspection_scope.add_registers(scope.encoded_event.args[3]);
        introspection_scope.add_registers(scope.encoded_event.args[4]);
        introspection_scope.add_registers(scope.encoded_event.args[5]);
        OnIntrospectionScope(introspection_scope);
      });
}

void LinuxTracingHandler::Stop() {
  CHECK(tracer_ != nullptr);
  CHECK(sender_thread_.joinable());

  tracer_->Stop();
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    tracer_.reset();
  }

  sender_thread_.join();
}

void LinuxTracingHandler::OnSchedulingSlice(SchedulingSlice scheduling_slice) {
  CaptureEvent event;
  *event.mutable_scheduling_slice() = std::move(scheduling_slice);
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
}

void LinuxTracingHandler::OnCallstackSample(CallstackSample callstack_sample) {
  CHECK(callstack_sample.callstack_or_key_case() == CallstackSample::kCallstack);
  callstack_sample.set_callstack_key(
      InternCallstackIfNecessaryAndGetKey(callstack_sample.callstack()));

  CaptureEvent event;
  *event.mutable_callstack_sample() = std::move(callstack_sample);
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
}

void LinuxTracingHandler::OnFunctionCall(FunctionCall function_call) {
  CaptureEvent event;
  *event.mutable_function_call() = std::move(function_call);
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
}

void LinuxTracingHandler::OnIntrospectionScope(
    orbit_grpc_protos::IntrospectionScope introspection_scope) {
  CaptureEvent event;
  *event.mutable_introspection_scope() = std::move(introspection_scope);
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
}

void LinuxTracingHandler::OnGpuJob(GpuJob gpu_job) {
  CHECK(gpu_job.timeline_or_key_case() == GpuJob::kTimeline);
  gpu_job.set_timeline_key(
      InternStringIfNecessaryAndGetKey(std::move(*gpu_job.mutable_timeline())));

  CaptureEvent event;
  *event.mutable_gpu_job() = std::move(gpu_job);
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
}

void LinuxTracingHandler::OnThreadName(ThreadName thread_name) {
  CaptureEvent event;
  *event.mutable_thread_name() = std::move(thread_name);
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
}

void LinuxTracingHandler::OnThreadStateSlice(ThreadStateSlice thread_state_slice) {
  CaptureEvent event;
  *event.mutable_thread_state_slice() = std::move(thread_state_slice);
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
}

void LinuxTracingHandler::OnAddressInfo(AddressInfo address_info) {
  {
    absl::MutexLock lock{&addresses_seen_mutex_};
    if (addresses_seen_.contains(address_info.absolute_address())) {
      return;
    }
    addresses_seen_.emplace(address_info.absolute_address());
  }

  CHECK(address_info.function_name_or_key_case() == AddressInfo::kFunctionName);
  address_info.set_function_name_key(
      InternStringIfNecessaryAndGetKey(llvm::demangle(address_info.function_name())));
  CHECK(address_info.map_name_or_key_case() == AddressInfo::kMapName);
  address_info.set_map_name_key(
      InternStringIfNecessaryAndGetKey(std::move(*address_info.mutable_map_name())));

  CaptureEvent event;
  *event.mutable_address_info() = std::move(address_info);
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
}

void LinuxTracingHandler::OnTracepointEvent(orbit_grpc_protos::TracepointEvent tracepoint_event) {
  CHECK(tracepoint_event.tracepoint_info_or_key_case() ==
        orbit_grpc_protos::TracepointEvent::kTracepointInfo);
  tracepoint_event.set_tracepoint_info_key(
      InternTracepointInfoIfNecessaryAndGetKey(tracepoint_event.tracepoint_info()));

  CaptureEvent event;
  *event.mutable_tracepoint_event() = std::move(tracepoint_event);
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
}

uint64_t LinuxTracingHandler::ComputeCallstackKey(const Callstack& callstack) {
  uint64_t key = 17;
  for (uint64_t pc : callstack.pcs()) {
    key = 31 * key + pc;
  }
  return key;
}

uint64_t LinuxTracingHandler::InternCallstackIfNecessaryAndGetKey(Callstack callstack) {
  uint64_t key = ComputeCallstackKey(callstack);
  {
    absl::MutexLock lock{&callstack_keys_sent_mutex_};
    if (callstack_keys_sent_.contains(key)) {
      return key;
    }
    callstack_keys_sent_.emplace(key);
  }

  CaptureEvent event;
  event.mutable_interned_callstack()->set_key(key);
  *event.mutable_interned_callstack()->mutable_intern() = std::move(callstack);
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
  return key;
}

uint64_t LinuxTracingHandler::ComputeStringKey(const std::string& str) {
  return std::hash<std::string>{}(str);
}

uint64_t LinuxTracingHandler::InternStringIfNecessaryAndGetKey(std::string str) {
  uint64_t key = ComputeStringKey(str);
  {
    absl::MutexLock lock{&string_keys_sent_mutex_};
    if (string_keys_sent_.contains(key)) {
      return key;
    }
    string_keys_sent_.emplace(key);
  }

  CaptureEvent event;
  event.mutable_interned_string()->set_key(key);
  event.mutable_interned_string()->set_intern(std::move(str));
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
  return key;
}

uint64_t LinuxTracingHandler::InternTracepointInfoIfNecessaryAndGetKey(
    orbit_grpc_protos::TracepointInfo tracepoint_info) {
  uint64_t key =
      ComputeStringKey(absl::StrCat(tracepoint_info.category(), ":", tracepoint_info.name()));
  {
    absl::MutexLock lock{&tracepoint_keys_sent_mutex_};
    if (tracepoint_keys_sent_.contains(key)) {
      return key;
    }
    tracepoint_keys_sent_.emplace(key);
  }

  CaptureEvent event;
  event.mutable_interned_tracepoint_info()->set_key(key);
  event.mutable_interned_tracepoint_info()->mutable_intern()->set_name(tracepoint_info.name());
  event.mutable_interned_tracepoint_info()->mutable_intern()->set_category(
      tracepoint_info.category());
  {
    absl::MutexLock lock{&event_buffer_mutex_};
    event_buffer_.emplace_back(std::move(event));
  }
  return key;
}

void LinuxTracingHandler::SenderThread() {
  pthread_setname_np(pthread_self(), "SenderThread");
  constexpr absl::Duration kSendTimeInterval = absl::Milliseconds(20);
  // This should be lower than kMaxEventsPerResponse in SendBufferedEvents as
  // a few more events are likely to arrive after the condition becomes true.
  constexpr uint64_t kSendEventCountInterval = 5000;

  bool stopped = false;
  while (!stopped) {
    ORBIT_SCOPE("SenderThread iteration");
    event_buffer_mutex_.LockWhenWithTimeout(absl::Condition(
                                                +[](LinuxTracingHandler* self) {
                                                  return self->event_buffer_.size() >=
                                                             kSendEventCountInterval ||
                                                         self->tracer_ == nullptr;
                                                },
                                                this),
                                            kSendTimeInterval);
    if (tracer_ == nullptr) {
      stopped = true;
    }
    std::vector<CaptureEvent> buffered_events = std::move(event_buffer_);
    event_buffer_.clear();
    event_buffer_mutex_.Unlock();
    capture_response_listener_->ProcessEvents(std::move(buffered_events));
  }
}

}  // namespace orbit_service
