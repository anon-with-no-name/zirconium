// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "content/browser/tracing/tracing_controller_impl.h"

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/json/string_escape.h"
#include "base/macros.h"
#include "base/strings/string_number_conversions.h"
#include "base/trace_event/trace_event.h"
#include "content/browser/tracing/trace_message_filter.h"
#include "content/browser/tracing/tracing_ui.h"
#include "content/common/child_process_messages.h"
#include "content/public/browser/browser_message_filter.h"
#include "content/public/common/content_switches.h"

#if defined(OS_CHROMEOS)
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/debug_daemon_client.h"
#endif

#if defined(OS_WIN)
#include "content/browser/tracing/etw_system_event_consumer_win.h"
#endif

using base::trace_event::TraceLog;
using base::trace_event::TraceOptions;
using base::trace_event::CategoryFilter;

namespace content {

namespace {

base::LazyInstance<TracingControllerImpl>::Leaky g_controller =
    LAZY_INSTANCE_INITIALIZER;

class FileTraceDataSink : public TracingController::TraceDataSink {
 public:
  explicit FileTraceDataSink(const base::FilePath& trace_file_path,
                             const base::Closure& callback)
      : file_path_(trace_file_path),
        completion_callback_(callback),
        file_(NULL) {}

  void AddTraceChunk(const std::string& chunk) override {
    std::string tmp = chunk;
    scoped_refptr<base::RefCountedString> chunk_ptr =
        base::RefCountedString::TakeString(&tmp);
    BrowserThread::PostTask(
        BrowserThread::FILE,
        FROM_HERE,
        base::Bind(
            &FileTraceDataSink::AddTraceChunkOnFileThread, this, chunk_ptr));
  }
  void SetSystemTrace(const std::string& data) override {
    system_trace_ = data;
  }
  void Close() override {
    BrowserThread::PostTask(
        BrowserThread::FILE,
        FROM_HERE,
        base::Bind(&FileTraceDataSink::CloseOnFileThread, this));
  }

 private:
  ~FileTraceDataSink() override { DCHECK(file_ == NULL); }

  void AddTraceChunkOnFileThread(
      const scoped_refptr<base::RefCountedString> chunk) {
    if (file_ != NULL)
      fputc(',', file_);
    else if (!OpenFileIfNeededOnFileThread())
      return;
    ignore_result(fwrite(chunk->data().c_str(), strlen(chunk->data().c_str()),
        1, file_));
  }

  bool OpenFileIfNeededOnFileThread() {
    if (file_ != NULL)
      return true;
    file_ = base::OpenFile(file_path_, "w");
    if (file_ == NULL) {
      LOG(ERROR) << "Failed to open " << file_path_.value();
      return false;
    }
    const char preamble[] = "{\"traceEvents\": [";
    ignore_result(fwrite(preamble, strlen(preamble), 1, file_));
    return true;
  }

  void CloseOnFileThread() {
    if (OpenFileIfNeededOnFileThread()) {
      fputc(']', file_);
      if (!system_trace_.empty()) {
        const char systemTraceEvents[] = ",\"systemTraceEvents\": ";
        ignore_result(fwrite(systemTraceEvents, strlen(systemTraceEvents),
            1, file_));
        ignore_result(fwrite(system_trace_.c_str(),
            strlen(system_trace_.c_str()), 1, file_));
      }
      fputc('}', file_);
      base::CloseFile(file_);
      file_ = NULL;
    }
    BrowserThread::PostTask(
        BrowserThread::UI,
        FROM_HERE,
        base::Bind(&FileTraceDataSink::FinalizeOnUIThread, this));
  }

  void FinalizeOnUIThread() { completion_callback_.Run(); }

  base::FilePath file_path_;
  base::Closure completion_callback_;
  FILE* file_;
  std::string system_trace_;

  DISALLOW_COPY_AND_ASSIGN(FileTraceDataSink);
};

class StringTraceDataSink : public TracingController::TraceDataSink {
 public:
  typedef base::Callback<void(base::RefCountedString*)> CompletionCallback;

