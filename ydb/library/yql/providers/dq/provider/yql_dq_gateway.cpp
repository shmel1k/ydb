#include "yql_dq_gateway.h"

#include <yql/essentials/providers/common/provider/yql_provider_names.h>
#include <ydb/library/yql/providers/dq/api/grpc/api.grpc.pb.h>
#include <ydb/library/yql/providers/dq/common/yql_dq_common.h>
#include <ydb/library/yql/providers/dq/actors/proto_builder.h>
#include <yql/essentials/utils/backtrace/backtrace.h>
#include <yql/essentials/utils/failure_injector/failure_injector.h>
#include <yql/essentials/public/issue/yql_issue_message.h>
#include <ydb/library/yql/providers/dq/config/config.pb.h>
#include <yql/essentials/utils/log/log.h>

#include <ydb/public/lib/yson_value/ydb_yson_value.h>

#include <ydb/public/sdk/cpp/src/library/grpc/client/grpc_client_low.h>

#include <library/cpp/yson/node/node_io.h>
#include <library/cpp/threading/task_scheduler/task_scheduler.h>

#include <util/system/mutex.h>
#include <util/generic/hash.h>
#include <util/string/builder.h>

#include <utility>

namespace NYql {

using namespace NThreading;

class TPlanPrinter {
public:
    TStringBuilder b;

    void DescribeChannel(const auto& ch, bool spilling) {
        if (spilling) {
            b << "Ch" << ch.GetId() << " [shape=diamond, label=\"Ch" << ch.GetId() << "\", color=\"red\"];";
        } else {
            b << "Ch" << ch.GetId() << " [shape=diamond, label=\"Ch" << ch.GetId() << "\"];";
        }
    }

    void PrintInputChannel(const auto& ch, const auto& type) {
        b << "Ch" << ch.GetId() << " -> T" << ch.GetDstTaskId() << " [label=" << "\"" << type << "\"];\n";
    }

    void PrintOutputChannel(const auto& ch, const auto& type) {
        b << "T" << ch.GetSrcTaskId() << " -> Ch" << ch.GetId() << " [label=" << "\"" << type << "\"];\n";
    }

    void PrintSource(auto taskId, auto sourceIndex) {
        b << "S" << taskId << "_" << sourceIndex << " -> T" << taskId << " [label=" << "\"S" << sourceIndex << "\"];\n";
    }

    void DescribeSource(auto taskId, auto sourceIndex) {
        b << "S" << taskId << "_" << sourceIndex << " ";
        b << "[shape=square, label=\"" << taskId << "/" << sourceIndex << "\"];\n";
    }

    void PrintTask(const auto& task) {
        int index = 0;
        for (const auto& input : task.GetInputs()) {
            TString inputName = "Unknown";
            bool isSource = false;
            if (input.HasUnionAll()) { inputName = "UnionAll"; }
            else if (input.HasMerge()) { inputName = "Merge"; }
            else if (input.HasSource()) { inputName = "Source"; isSource = true; }
            if (isSource) {
                PrintSource(task.GetId(), index);
            } else {
                for (const auto& ch : input.GetChannels()) {
                    PrintInputChannel(ch, inputName);
                }
            }
            index ++;
        }
        for (const auto& output : task.GetOutputs()) {
            TString outputName = "Unknown";
            if (output.HasMap()) { outputName = "Map"; }
            else if (output.HasRangePartition()) { outputName = "Range"; }
            else if (output.HasHashPartition()) { outputName = "Hash"; }
            else if (output.HasBroadcast()) { outputName = "Broadcast"; }
            // TODO: effects, sink
            for (const auto& ch : output.GetChannels()) {
                PrintOutputChannel(ch, outputName);
            }
        }
    }

