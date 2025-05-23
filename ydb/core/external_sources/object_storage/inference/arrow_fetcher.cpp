#include "arrow_fetcher.h"
#include "arrow_inferencinator.h"
#include "infer_config.h"

#include <arrow/buffer.h>
#include <arrow/buffer_builder.h>
#include <arrow/csv/chunker.h>
#include <arrow/csv/options.h>
#include <arrow/json/chunker.h>
#include <arrow/json/options.h>
#include <arrow/io/memory.h>
#include <arrow/util/endian.h>

#include <util/generic/guid.h>
#include <util/generic/size_literals.h>

#include <ydb/core/external_sources/object_storage/events.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>

#if defined(_linux_) || defined(_darwin_)
#include <ydb/library/yql/providers/s3/compressors/factory.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/IO/ReadBufferFromString.h>
#endif

namespace NKikimr::NExternalSource::NObjectStorage::NInference {

class TArrowFileFetcher : public NActors::TActorBootstrapped<TArrowFileFetcher> {
    static constexpr uint64_t PrefixSize = 10_MB;
public:
    TArrowFileFetcher(NActors::TActorId s3FetcherId, const THashMap<TString, TString>& params)
        : S3FetcherId_{s3FetcherId}
        , Config_{MakeFormatConfig(params)}
    {
        Y_ABORT_UNLESS(IsArrowInferredFormat(Config_->Format));
        
        auto decompression = params.FindPtr("compression");
        if (decompression) {
            DecompressionFormat_ = *decompression;
        }
    }

    void Bootstrap() {
        Become(&TArrowFileFetcher::WorkingState);
    }

    STRICT_STFUNC(WorkingState,
        HFunc(TEvInferFileSchema, HandleFileRequest);
        HFunc(TEvS3RangeResponse, HandleS3Response);
        HFunc(TEvS3RangeError, HandleS3Error);
    )

    void HandleFileRequest(TEvInferFileSchema::TPtr& ev, const NActors::TActorContext& ctx) {
        const auto& request = *ev->Get();
        TRequest localRequest{
            .Path = request.Path,
            .RequestId = TGUID::Create(),
            .Requester = ev->Sender,
            .MetadataRequest = false,
        };

        switch (Config_->Format) {
            case EFileFormat::CsvWithNames:
            case EFileFormat::TsvWithNames:
            case EFileFormat::JsonEachRow:
            case EFileFormat::JsonList: {
                RequestPartialFile(std::move(localRequest), ctx, 0, 10_MB);
                break;
            }
            case EFileFormat::Parquet: {
                localRequest.MetadataRequest = true;
                RequestPartialFile(std::move(localRequest), ctx, request.Size - 8, request.Size - 4);
                break;
            }
            default: {
                ctx.Send(localRequest.Requester, MakeError(
                    localRequest.Path,
                    NFq::TIssuesIds::UNSUPPORTED,
                    TStringBuilder{} << "unsupported format for inference: " << ConvertFileFormat(Config_->Format))
                );
                return;
            }
            case EFileFormat::Undefined:
                Y_ABORT("Invalid format should be unreachable");
        }
    }

    void HandleS3Response(TEvS3RangeResponse::TPtr& ev, const NActors::TActorContext& ctx) {
        auto& response = *ev->Get();
        auto requestIt = InflightRequests_.find(response.Path);
        Y_ABORT_UNLESS(requestIt != InflightRequests_.end(), "S3 response with path %s for unknown request %s", response.Path.c_str(), response.RequestId.AsGuidString().c_str());

        const auto& request = requestIt->second;

        TString data = std::move(response.Data);
        if (DecompressionFormat_) {
            auto decompressedData = DecompressFile(data, request, ctx);
            if (!decompressedData) {
                return;
            }
            data = std::move(*decompressedData);
        }

        std::shared_ptr<arrow::io::RandomAccessFile> file;
        switch (Config_->Format) {
            case EFileFormat::CsvWithNames:
            case EFileFormat::TsvWithNames: {
                file = CleanupCsvFile(data, request, std::dynamic_pointer_cast<CsvConfig>(Config_)->ParseOpts, ctx);
                if (!file) {
                    return;
                }
                ctx.Send(request.Requester, new TEvArrowFile(Config_, std::move(file), request.Path));
                break;
            }
            case EFileFormat::Parquet: {
                if (request.MetadataRequest) {
                    HandleMetadataSizeRequest(data, request, ctx);
                    return;
                }
                file = BuildParquetFileFromMetadata(data, request, ctx);
                if (!file) {
                    return;
                }
                ctx.Send(request.Requester, new TEvArrowFile(Config_, std::move(file), request.Path));
                break;
            }
            case EFileFormat::JsonEachRow:
            case EFileFormat::JsonList: {
                file = CleanupJsonFile(data, request, std::dynamic_pointer_cast<JsonConfig>(Config_)->ParseOpts, ctx);
                if (!file) {
                    return;
                }
                ctx.Send(request.Requester, new TEvArrowFile(Config_, std::move(file), request.Path));
                break;
            }
            case EFileFormat::Undefined:
            default:
                Y_ABORT("Invalid format should be unreachable");
        }
    }