  explicit StringTraceDataSink(CompletionCallback callback)
      : completion_callback_(callback) {}

  // TracingController::TraceDataSink implementation
  void AddTraceChunk(const std::string& chunk) override {
    if (!trace_.empty())
      trace_ += ",";
    trace_ += chunk;
  }
  void SetSystemTrace(const std::string& data) override {
    system_trace_ = data;
  }
  void Close() override {
    std::string result = "{\"traceEvents\":[" + trace_ + "]";
    if (!system_trace_.empty())
      result += ",\"systemTraceEvents\": " + system_trace_;
    result += "}";

    scoped_refptr<base::RefCountedString> str =
        base::RefCountedString::TakeString(&result);
    completion_callback_.Run(str.get());
  }

 private:
  ~StringTraceDataSink() override {}

  std::string trace_;
  std::string system_trace_;
  CompletionCallback completion_callback_;

  DISALLOW_COPY_AND_ASSIGN(StringTraceDataSink);
};

}  // namespace

TracingController* TracingController::GetInstance() {
  return TracingControllerImpl::GetInstance();
}

TracingControllerImpl::TracingControllerImpl()
    : pending_disable_recording_ack_count_(0),
      pending_capture_monitoring_snapshot_ack_count_(0),
      pending_trace_log_status_ack_count_(0),
      maximum_trace_buffer_usage_(0),
      approximate_event_count_(0),
    // Tracing may have been enabled by ContentMainRunner if kTraceStartup
    // is specified in command line.
#if defined(OS_CHROMEOS) || defined(OS_WIN)
      is_system_tracing_(false),
#endif
      is_recording_(TraceLog::GetInstance()->IsEnabled()),
      is_monitoring_(false) {
  base::trace_event::MemoryDumpManager::GetInstance()->SetDelegate(this);
}

TracingControllerImpl::~TracingControllerImpl() {
  // This is a Leaky instance.
  NOTREACHED();
}

TracingControllerImpl* TracingControllerImpl::GetInstance() {
  return g_controller.Pointer();
}

bool TracingControllerImpl::GetCategories(
    const GetCategoriesDoneCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // Known categories come back from child processes with the EndTracingAck
  // message. So to get known categories, just begin and end tracing immediately
  // afterwards. This will ping all the child processes for categories.
  pending_get_categories_done_callback_ = callback;
  if (!EnableRecording(
          CategoryFilter("*"), TraceOptions(), EnableRecordingDoneCallback())) {
    pending_get_categories_done_callback_.Reset();
    return false;
  }

  bool ok = DisableRecording(NULL);
  DCHECK(ok);
  return true;
}

void TracingControllerImpl::SetEnabledOnFileThread(
    const CategoryFilter& category_filter,
    int mode,
    const TraceOptions& trace_options,
    const base::Closure& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::FILE);

  TraceLog::GetInstance()->SetEnabled(
      category_filter, static_cast<TraceLog::Mode>(mode), trace_options);
  BrowserThread::PostTask(BrowserThread::UI, FROM_HERE, callback);
}

void TracingControllerImpl::SetDisabledOnFileThread(
    const base::Closure& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::FILE);

  TraceLog::GetInstance()->SetDisabled();
  BrowserThread::PostTask(BrowserThread::UI, FROM_HERE, callback);
}