    void DescribeTask(const auto& task) {
        b << "T" << task.GetId() << " [shape=circle, label=\"" << task.GetId() << "/" << task.GetStageId() << "\"];\n";
        int index = 0;
        for (const auto& input : task.GetInputs()) {
            if (input.HasSource()) {
                DescribeSource(task.GetId(), index);
            }
            index ++;
        }
        for (const auto& output : task.GetOutputs()) {
            for (const auto& ch : output.GetChannels()) {
                DescribeChannel(ch, task.GetEnableSpilling());
            }
        }
    }
 
    TString Print(const NDqs::TPlan& plan) {
        b.clear();
        b << "digraph G {\n";
        for (const auto& task : plan.Tasks) {
            DescribeTask(task);
        }
        b << "\n";
        for (const auto& task : plan.Tasks) {
            PrintTask(task);
        }
        b << "}\n";
        return b;
    }
};

class TDqTaskScheduler : public TTaskScheduler {
private:
    struct TDelay: public TTaskScheduler::ITask {
        TDelay(TPromise<void> p)
            : Promise(std::move(p))
        { }

        TInstant Process() override {
            Promise.SetValue();
            return TInstant::Max();
        }

        TPromise<void> Promise;
    };

public:
    TDqTaskScheduler()
        : TTaskScheduler(1) // threads
    {}

    TFuture<void> Delay(TDuration duration) {
        TPromise<void> promise = NewPromise();

        auto future = promise.GetFuture();

        if (!Add(MakeIntrusive<TDelay>(promise), TInstant::Now() + duration)) {
            promise.SetException("cannot delay");
        }

        return future;
    }
};

class TDqGatewaySession: public std::enable_shared_from_this<TDqGatewaySession> {
public:
    using TResult = IDqGateway::TResult;
    using TDqProgressWriter = IDqGateway::TDqProgressWriter;

    TDqGatewaySession(const TString& sessionId, TDqTaskScheduler& taskScheduler, NYdbGrpc::TServiceConnection<Yql::DqsProto::DqService>& service, TFuture<void>&& openSessionFuture)
        : SessionId(sessionId)
        , TaskScheduler(taskScheduler)
        , Service(service)
        , OpenSessionFuture(std::move(openSessionFuture))
    {
    }

    const TString& GetSessionId() const {
        return SessionId;
    }