    void HandleS3Error(TEvS3RangeError::TPtr& ev, const NActors::TActorContext& ctx) {
        auto& error = *ev->Get();

        auto resultError = MakeError(
            error.Path,
            NFq::TIssuesIds::INTERNAL_ERROR,
            TStringBuilder{} << "couldn't fetch data from S3, curl code: " << static_cast<ui32>(error.CurlResponseCode) << ", http code: " << error.HttpCode,
            std::move(error.Issues)
        );
        SendError(ctx, resultError);
    }
private:
    struct TRequest {
        TString Path;
        TGUID RequestId;
        uint64_t From = 0;
        uint64_t To = 0;
        NActors::TActorId Requester;
        bool MetadataRequest;
    };

    // Reading file

    void RequestPartialFile(TRequest&& insertedRequest, const NActors::TActorContext& ctx, uint64_t from, uint64_t to) {
        auto path = insertedRequest.Path;
        insertedRequest.From = from;
        insertedRequest.To = to;
        auto it = InflightRequests_.try_emplace(path, std::move(insertedRequest));
        Y_ABORT_UNLESS(it.second, "couldn't insert request for path: %s", path.c_str());

        const auto& request = it.first->second;
        auto s3Request = new TEvRequestS3Range(
            request.Path,
            request.From, request.To,
            request.RequestId,
            SelfId()
        );
        ctx.Send(S3FetcherId_, s3Request);
    }

    void HandleAsRAFile(TRequest&& insertedRequest, const NActors::TActorContext& ctx) {
        auto format = Config_->Format;
        auto error = MakeError(
            insertedRequest.Path, NFq::TIssuesIds::UNSUPPORTED,
            TStringBuilder{} << "got unsupported format: " << ConvertFileFormat(format) << '(' << static_cast<ui32>(format) << ')'
        );
        SendError(ctx, error);
    }

    // Cutting file

    TMaybe<TString> DecompressFile(const TString& data, const TRequest& request, const NActors::TActorContext& ctx) {
#if defined(_linux_) || defined(_darwin_)
        try {
            NDB::ReadBufferFromString dataBuffer(data);
            auto decompressorBuffer = NYql::MakeDecompressor(dataBuffer, *DecompressionFormat_);
            if (!decompressorBuffer) {
                auto error = MakeError(
                    request.Path,
                    NFq::TIssuesIds::INTERNAL_ERROR,
                    TStringBuilder{} << "unknown compression: " << *DecompressionFormat_ << ". Use one of: gzip, zstd, lz4, brotli, bzip2, xz" 
                );
                SendError(ctx, error);
                return {};
            }

            TStringBuilder decompressedData;
            while (!decompressorBuffer->eof() && decompressedData.size() < 10_MB) {
                decompressorBuffer->nextIfAtEnd();
                size_t maxDecompressedChunkSize = std::min(
                    decompressorBuffer->available(),                
                    10_MB - decompressedData.size()
                );
                TString decompressedChunk{maxDecompressedChunkSize, ' '};
                decompressorBuffer->read(&decompressedChunk.front(), maxDecompressedChunkSize);
                decompressedData << decompressedChunk;
            }
            return std::move(decompressedData);
        } catch (const std::exception& error) {
            auto errorEv = MakeError(
                request.Path,
                NFq::TIssuesIds::INTERNAL_ERROR,
                TStringBuilder{} << "couldn't decompress file, check compression params: " << error.what()
            );
            SendError(ctx, errorEv);
            return {};
        }
#else
        auto error = MakeError(
            request.Path,
            NFq::TIssuesIds::INTERNAL_ERROR,
            TStringBuilder{} << "inference with decompression is not supported on windows"
        );
        SendError(ctx, error);
        return {};
#endif
    }

    std::shared_ptr<arrow::io::RandomAccessFile> CleanupCsvFile(const TString& data, const TRequest& request, const arrow::csv::ParseOptions& options, const NActors::TActorContext& ctx) {
        auto chunker = arrow::csv::MakeChunker(options);
        std::shared_ptr<arrow::Buffer> whole, partial;
        auto arrowData = BuildBufferFromData(data, request, ctx);
        if (!arrowData) {
            return nullptr;
        }
        auto status = chunker->Process(arrowData, &whole, &partial);

        if (!status.ok()) {
            auto error = MakeError(
                request.Path,
                NFq::TIssuesIds::INTERNAL_ERROR,
                TStringBuilder{} << "couldn't run arrow CSV chunker for " << request.Path << ": " << status.ToString()
            );
            SendError(ctx, error);
            return nullptr;
        }

        return std::make_shared<arrow::io::BufferReader>(std::move(whole));
    }