bool TracingControllerImpl::EnableRecording(
    const CategoryFilter& category_filter,
    const TraceOptions& trace_options,
    const EnableRecordingDoneCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (!can_enable_recording())
    return false;
  is_recording_ = true;

#if defined(OS_ANDROID)
  if (pending_get_categories_done_callback_.is_null())
    TraceLog::GetInstance()->AddClockSyncMetadataEvent();
#endif

  trace_options_ = trace_options;

  if (trace_options.enable_systrace) {
#if defined(OS_CHROMEOS)
    DCHECK(!is_system_tracing_);
    chromeos::DBusThreadManager::Get()->GetDebugDaemonClient()->
      StartSystemTracing();
    is_system_tracing_ = true;
#elif defined(OS_WIN)
    DCHECK(!is_system_tracing_);
    is_system_tracing_ =
        EtwSystemEventConsumer::GetInstance()->StartSystemTracing();
#endif
  }


  base::Closure on_enable_recording_done_callback =
      base::Bind(&TracingControllerImpl::OnEnableRecordingDone,
                 base::Unretained(this),
                 category_filter, trace_options, callback);
  BrowserThread::PostTask(BrowserThread::FILE, FROM_HERE,
      base::Bind(&TracingControllerImpl::SetEnabledOnFileThread,
                 base::Unretained(this),
                 category_filter,
                 base::trace_event::TraceLog::RECORDING_MODE,
                 trace_options,
                 on_enable_recording_done_callback));
  return true;
}

void TracingControllerImpl::OnEnableRecordingDone(
    const CategoryFilter& category_filter,
    const TraceOptions& trace_options,
    const EnableRecordingDoneCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // Notify all child processes.
  for (TraceMessageFilterSet::iterator it = trace_message_filters_.begin();
      it != trace_message_filters_.end(); ++it) {
    it->get()->SendBeginTracing(category_filter, trace_options);
  }

  if (!callback.is_null())
    callback.Run();
}

bool TracingControllerImpl::DisableRecording(
    const scoped_refptr<TraceDataSink>& trace_data_sink) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (!can_disable_recording())
    return false;

  trace_data_sink_ = trace_data_sink;
  trace_options_ = TraceOptions();
  // Disable local trace early to avoid traces during end-tracing process from
  // interfering with the process.
  base::Closure on_disable_recording_done_callback = base::Bind(
      &TracingControllerImpl::OnDisableRecordingDone, base::Unretained(this));
  BrowserThread::PostTask(BrowserThread::FILE, FROM_HERE,
      base::Bind(&TracingControllerImpl::SetDisabledOnFileThread,
                 base::Unretained(this),
                 on_disable_recording_done_callback));
  return true;
}

void TracingControllerImpl::OnDisableRecordingDone() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

#if defined(OS_ANDROID)
  if (pending_get_categories_done_callback_.is_null())
    TraceLog::GetInstance()->AddClockSyncMetadataEvent();
#endif

  // Count myself (local trace) in pending_disable_recording_ack_count_,
  // acked below.
  pending_disable_recording_ack_count_ = trace_message_filters_.size() + 1;
  pending_disable_recording_filters_ = trace_message_filters_;

#if defined(OS_CHROMEOS) || defined(OS_WIN)
  if (is_system_tracing_) {
    // Disable system tracing.
    is_system_tracing_ = false;
    ++pending_disable_recording_ack_count_;

#if defined(OS_CHROMEOS)
    scoped_refptr<base::TaskRunner> task_runner =
        BrowserThread::GetBlockingPool();
    chromeos::DBusThreadManager::Get()
        ->GetDebugDaemonClient()
        ->RequestStopSystemTracing(
            task_runner,
            base::Bind(&TracingControllerImpl::OnEndSystemTracingAcked,
                       base::Unretained(this)));
#elif defined(OS_WIN)
    EtwSystemEventConsumer::GetInstance()->StopSystemTracing(
        base::Bind(&TracingControllerImpl::OnEndSystemTracingAcked,
                   base::Unretained(this)));
#endif
  }
#endif  // defined(OS_CHROMEOS) || defined(OS_WIN)

  // Handle special case of zero child processes by immediately flushing the
  // trace log. Once the flush has completed the caller will be notified that
  // tracing has ended.
  if (pending_disable_recording_ack_count_ == 1) {
    // Flush asynchronously now, because we don't have any children to wait for.
    TraceLog::GetInstance()->Flush(
        base::Bind(&TracingControllerImpl::OnLocalTraceDataCollected,
                   base::Unretained(this)),
        true);
  }

  // Notify all child processes.
  for (TraceMessageFilterSet::iterator it = trace_message_filters_.begin();
      it != trace_message_filters_.end(); ++it) {
    it->get()->SendEndTracing();
  }
}