    template<typename RespType>
    void OnResponse(TPromise<TResult> promise, NYdbGrpc::TGrpcStatus&& status, RespType&& resp, const NCommon::TResultFormatSettings& resultFormatSettings, const THashMap<TString, TString>& modulesMapping, bool alwaysFallback = false) {
        YQL_LOG_CTX_ROOT_SESSION_SCOPE(SessionId);
        YQL_CLOG(TRACE, ProviderDq) << "TDqGateway::callback";

        TResult result;

        bool error = false;
        bool fallback = false;
        result.Timeout = resp.GetTimeout();

        if (status.Ok()) {
            YQL_CLOG(TRACE, ProviderDq) << "TDqGateway::Ok";

            result.Truncated = resp.GetTruncated();

            TOperationStatistics statistics;

            for (const auto& t : resp.GetMetric()) {
                YQL_CLOG(TRACE, ProviderDq) << "Counter: " << t.GetName() << " : " << t.GetSum() << " : " << t.GetCount();
                TOperationStatistics::TEntry entry(
                    t.GetName(),
                    t.GetSum(),
                    t.GetMax(),
                    t.GetMin(),
                    t.GetAvg(),
                    t.GetCount());
                statistics.Entries.push_back(entry);
            }

            result.Statistics = statistics;

            NYql::TIssues issues;
            auto operation = resp.operation();

            for (auto& message_ : *operation.Mutableissues()) {
                TDeque<std::remove_reference_t<decltype(message_)>*> queue;
                queue.push_front(&message_);
                while (!queue.empty()) {
                    auto& message = *queue.front();
                    queue.pop_front();
                    message.Setmessage(NBacktrace::Symbolize(message.Getmessage(), modulesMapping));
                    for (auto& subMsg : *message.Mutableissues()) {
                        queue.push_back(&subMsg);
                    }
                }
            }

            NYql::IssuesFromMessage(operation.issues(), issues);
            error = false;
            for (const auto& issue : issues) {
                if (issue.GetSeverity() <= TSeverityIds::S_ERROR) {
                    error = true;
                }
                if (issue.GetCode() == TIssuesIds::DQ_GATEWAY_NEED_FALLBACK_ERROR) {
                    fallback = true;
                }
            }

            // TODO: Save statistics in case of result failure
            if (!error) {
                Yql::DqsProto::ExecuteQueryResult queryResult;
                resp.operation().result().UnpackTo(&queryResult);
                TVector<NDq::TDqSerializedBatch> rows;
                for (const auto& s : queryResult.Getsample()) {
                    NDq::TDqSerializedBatch batch;
                    batch.Proto = s;
                    rows.emplace_back(std::move(batch));
                }

                result.AddIssues(issues);
                try {
                    NYql::NDqs::TProtoBuilder protoBuilder(resultFormatSettings.ResultType, resultFormatSettings.Columns);

                    bool ysonTruncated = false;
                    result.Data = protoBuilder.BuildYson(std::move(rows),
                        result.Truncated ? resultFormatSettings.SizeLimit.GetOrElse(Max<ui64>()) : Max<ui64>(),
                        result.Truncated ? resultFormatSettings.RowsLimit.GetOrElse(Max<ui64>()) : Max<ui64>(),
                        &ysonTruncated);

                    result.Truncated = result.Truncated || ysonTruncated;
                    result.SetSuccess();
                } catch (...) {
                    YQL_CLOG(ERROR, ProviderDq) << "Failed to build yson result: " << CurrentExceptionMessage();
                    error = true;
                    auto issue = TIssue("Failed to build query result (probably due to malformed UDF)");
                    result.AddIssue(issue.SetCode(TIssuesIds::DQ_GATEWAY_ERROR, TSeverityIds::S_ERROR));
                }
            } else {
                YQL_CLOG(ERROR, ProviderDq) << "Issue " << issues.ToString();
                result.AddIssues(issues);
                if (fallback) {
                    result.Fallback = true;
                    result.SetSuccess();
                }
            }
        } else {
            YQL_CLOG(ERROR, ProviderDq) << "Issue " << status.Msg;
            auto issue = TIssue(TStringBuilder{} << "Error " << status.GRpcStatusCode << " message: " << status.Msg);
            result.Retriable = status.GRpcStatusCode == grpc::CANCELLED;
            if ((status.GRpcStatusCode == grpc::UNAVAILABLE /* terminating state */
                || status.GRpcStatusCode == grpc::CANCELLED /* server crashed or stopped before task process */)
                || status.GRpcStatusCode == grpc::RESOURCE_EXHAUSTED /* send message limit */
                || status.GRpcStatusCode == grpc::INVALID_ARGUMENT /* Bad session */
                )
            {
                YQL_CLOG(ERROR, ProviderDq) << "Fallback " << status.GRpcStatusCode;
                result.Fallback = true;
                result.SetSuccess();
                result.AddIssue(issue.SetCode(TIssuesIds::DQ_GATEWAY_NEED_FALLBACK_ERROR, TSeverityIds::S_ERROR));
            } else {
                error = true;
                result.AddIssue(issue.SetCode(TIssuesIds::DQ_GATEWAY_ERROR, TSeverityIds::S_ERROR));
            }
        }

        if (error && alwaysFallback) {
            YQL_CLOG(ERROR, ProviderDq) << "Force Fallback";
            result.Fallback = true;
            result.ForceFallback = true;
            result.SetSuccess();
        }

        promise.SetValue(result);
    }