    void HandleMetadataSizeRequest(const TString& data, TRequest request, const NActors::TActorContext& ctx) {
        uint32_t metadataSize = arrow::BitUtil::FromLittleEndian<uint32_t>(ReadUnaligned<uint32_t>(data.data()));

        if (metadataSize > 10_MB) {
            auto error = MakeError(
                request.Path,
                NFq::TIssuesIds::INTERNAL_ERROR,
                TStringBuilder{} << "couldn't load parquet metadata, size is bigger than 10MB : " << metadataSize
            );
            SendError(ctx, error);
            return;
        }

        InflightRequests_.erase(request.Path);

        TRequest localRequest{
            .Path = request.Path,
            .RequestId = TGUID::Create(),
            .Requester = request.Requester,
            .MetadataRequest = false,
        };
        RequestPartialFile(std::move(localRequest), ctx, request.From - metadataSize, request.To + 4);
    }

    std::shared_ptr<arrow::io::RandomAccessFile> BuildParquetFileFromMetadata(const TString& data, const TRequest& request, const NActors::TActorContext& ctx) {
        auto arrowData = BuildBufferFromData(data, request, ctx);
        if (!arrowData) {
            return nullptr;
        }
        return std::make_shared<arrow::io::BufferReader>(std::move(arrowData));
    }

    std::shared_ptr<arrow::io::RandomAccessFile> CleanupJsonFile(const TString& data, const TRequest& request, const arrow::json::ParseOptions& options, const NActors::TActorContext& ctx) {
        auto chunker = arrow::json::MakeChunker(options);
        std::shared_ptr<arrow::Buffer> whole, partial;
        auto arrowData = BuildBufferFromData(data, request, ctx);
        if (!arrowData) {
            return nullptr;
        }

        if (Config_->Format == EFileFormat::JsonList) {
            auto empty = std::make_shared<arrow::Buffer>(nullptr, 0);
            int64_t count = 1;
            auto status = chunker->ProcessSkip(empty, arrowData, false, &count, &whole);
            
            if (!status.ok()) {
                auto error = MakeError(
                    request.Path,
                    NFq::TIssuesIds::INTERNAL_ERROR,
                    TStringBuilder{} << "couldn't run arrow json chunker for " << request.Path << ": " << status.ToString()
                );
                SendError(ctx, error);
                return nullptr;
            }

            arrowData = std::move(whole);
        }

        auto status = chunker->Process(arrowData, &whole, &partial);

        if (!status.ok()) {
            auto error = MakeError(
                request.Path,
                NFq::TIssuesIds::INTERNAL_ERROR,
                TStringBuilder{} << "couldn't run arrow json chunker for " << request.Path << ": " << status.ToString()
            );
            SendError(ctx, error);
            return nullptr;
        }

        return std::make_shared<arrow::io::BufferReader>(std::move(whole));
    }

    std::shared_ptr<arrow::Buffer> BuildBufferFromData(const TString& data, const TRequest& request, const NActors::TActorContext& ctx) {
        auto dataBuffer = std::make_shared<arrow::Buffer>(nullptr, 0);
        arrow::BufferBuilder builder;
        auto buildRes = builder.Append(data.data(), data.size());
        if (!buildRes.ok()) {
            auto error = MakeError(
                request.Path,
                NFq::TIssuesIds::INTERNAL_ERROR,
                TStringBuilder{} << "couldn't read data from S3Fetcher: " << buildRes.ToString()
            );
            SendError(ctx, error);
            return nullptr;
        }

        buildRes = builder.Finish(&dataBuffer);
        if (!buildRes.ok()) {
            auto error = MakeError(
                request.Path,
                NFq::TIssuesIds::INTERNAL_ERROR,
                TStringBuilder{} << "couldn't copy data from S3Fetcher: " << buildRes.ToString()
            );
            SendError(ctx, error);
            return nullptr;
        }

        return dataBuffer;
    }

    // Utility
    void SendError(const NActors::TActorContext& ctx, TEvFileError* error) {
        auto requestIt = InflightRequests_.find(error->Path);
        // Is there something we can do if can't find request?
        Y_ABORT_UNLESS(requestIt != InflightRequests_.end(), "error for unexpected request: %s", error->Path.c_str());
        ctx.Send(requestIt->second.Requester, error);

        InflightRequests_.erase(requestIt);
    }

    // Fields
    NActors::TActorId S3FetcherId_;
    std::shared_ptr<FormatConfig> Config_;
    TMaybe<TString> DecompressionFormat_;
    std::unordered_map<TString, TRequest> InflightRequests_; // Path -> Request
};

NActors::IActor* CreateArrowFetchingActor(NActors::TActorId s3FetcherId, const THashMap<TString, TString>& params) {
    return new TArrowFileFetcher{s3FetcherId, params};
}
} // namespace NKikimr::NExternalSource::NObjectStorage::NInference