bool TracingControllerImpl::EnableMonitoring(
    const CategoryFilter& category_filter,
    const TraceOptions& trace_options,
    const EnableMonitoringDoneCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (!can_enable_monitoring())
    return false;
  OnMonitoringStateChanged(true);

#if defined(OS_ANDROID)
  TraceLog::GetInstance()->AddClockSyncMetadataEvent();
#endif

  trace_options_ = trace_options;

  base::Closure on_enable_monitoring_done_callback =
      base::Bind(&TracingControllerImpl::OnEnableMonitoringDone,
                 base::Unretained(this),
                 category_filter, trace_options, callback);
  BrowserThread::PostTask(BrowserThread::FILE, FROM_HERE,
      base::Bind(&TracingControllerImpl::SetEnabledOnFileThread,
                 base::Unretained(this),
                 category_filter,
                 base::trace_event::TraceLog::MONITORING_MODE,
                 trace_options,
                 on_enable_monitoring_done_callback));
  return true;
}

void TracingControllerImpl::OnEnableMonitoringDone(
    const CategoryFilter& category_filter,
    const TraceOptions& trace_options,
    const EnableMonitoringDoneCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // Notify all child processes.
  for (TraceMessageFilterSet::iterator it = trace_message_filters_.begin();
      it != trace_message_filters_.end(); ++it) {
    it->get()->SendEnableMonitoring(category_filter, trace_options);
  }

  if (!callback.is_null())
    callback.Run();
}

bool TracingControllerImpl::DisableMonitoring(
    const DisableMonitoringDoneCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (!can_disable_monitoring())
    return false;

  trace_options_ = TraceOptions();
  base::Closure on_disable_monitoring_done_callback =
      base::Bind(&TracingControllerImpl::OnDisableMonitoringDone,
                 base::Unretained(this), callback);
  BrowserThread::PostTask(BrowserThread::FILE, FROM_HERE,
      base::Bind(&TracingControllerImpl::SetDisabledOnFileThread,
                 base::Unretained(this),
                 on_disable_monitoring_done_callback));
  return true;
}

scoped_refptr<TracingController::TraceDataSink>
TracingController::CreateStringSink(
    const base::Callback<void(base::RefCountedString*)>& callback) {
  return new StringTraceDataSink(callback);
}

scoped_refptr<TracingController::TraceDataSink>
TracingController::CreateFileSink(const base::FilePath& file_path,
                                  const base::Closure& callback) {
  return new FileTraceDataSink(file_path, callback);
}

void TracingControllerImpl::OnDisableMonitoringDone(
    const DisableMonitoringDoneCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  OnMonitoringStateChanged(false);

  // Notify all child processes.
  for (TraceMessageFilterSet::iterator it = trace_message_filters_.begin();
      it != trace_message_filters_.end(); ++it) {
    it->get()->SendDisableMonitoring();
  }
  if (!callback.is_null())
    callback.Run();
}

void TracingControllerImpl::GetMonitoringStatus(
    bool* out_enabled,
    CategoryFilter* out_category_filter,
    TraceOptions* out_trace_options) {
  *out_enabled = is_monitoring_;
  *out_category_filter = TraceLog::GetInstance()->GetCurrentCategoryFilter();
  *out_trace_options = trace_options_;
}