    template <typename TResponse, typename TRequest, typename TStub>
    TFuture<TResult> WithRetry(
        const TRequest& queryPB,
        TStub stub,
        int retry,
        const TDqSettings::TPtr& settings,
        const NCommon::TResultFormatSettings& resultFormatSettings,
        const THashMap<TString, TString>& modulesMapping,
        const TDqProgressWriter& progressWriter
    ) {
        auto backoff = TDuration::MilliSeconds(settings->RetryBackoffMs.Get().GetOrElse(1000));
        auto promise = NewPromise<TResult>();
        const auto fallbackPolicy = settings->FallbackPolicy.Get().GetOrElse(EFallbackPolicy::Default);
        const auto alwaysFallback = EFallbackPolicy::Always == fallbackPolicy;
        auto self = weak_from_this();
        auto callback = [self, promise, sessionId = SessionId, alwaysFallback, resultFormatSettings, modulesMapping](
            NYdbGrpc::TGrpcStatus&& status, TResponse&& resp) mutable {
            auto this_ = self.lock();
            if (!this_) {
                YQL_CLOG(DEBUG, ProviderDq) << "Session was closed: " << sessionId;
                promise.SetException("Session was closed");
                return;
            }

            this_->OnResponse(std::move(promise), std::move(status), std::move(resp), resultFormatSettings,
                modulesMapping, alwaysFallback);
        };

        Service.DoRequest<TRequest, TResponse>(queryPB, callback, stub);

        ScheduleQueryStatusRequest(progressWriter, queryPB.GetQuerySeqNo());

        return promise.GetFuture().Apply([=](const TFuture<TResult>& result) {
            if (result.HasException()) {
                return result;
            }
            auto value = result.GetValue();
            auto this_ = self.lock();

            if (value.Success() || retry == 0 || !value.Retriable || !this_) {
                return result;
            }

            return this_->TaskScheduler.Delay(backoff)
                .Apply([=, sessionId = this_->GetSessionId()](const TFuture<void>& result) {
                    auto this_ = self.lock();
                    try {
                        result.TryRethrow();
                        if (!this_) {
                            YQL_CLOG(DEBUG, ProviderDq) << "Session was closed: " << sessionId;
                            throw std::runtime_error("Session was closed");
                        }
                    } catch (...) {
                        return MakeErrorFuture<TResult>(std::current_exception());
                    }
                    return this_->WithRetry<TResponse>(queryPB, stub, retry - 1, settings, resultFormatSettings,
                        modulesMapping, progressWriter);
                });
        });
    }

    TFuture<TResult>
    ExecutePlan(NDqs::TPlan&& plan, const TVector<TString>& columns,
                const THashMap<TString, TString>& secureParams, const THashMap<TString, TString>& graphParams,
                const TDqSettings::TPtr& settings,
                const TDqProgressWriter& progressWriter, const THashMap<TString, TString>& modulesMapping,
                bool discard, ui64 executionTimeout)
    {
        YQL_LOG_CTX_ROOT_SESSION_SCOPE(SessionId);

        Yql::DqsProto::ExecuteGraphRequest queryPB;
        for (const auto& task : plan.Tasks) {
            auto* t = queryPB.AddTask();
            *t = task;

            Yql::DqsProto::TTaskMeta taskMeta;
            task.GetMeta().UnpackTo(&taskMeta);

            for (auto& file : taskMeta.GetFiles()) {
                YQL_ENSURE(!file.GetObjectId().empty());
            }
        }
        queryPB.SetExecutionTimeout(executionTimeout);
        queryPB.SetSession(SessionId);
        queryPB.SetResultType(plan.ResultType);
        queryPB.SetSourceId(plan.SourceID.NodeId()-1);
        for (const auto& column : columns) {
            *queryPB.AddColumns() = column;
        }
        settings->Save(queryPB);

        NCommon::TResultFormatSettings resultFormatSettings;
        resultFormatSettings.Columns = columns;
        resultFormatSettings.ResultType = plan.ResultType;
        resultFormatSettings.SizeLimit = settings->_AllResultsBytesLimit.Get();
        resultFormatSettings.RowsLimit = settings->_RowsLimitPerWrite.Get();

        YQL_CLOG(TRACE, ProviderDq) << TPlanPrinter().Print(plan);

        {
            auto& secParams = *queryPB.MutableSecureParams();
            for (const auto&[k, v] : secureParams) {
                secParams[k] = v;
            }
        }

        {
            auto& gParams = *queryPB.MutableGraphParams();
            for (const auto&[k, v] : graphParams) {
                gParams[k] = v;
            }
        }

        queryPB.SetDiscard(discard);
        queryPB.SetQuerySeqNo(QuerySeqNo++);

        int retry = settings->MaxRetries.Get().GetOrElse(5);

        YQL_CLOG(DEBUG, ProviderDq) << "Send query of size " << queryPB.ByteSizeLong();

        auto self = weak_from_this();
        return OpenSessionFuture.Apply([self, sessionId = SessionId, queryPB, retry, settings, resultFormatSettings, modulesMapping,
            progressWriter](const TFuture<void>& f) {
            f.TryRethrow();
            auto this_ = self.lock();
            if (!this_) {
                YQL_CLOG(DEBUG, ProviderDq) << "Session was closed: " << sessionId;
                return MakeErrorFuture<TResult>(std::make_exception_ptr(std::runtime_error("Session was closed")));
            }

            return this_->WithRetry<Yql::DqsProto::ExecuteGraphResponse>(
                queryPB,
                &Yql::DqsProto::DqService::Stub::AsyncExecuteGraph,
                retry,
                settings,
                resultFormatSettings,
                modulesMapping,
                progressWriter);
        });
    }