bool TracingControllerImpl::CaptureMonitoringSnapshot(
    const scoped_refptr<TraceDataSink>& monitoring_data_sink) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (!can_disable_monitoring())
    return false;

  if (!monitoring_data_sink.get())
    return false;

  monitoring_data_sink_ = monitoring_data_sink;

  // Count myself in pending_capture_monitoring_snapshot_ack_count_,
  // acked below.
  pending_capture_monitoring_snapshot_ack_count_ =
      trace_message_filters_.size() + 1;
  pending_capture_monitoring_filters_ = trace_message_filters_;

  // Handle special case of zero child processes by immediately flushing the
  // trace log. Once the flush has completed the caller will be notified that
  // the capture snapshot has ended.
  if (pending_capture_monitoring_snapshot_ack_count_ == 1) {
    // Flush asynchronously now, because we don't have any children to wait for.
    TraceLog::GetInstance()->FlushButLeaveBufferIntact(
        base::Bind(&TracingControllerImpl::OnLocalMonitoringTraceDataCollected,
                   base::Unretained(this)));
  }

  // Notify all child processes.
  for (TraceMessageFilterSet::iterator it = trace_message_filters_.begin();
      it != trace_message_filters_.end(); ++it) {
    it->get()->SendCaptureMonitoringSnapshot();
  }

#if defined(OS_ANDROID)
  TraceLog::GetInstance()->AddClockSyncMetadataEvent();
#endif

  return true;
}

bool TracingControllerImpl::GetTraceBufferUsage(
    const GetTraceBufferUsageCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (!can_get_trace_buffer_usage() || callback.is_null())
    return false;

  pending_trace_buffer_usage_callback_ = callback;

  // Count myself in pending_trace_log_status_ack_count_, acked below.
  pending_trace_log_status_ack_count_ = trace_message_filters_.size() + 1;
  pending_trace_log_status_filters_ = trace_message_filters_;
  maximum_trace_buffer_usage_ = 0;
  approximate_event_count_ = 0;

  base::trace_event::TraceLogStatus status =
      TraceLog::GetInstance()->GetStatus();
  // Call OnTraceLogStatusReply unconditionally for the browser process.
  // This will result in immediate execution of the callback if there are no
  // child processes.
  BrowserThread::PostTask(
      BrowserThread::UI, FROM_HERE,
      base::Bind(&TracingControllerImpl::OnTraceLogStatusReply,
                 base::Unretained(this), scoped_refptr<TraceMessageFilter>(),
                 status));

  // Notify all child processes.
  for (TraceMessageFilterSet::iterator it = trace_message_filters_.begin();
      it != trace_message_filters_.end(); ++it) {
    it->get()->SendGetTraceLogStatus();
  }
  return true;
}

bool TracingControllerImpl::SetWatchEvent(
    const std::string& category_name,
    const std::string& event_name,
    const WatchEventCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (callback.is_null())
    return false;

  watch_category_name_ = category_name;
  watch_event_name_ = event_name;
  watch_event_callback_ = callback;

  TraceLog::GetInstance()->SetWatchEvent(
      category_name, event_name,
      base::Bind(&TracingControllerImpl::OnWatchEventMatched,
                 base::Unretained(this)));

  for (TraceMessageFilterSet::iterator it = trace_message_filters_.begin();
      it != trace_message_filters_.end(); ++it) {
    it->get()->SendSetWatchEvent(category_name, event_name);
  }
  return true;
}

bool TracingControllerImpl::CancelWatchEvent() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (!can_cancel_watch_event())
    return false;

  for (TraceMessageFilterSet::iterator it = trace_message_filters_.begin();
      it != trace_message_filters_.end(); ++it) {
    it->get()->SendCancelWatchEvent();
  }

  watch_event_callback_.Reset();
  return true;
}

void TracingControllerImpl::AddTraceMessageFilter(
    TraceMessageFilter* trace_message_filter) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
        base::Bind(&TracingControllerImpl::AddTraceMessageFilter,
                   base::Unretained(this),
                   make_scoped_refptr(trace_message_filter)));
    return;
  }

  trace_message_filters_.insert(trace_message_filter);
  if (can_cancel_watch_event()) {
    trace_message_filter->SendSetWatchEvent(watch_category_name_,
                                            watch_event_name_);
  }
  if (can_disable_recording()) {
    trace_message_filter->SendBeginTracing(
        TraceLog::GetInstance()->GetCurrentCategoryFilter(),
        TraceLog::GetInstance()->GetCurrentTraceOptions());
  }
  if (can_disable_monitoring()) {
    trace_message_filter->SendEnableMonitoring(
        TraceLog::GetInstance()->GetCurrentCategoryFilter(),
        TraceLog::GetInstance()->GetCurrentTraceOptions());
  }
}

void TracingControllerImpl::RemoveTraceMessageFilter(
    TraceMessageFilter* trace_message_filter) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
        base::Bind(&TracingControllerImpl::RemoveTraceMessageFilter,
                   base::Unretained(this),
                   make_scoped_refptr(trace_message_filter)));
    return;
  }

  // If a filter is removed while a response from that filter is pending then
  // simulate the response. Otherwise the response count will be wrong and the
  // completion callback will never be executed.
  if (pending_disable_recording_ack_count_ > 0) {
    TraceMessageFilterSet::const_iterator it =
        pending_disable_recording_filters_.find(trace_message_filter);
    if (it != pending_disable_recording_filters_.end()) {
      BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
          base::Bind(&TracingControllerImpl::OnDisableRecordingAcked,
                     base::Unretained(this),
                     make_scoped_refptr(trace_message_filter),
                     std::vector<std::string>()));
    }
  }
  if (pending_capture_monitoring_snapshot_ack_count_ > 0) {
    TraceMessageFilterSet::const_iterator it =
        pending_capture_monitoring_filters_.find(trace_message_filter);
    if (it != pending_capture_monitoring_filters_.end()) {
      BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
          base::Bind(&TracingControllerImpl::OnCaptureMonitoringSnapshotAcked,
                     base::Unretained(this),
                     make_scoped_refptr(trace_message_filter)));
    }
  }
  if (pending_trace_log_status_ack_count_ > 0) {
    TraceMessageFilterSet::const_iterator it =
        pending_trace_log_status_filters_.find(trace_message_filter);
    if (it != pending_trace_log_status_filters_.end()) {
      BrowserThread::PostTask(
          BrowserThread::UI, FROM_HERE,
          base::Bind(&TracingControllerImpl::OnTraceLogStatusReply,
                     base::Unretained(this),
                     make_scoped_refptr(trace_message_filter),
                     base::trace_event::TraceLogStatus()));
    }
  }

  trace_message_filters_.erase(trace_message_filter);
}

void TracingControllerImpl::OnDisableRecordingAcked(
    TraceMessageFilter* trace_message_filter,
    const std::vector<std::string>& known_category_groups) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
        base::Bind(&TracingControllerImpl::OnDisableRecordingAcked,
                   base::Unretained(this),
                   make_scoped_refptr(trace_message_filter),
                   known_category_groups));
    return;
  }

  // Merge known_category_groups with known_category_groups_
  known_category_groups_.insert(known_category_groups.begin(),
                                known_category_groups.end());

  if (pending_disable_recording_ack_count_ == 0)
    return;

  if (trace_message_filter &&
      !pending_disable_recording_filters_.erase(trace_message_filter)) {
    // The response from the specified message filter has already been received.
    return;
  }

  if (--pending_disable_recording_ack_count_ == 1) {
    // All acks from subprocesses have been received. Now flush the local trace.
    // During or after this call, our OnLocalTraceDataCollected will be
    // called with the last of the local trace data.
    TraceLog::GetInstance()->Flush(
        base::Bind(&TracingControllerImpl::OnLocalTraceDataCollected,
                   base::Unretained(this)),
        true);
    return;
  }

  if (pending_disable_recording_ack_count_ != 0)
    return;

  // All acks (including from the subprocesses and the local trace) have been
  // received.
  is_recording_ = false;

  // Trigger callback if one is set.
  if (!pending_get_categories_done_callback_.is_null()) {
    pending_get_categories_done_callback_.Run(known_category_groups_);
    pending_get_categories_done_callback_.Reset();
  } else if (trace_data_sink_.get()) {
    trace_data_sink_->Close();
    trace_data_sink_ = NULL;
  }
}