    TFuture<void> Close() {
        Yql::DqsProto::CloseSessionRequest request;
        request.SetSession(SessionId);

        auto promise = NewPromise<void>();
        auto callback = [promise, sessionId = SessionId](NYdbGrpc::TGrpcStatus&& status, Yql::DqsProto::CloseSessionResponse&& resp) mutable {
            Y_UNUSED(resp);
            YQL_LOG_CTX_ROOT_SESSION_SCOPE(sessionId);
            if (status.Ok()) {
                YQL_CLOG(DEBUG, ProviderDq) << "Async close session OK";
                promise.SetValue();
            } else {
                YQL_CLOG(ERROR, ProviderDq) << "Async close session error: " << status.GRpcStatusCode << ", message: " << status.Msg;
                promise.SetException(TStringBuilder() << "Async close session error: " << status.GRpcStatusCode << ", message: " << status.Msg);
            }
        };

        Service.DoRequest<Yql::DqsProto::CloseSessionRequest, Yql::DqsProto::CloseSessionResponse>(
            request, callback, &Yql::DqsProto::DqService::Stub::AsyncCloseSession);
        return promise.GetFuture();
    }

    void OnRequestQueryStatus(const TDqProgressWriter& progressWriter, IDqGateway::TProgressWriterState state, bool ok, uint64_t querySeqNo) {
        if (ok) {
            ScheduleQueryStatusRequest(progressWriter, querySeqNo);
            if (!state.empty()) {
                progressWriter(std::move(state));
            }
        }
    }

    static std::unordered_map<ui64, IDqGateway::TStageStats> ExtractStats(const Yql::DqsProto::QueryStatusResponse& resp) {
        std::unordered_map<ui64, IDqGateway::TStageStats> ret;
        for (const auto& metric : resp.GetMetric()) {
            auto longName = metric.GetName();
            TString prefix;
            TString name;
            std::map<TString, TString> labels;
            if (!NYql::NCommon::ParseCounterName(&prefix, &labels, &name, longName)) {
                continue;
            }

            auto maybeStage = labels.find("Stage");
            if (maybeStage == labels.end()) {
                continue;
            }
            auto stageId = atoi(maybeStage->second.data());
            if (!stageId) {
                continue;
            }
            auto& stage = ret[stageId];

            if (name == "OutputRows") {
                stage.OutputRows += metric.GetSum();
            }
            if (name == "InputRows") {
                stage.InputRows += metric.GetSum();
            }
            if (name == "OutputBytes") {
                stage.OutputBytes += metric.GetSum();
            }
            if (name == "InputBytes") {
                stage.InputBytes += metric.GetSum();
            }
        }
        return ret;
    }