#if defined(OS_CHROMEOS) || defined(OS_WIN)
void TracingControllerImpl::OnEndSystemTracingAcked(
    const scoped_refptr<base::RefCountedString>& events_str_ptr) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (trace_data_sink_.get()) {
#if defined(OS_WIN)
    // The Windows kernel events are kept into a JSon format stored as string
    // and must not be escaped.
    std::string json_string = events_str_ptr->data();
#else
    std::string json_string =
        base::GetQuotedJSONString(events_str_ptr->data());
#endif
    trace_data_sink_->SetSystemTrace(json_string);
  }
  DCHECK(!is_system_tracing_);
  std::vector<std::string> category_groups;
  OnDisableRecordingAcked(NULL, category_groups);
}
#endif

void TracingControllerImpl::OnCaptureMonitoringSnapshotAcked(
    TraceMessageFilter* trace_message_filter) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
        base::Bind(&TracingControllerImpl::OnCaptureMonitoringSnapshotAcked,
                   base::Unretained(this),
                   make_scoped_refptr(trace_message_filter)));
    return;
  }

  if (pending_capture_monitoring_snapshot_ack_count_ == 0)
    return;

  if (trace_message_filter &&
      !pending_capture_monitoring_filters_.erase(trace_message_filter)) {
    // The response from the specified message filter has already been received.
    return;
  }

  if (--pending_capture_monitoring_snapshot_ack_count_ == 1) {
    // All acks from subprocesses have been received. Now flush the local trace.
    // During or after this call, our OnLocalMonitoringTraceDataCollected
    // will be called with the last of the local trace data.
    TraceLog::GetInstance()->FlushButLeaveBufferIntact(
        base::Bind(&TracingControllerImpl::OnLocalMonitoringTraceDataCollected,
                   base::Unretained(this)));
    return;
  }

  if (pending_capture_monitoring_snapshot_ack_count_ != 0)
    return;

  if (monitoring_data_sink_.get()) {
    monitoring_data_sink_->Close();
    monitoring_data_sink_ = NULL;
  }
}

void TracingControllerImpl::OnTraceDataCollected(
    const scoped_refptr<base::RefCountedString>& events_str_ptr) {
  // OnTraceDataCollected may be called from any browser thread, either by the
  // local event trace system or from child processes via TraceMessageFilter.
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
        base::Bind(&TracingControllerImpl::OnTraceDataCollected,
                   base::Unretained(this), events_str_ptr));
    return;
  }

  if (trace_data_sink_.get())
    trace_data_sink_->AddTraceChunk(events_str_ptr->data());
}

void TracingControllerImpl::OnMonitoringTraceDataCollected(
    const scoped_refptr<base::RefCountedString>& events_str_ptr) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
        base::Bind(&TracingControllerImpl::OnMonitoringTraceDataCollected,
                   base::Unretained(this), events_str_ptr));
    return;
  }

  if (monitoring_data_sink_.get())
    monitoring_data_sink_->AddTraceChunk(events_str_ptr->data());
}

void TracingControllerImpl::OnLocalTraceDataCollected(
    const scoped_refptr<base::RefCountedString>& events_str_ptr,
    bool has_more_events) {
  if (events_str_ptr->data().size())
    OnTraceDataCollected(events_str_ptr);

  if (has_more_events)
    return;

  // Simulate an DisableRecordingAcked for the local trace.
  std::vector<std::string> category_groups;
  TraceLog::GetInstance()->GetKnownCategoryGroups(&category_groups);
  OnDisableRecordingAcked(NULL, category_groups);
}

void TracingControllerImpl::OnLocalMonitoringTraceDataCollected(
    const scoped_refptr<base::RefCountedString>& events_str_ptr,
    bool has_more_events) {
  if (events_str_ptr->data().size())
    OnMonitoringTraceDataCollected(events_str_ptr);

  if (has_more_events)
    return;

  // Simulate an CaptureMonitoringSnapshotAcked for the local trace.
  OnCaptureMonitoringSnapshotAcked(NULL);
}

void TracingControllerImpl::OnTraceLogStatusReply(
    TraceMessageFilter* trace_message_filter,
    const base::trace_event::TraceLogStatus& status) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(
        BrowserThread::UI, FROM_HERE,
        base::Bind(&TracingControllerImpl::OnTraceLogStatusReply,
                   base::Unretained(this),
                   make_scoped_refptr(trace_message_filter), status));
    return;
  }

  if (pending_trace_log_status_ack_count_ == 0)
    return;

  if (trace_message_filter &&
      !pending_trace_log_status_filters_.erase(trace_message_filter)) {
    // The response from the specified message filter has already been received.
    return;
  }

  float percent_full = static_cast<float>(
      static_cast<double>(status.event_count) / status.event_capacity);
  maximum_trace_buffer_usage_ =
      std::max(maximum_trace_buffer_usage_, percent_full);
  approximate_event_count_ += status.event_count;

  if (--pending_trace_log_status_ack_count_ == 0) {
    // Trigger callback if one is set.
    pending_trace_buffer_usage_callback_.Run(maximum_trace_buffer_usage_,
                                             approximate_event_count_);
    pending_trace_buffer_usage_callback_.Reset();
  }
}

void TracingControllerImpl::OnWatchEventMatched() {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
        base::Bind(&TracingControllerImpl::OnWatchEventMatched,
                   base::Unretained(this)));
    return;
  }

  if (!watch_event_callback_.is_null())
    watch_event_callback_.Run();
}

void TracingControllerImpl::RegisterTracingUI(TracingUI* tracing_ui) {
  DCHECK(tracing_uis_.find(tracing_ui) == tracing_uis_.end());
  tracing_uis_.insert(tracing_ui);
}

void TracingControllerImpl::UnregisterTracingUI(TracingUI* tracing_ui) {
  std::set<TracingUI*>::iterator it = tracing_uis_.find(tracing_ui);
  DCHECK(it != tracing_uis_.end());
  tracing_uis_.erase(it);
}

void TracingControllerImpl::RequestGlobalMemoryDump(
    const base::trace_event::MemoryDumpRequestArgs& args,
    const base::trace_event::MemoryDumpCallback& callback) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(
        BrowserThread::UI, FROM_HERE,
        base::Bind(&TracingControllerImpl::RequestGlobalMemoryDump,
                   base::Unretained(this), args, callback));
    return;
  }
  // TODO(primiano): send a local dump request to each of the child processes
  // and do the bookkeeping to keep track of the outstanding requests.
  // Also, at this point, this should check for collisions and bail out if a
  // global dump is requested while another is already in progress.
  NOTIMPLEMENTED();
}

void TracingControllerImpl::OnProcessMemoryDumpResponse(
    TraceMessageFilter* trace_message_filter,
    uint64 dump_guid,
    bool success) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(
        BrowserThread::UI, FROM_HERE,
        base::Bind(&TracingControllerImpl::OnProcessMemoryDumpResponse,
                   base::Unretained(this),
                   make_scoped_refptr(trace_message_filter), dump_guid,
                   success));
    return;
  }
  // TODO(primiano): update the bookkeeping structs and, if this was the
  // response from the last pending child, fire the completion callback, which
  // in turn will cause a GlobalMemoryDumpResponse message to be sent back to
  // the child, if this global dump was NOT initiated by the browser.
  NOTIMPLEMENTED();
}

void TracingControllerImpl::OnMonitoringStateChanged(bool is_monitoring) {
  if (is_monitoring_ == is_monitoring)
    return;

  is_monitoring_ = is_monitoring;
#if !defined(OS_ANDROID)
  for (std::set<TracingUI*>::iterator it = tracing_uis_.begin();
       it != tracing_uis_.end(); it++) {
    (*it)->OnMonitoringStateChanged(is_monitoring);
  }
#endif
}

}  // namespace content