    void RequestQueryStatus(const TDqProgressWriter& progressWriter, uint64_t querySeqNo) {
        Yql::DqsProto::QueryStatusRequest request;
        request.SetSession(SessionId);
        request.SetQuerySeqNo(querySeqNo);
        auto self = weak_from_this();
        auto callback = [self, progressWriter, querySeqNo](NYdbGrpc::TGrpcStatus&& status, Yql::DqsProto::QueryStatusResponse&& resp) {
            auto this_ = self.lock();
            if (!this_) {
                return;
            }

            this_->OnRequestQueryStatus(progressWriter, std::move(IDqGateway::TProgressWriterState{resp.GetStatus(), std::move(ExtractStats(resp))}), status.Ok(), querySeqNo);
        };

        Service.DoRequest<Yql::DqsProto::QueryStatusRequest, Yql::DqsProto::QueryStatusResponse>(
            request, callback, &Yql::DqsProto::DqService::Stub::AsyncQueryStatus, {}, nullptr);
    }

    void ScheduleQueryStatusRequest(const TDqProgressWriter& progressWriter, uint64_t querySeqNo) {
        auto self = weak_from_this();
        TaskScheduler.Delay(TDuration::MilliSeconds(1000)).Subscribe([self, progressWriter, querySeqNo](const TFuture<void>& f) {
            auto this_ = self.lock();
            if (!this_) {
                return;
            }

            if (!f.HasException()) {
                this_->RequestQueryStatus(progressWriter, querySeqNo);
            }
        });
    }

private:
    const TString SessionId;
    TDqTaskScheduler& TaskScheduler;
    NYdbGrpc::TServiceConnection<Yql::DqsProto::DqService>& Service;

    TMutex ProgressMutex;

    std::optional<TDqProgressWriter> ProgressWriter;
    TString Status;
    TFuture<void> OpenSessionFuture;
    std::atomic<ui64> QuerySeqNo = 1;
};

class TDqGatewayImpl: public std::enable_shared_from_this<TDqGatewayImpl> {
    using TResult = IDqGateway::TResult;
    using TDqProgressWriter = IDqGateway::TDqProgressWriter;

public:
    TDqGatewayImpl(const TString& host, int port, TDuration timeout = TDuration::Minutes(60), TDuration requestTimeout = TDuration::Max())
        : GrpcConf(TStringBuilder() << host << ":" << port, requestTimeout)
        , GrpcClient(1)
        , Service(GrpcClient.CreateGRpcServiceConnection<Yql::DqsProto::DqService>(GrpcConf))
        , TaskScheduler()
        , OpenSessionTimeout(timeout)
        , IsStopped(false)
    {
        TaskScheduler.Start();
    }

    ~TDqGatewayImpl() {
        Stop();
    }

    void Stop() {
        bool expected = false;
        if (!IsStopped.compare_exchange_strong(expected, true)) {
            return;
        }

        decltype(Sessions) sessions;
        with_lock (Mutex) {
            sessions = std::move(Sessions);
        }
        for (auto& pair: sessions) {
            try {
                pair.second->Close().GetValueSync();
            } catch (...) {
                YQL_LOG_CTX_ROOT_SESSION_SCOPE(pair.first);
                YQL_CLOG(ERROR, ProviderDq) << "Error closing session " << pair.first << ": " << CurrentExceptionMessage();
            }
        }
        sessions.clear(); // Destroy session objects explicitly before stopping grpc
        TaskScheduler.Stop();
        try {
            GrpcClient.Stop(/* wait = */ true);
        } catch (...) {
            YQL_CLOG(ERROR, ProviderDq) << "Error while stopping GRPC client: " << CurrentExceptionMessage();
        }
    }

    void DropSession(const TString& sessionId) {
        with_lock (Mutex) {
            Sessions.erase(sessionId);
        }
    }

    TFuture<void> OpenSession(const TString& sessionId, const TString& username) {
        YQL_LOG_CTX_ROOT_SESSION_SCOPE(sessionId);
        YQL_CLOG(INFO, ProviderDq) << "OpenSession";

        auto promise = NewPromise<void>();
        std::shared_ptr<TDqGatewaySession> session = std::make_shared<TDqGatewaySession>(sessionId, TaskScheduler, *Service, promise.GetFuture());
        with_lock (Mutex) {
            if (!Sessions.emplace(sessionId, session).second) {
                return MakeErrorFuture<void>(std::make_exception_ptr(yexception() << "Duplicate session id: " << sessionId));
            }
        }

        Yql::DqsProto::OpenSessionRequest request;
        request.SetSession(sessionId);
        request.SetUsername(username);

        NYdbGrpc::TCallMeta meta;
        meta.Timeout = OpenSessionTimeout;

        auto self = weak_from_this();
        auto callback = [self, promise, sessionId](NYdbGrpc::TGrpcStatus&& status, Yql::DqsProto::OpenSessionResponse&& resp) mutable {
            Y_UNUSED(resp);
            YQL_LOG_CTX_ROOT_SESSION_SCOPE(sessionId);
            auto this_ = self.lock();
            if (!this_) {
                YQL_CLOG(ERROR, ProviderDq) << "Session was closed: " << sessionId;
                promise.SetException("Session was closed");
                return;
            }
            if (status.Ok()) {
                YQL_CLOG(INFO, ProviderDq) << "OpenSession OK";
                this_->SchedulePingSessionRequest(sessionId);
                promise.SetValue();
            } else {
                YQL_CLOG(ERROR, ProviderDq) << "OpenSession error: " << status.Msg;
                this_->DropSession(sessionId);
                promise.SetException(TString{status.Msg});
            }
        };

        Service->DoRequest<Yql::DqsProto::OpenSessionRequest, Yql::DqsProto::OpenSessionResponse>(
            request, callback, &Yql::DqsProto::DqService::Stub::AsyncOpenSession, meta);

       return MakeFuture();
    }

    void SchedulePingSessionRequest(const TString& sessionId) {
        auto self = weak_from_this();
        auto callback = [self, sessionId] (NYdbGrpc::TGrpcStatus&& status, Yql::DqsProto::PingSessionResponse&&) mutable {
            auto this_ = self.lock();
            if (!this_) {
                return;
            }

            if (status.GRpcStatusCode == grpc::INVALID_ARGUMENT || status.GRpcStatusCode == grpc::CANCELLED) {
                YQL_CLOG(INFO, ProviderDq) << "Session closed " << sessionId;
                this_->DropSession(sessionId);
            } else {
                this_->SchedulePingSessionRequest(sessionId);
            }
        };
        TaskScheduler.Delay(TDuration::Seconds(10)).Subscribe([self, callback, sessionId](const TFuture<void>&) {
            auto this_ = self.lock();
            if (!this_) {
                return;
            }

            Yql::DqsProto::PingSessionRequest query;
            query.SetSession(sessionId);

            this_->Service->DoRequest<Yql::DqsProto::PingSessionRequest, Yql::DqsProto::PingSessionResponse>(
                query,
                callback,
                &Yql::DqsProto::DqService::Stub::AsyncPingSession);
        });
    }

    TFuture<void> CloseSessionAsync(const TString& sessionId) {
        std::shared_ptr<TDqGatewaySession> session;
        with_lock (Mutex) {
            auto it = Sessions.find(sessionId);
            if (it != Sessions.end()) {
                session = it->second;
                Sessions.erase(it);
            }
        }
        if (session) {
            return session->Close();
        }
        return MakeFuture();
    }

    TFuture<TResult> ExecutePlan(const TString& sessionId, NDqs::TPlan&& plan, const TVector<TString>& columns,
        const THashMap<TString, TString>& secureParams, const THashMap<TString, TString>& graphParams,
        const TDqSettings::TPtr& settings,
        const TDqProgressWriter& progressWriter, const THashMap<TString, TString>& modulesMapping,
        bool discard, ui64 executionTimeout)
    {
        std::shared_ptr<TDqGatewaySession> session;
        with_lock(Mutex) {
            auto it = Sessions.find(sessionId);
            if (it != Sessions.end()) {
                session = it->second;
            }
        }
        TFailureInjector::Reach("dq_session_was_closed", [&] { session = nullptr; });
        if (!session) {
            YQL_CLOG(ERROR, ProviderDq) << "Session was closed: " << sessionId;
            auto res = NCommon::ResultFromException<TResult>(yexception() << "Session was closed");
            res.Fallback = true;
            res.SetSuccess();
            return MakeFuture(res);
        }
        return session->ExecutePlan(std::move(plan), columns, secureParams, graphParams, settings, progressWriter, modulesMapping, discard, executionTimeout)
            .Apply([](const TFuture<TResult>& f) {
                try {
                    f.TryRethrow();
                } catch (const std::exception& e) {
                    YQL_CLOG(ERROR, ProviderDq) << e.what();
                    return MakeFuture(NCommon::ResultFromException<TResult>(e));
                }
                return f;
            });
    }

private:
    NYdbGrpc::TGRpcClientConfig GrpcConf;
    NYdbGrpc::TGRpcClientLow GrpcClient;
    std::unique_ptr<NYdbGrpc::TServiceConnection<Yql::DqsProto::DqService>> Service;

    TDqTaskScheduler TaskScheduler;
    const TDuration OpenSessionTimeout;

    TMutex Mutex;
    THashMap<TString, std::shared_ptr<TDqGatewaySession>> Sessions;

    std::atomic<bool> IsStopped;
};

class TDqGateway: public IDqGateway {
public:
    TDqGateway(const TString& host, int port, const TString& vanillaJobPath, const TString& vanillaJobMd5, TDuration timeout = TDuration::Minutes(60), TDuration requestTimeout = TDuration::Max())
        : Impl(std::make_shared<TDqGatewayImpl>(host, port, timeout, requestTimeout))
        , VanillaJobPath(vanillaJobPath)
        , VanillaJobMd5(vanillaJobMd5)
    {
    }

    ~TDqGateway() {
        Stop();
    }

    void Stop() override {
        Impl->Stop();
    }

    TFuture<void> OpenSession(const TString& sessionId, const TString& username) override {
        return Impl->OpenSession(sessionId, username);
    }

    TFuture<void> CloseSessionAsync(const TString& sessionId) override {
        return Impl->CloseSessionAsync(sessionId);
    }

    TFuture<TResult> ExecutePlan(const TString& sessionId, NDqs::TPlan&& plan, const TVector<TString>& columns,
        const THashMap<TString, TString>& secureParams, const THashMap<TString, TString>& graphParams,
        const TDqSettings::TPtr& settings,
        const TDqProgressWriter& progressWriter, const THashMap<TString, TString>& modulesMapping,
        bool discard, ui64 executionTimeout) override
    {
        return Impl->ExecutePlan(sessionId, std::move(plan), columns, secureParams, graphParams, settings, progressWriter, modulesMapping, discard, executionTimeout);
    }

    TString GetVanillaJobPath() override {
        return VanillaJobPath;
    }

    TString GetVanillaJobMd5() override {
        return VanillaJobMd5;
    }

private:
    std::shared_ptr<TDqGatewayImpl> Impl;
    TString VanillaJobPath;
    TString VanillaJobMd5;
};

TIntrusivePtr<IDqGateway> CreateDqGateway(const TString& host, int port) {
    return new TDqGateway(host, port, "", "");
}

TIntrusivePtr<IDqGateway> CreateDqGateway(const NProto::TDqConfig& config) {
    return new TDqGateway("localhost", config.GetPort(),
        config.GetYtBackends()[0].GetVanillaJobLite(),
        config.GetYtBackends()[0].GetVanillaJobLiteMd5(),
        TDuration::MilliSeconds(config.GetOpenSessionTimeoutMs()),
        TDuration::MilliSeconds(config.GetRequestTimeoutMs()));
}

} // namespace NYql
