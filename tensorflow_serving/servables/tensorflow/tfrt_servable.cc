/* Copyright 2023 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow_serving/servables/tensorflow/tfrt_servable.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tensorflow/cc/saved_model/signature_constants.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor.pb.h"
#include "tensorflow/core/platform/statusor.h"
#include "tensorflow/core/platform/tracing.h"  // NOLINT
#include "tensorflow/core/tfrt/saved_model/saved_model.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/statusor.h"
#include "tsl/platform/threadpool_options.h"
#include "tensorflow_serving/apis/classification.pb.h"
#include "tensorflow_serving/apis/get_model_metadata.pb.h"
#include "tensorflow_serving/apis/inference.pb.h"
#include "tensorflow_serving/apis/predict.pb.h"
#include "tensorflow_serving/apis/regression.pb.h"
#include "tensorflow_serving/servables/tensorflow/predict_response_tensor_serialization_option.h"
#include "tensorflow_serving/servables/tensorflow/servable.h"
#include "tensorflow_serving/servables/tensorflow/tfrt_classifier.h"
#include "tensorflow_serving/servables/tensorflow/tfrt_multi_inference.h"
#include "tensorflow_serving/servables/tensorflow/tfrt_predict_util.h"
#include "tensorflow_serving/servables/tensorflow/tfrt_regressor.h"
#include "tensorflow_serving/servables/tensorflow/tfrt_saved_model_source_adapter.pb.h"
#include "tensorflow_serving/servables/tensorflow/thread_pool_factory.h"

namespace tensorflow {
namespace serving {

TfrtSavedModelServable::TfrtSavedModelServable(
    absl::string_view name, int64_t version, const TfrtSavedModelConfig& config,
    std::unique_ptr<tfrt_stub::SavedModel> saved_model,
    ThreadPoolFactory* thread_pool_factory)
    : Servable(name, version),
      saved_model_(std::move(saved_model)),
      config_(config),
      thread_pool_factory_(thread_pool_factory) {
  switch (config_.predict_response_tensor_serialization_option()) {
    case TfrtSavedModelConfig::AS_PROTO_FIELD: {
      predict_response_tensor_serialization_option_ =
          internal::PredictResponseTensorSerializationOption::kAsProtoField;
      break;
    }
    case TfrtSavedModelConfig::AS_PROTO_CONTENT: {
      predict_response_tensor_serialization_option_ =
          internal::PredictResponseTensorSerializationOption::kAsProtoContent;
      break;
    }
    default: {
      predict_response_tensor_serialization_option_ =
          internal::PredictResponseTensorSerializationOption::kAsProtoField;
      break;
    }
  }
}

tfrt_stub::SavedModel::RunOptions
TfrtSavedModelServable::GetTFRTSavedModelRunOptions(
    const Servable::RunOptions& run_options) const {
  tfrt_stub::SavedModel::RunOptions options;
  if (run_options.deadline != absl::InfiniteFuture()) {
    options.deadline = absl::ToChronoTime(run_options.deadline);
  }
  options.validate_input_specs = config_.validate_input_specs();
  options.validate_input_specs_dry_run = config_.validate_input_specs_dry_run();
  return options;
}

absl::Status TfrtSavedModelServable::Classify(
    const RunOptions& run_options, const ClassificationRequest& request,
    ClassificationResponse* response) {
  TRACELITERAL("TfrtSavedModelServable::Classify");
  return RunClassify(GetTFRTSavedModelRunOptions(run_options), version(),
                     saved_model_.get(), request, response);
}

absl::Status TfrtSavedModelServable::Regress(const RunOptions& run_options,
                                             const RegressionRequest& request,
                                             RegressionResponse* response) {
  TRACELITERAL("TfrtSavedModelServable::Regress");
  return RunRegress(GetTFRTSavedModelRunOptions(run_options), version(),
                    saved_model_.get(), request, response);
}

absl::Status TfrtSavedModelServable::Predict(const RunOptions& run_options,
                                             const PredictRequest& request,
                                             PredictResponse* response) {
  TRACELITERAL("TfrtSavedModelServable::Predict");
  return internal::RunPredict(
      GetTFRTSavedModelRunOptions(run_options), version(),
      predict_response_tensor_serialization_option_, saved_model_.get(),
      request, response,
      thread_pool_factory_ == nullptr
          ? tsl::thread::ThreadPoolOptions()
          : thread_pool_factory_->GetThreadPools().get());
}

// TODO(b/288096487): Add a unit test once we have the streaming model in OSS.
absl::StatusOr<std::unique_ptr<PredictStreamedContext>>
TfrtSavedModelServable::PredictStreamed(
    const RunOptions& run_options,
    absl::AnyInvocable<void(absl::StatusOr<PredictResponse>)>
        response_callback) {
  return std::make_unique<SingleRequestPredictStreamedContext>(
      [this, run_options, response_callback = std::move(response_callback)](
          const PredictRequest& request) mutable -> absl::Status {
        TRACELITERAL("TfrtSavedModelServable::PredictStreamed");

        auto tfrt_run_options = GetTFRTSavedModelRunOptions(run_options);

        std::string signature_name =
            request.model_spec().signature_name().empty()
                ? kDefaultServingSignatureDefKey
                : request.model_spec().signature_name();

        tensorflow::serving::ModelSpec model_spec = request.model_spec();
        model_spec.set_signature_name(signature_name);
        model_spec.mutable_version()->set_value(version());

        tfrt_run_options.streamed_output_callback =
            [&](absl::flat_hash_map<std::string, tensorflow::Tensor> outputs) {
              tensorflow::serving::PredictResponse response;
              *response.mutable_model_spec() = model_spec;

              for (const auto& [output_key, output_tensor] : outputs) {
                tensorflow::TensorProto& tensor_proto =
                    (*response.mutable_outputs())[output_key];

                // TODO(b/288096487): We are assuming
                // predict_response_tensor_serialization_option_ ==
                // kAsProtoField. The proper way is to check serialize based on
                // the value of predict_response_tensor_serialization_option_.
                output_tensor.AsProtoField(&tensor_proto);
              }

              response_callback(std::move(response));
              // TODO(b/288096487): Add streamz support.
            };

        // The actual responses are passed through `response_callback`. The
        // graph should have no output tensors currently.
        PredictResponse response;

        return internal::RunPredict(
            tfrt_run_options, version(),
            predict_response_tensor_serialization_option_, saved_model_.get(),
            request, &response,
            thread_pool_factory_ == nullptr
                ? tsl::thread::ThreadPoolOptions()
                : thread_pool_factory_->GetThreadPools().get());
      });
}

absl::Status TfrtSavedModelServable::MultiInference(
    const RunOptions& run_options, const MultiInferenceRequest& request,
    MultiInferenceResponse* response) {
  TRACELITERAL("TfrtSavedModelServable::MultiInference");
  return RunMultiInference(GetTFRTSavedModelRunOptions(run_options), version(),
                           saved_model_.get(), request, response);
}

namespace {

absl::Status ValidateGetModelMetadataRequest(
    const GetModelMetadataRequest& request) {
  for (const auto& metadata_field : request.metadata_field()) {
    if (metadata_field != kSignatureDef) {
      return absl::InvalidArgumentError(
          absl::StrCat("Metadata field ", metadata_field, " is not supported"));
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status TfrtSavedModelServable::GetModelMetadata(
    const GetModelMetadataRequest& request,
    GetModelMetadataResponse* response) {
  TRACELITERAL("TfrtSavedModelServable::GetModelMetadata");

  TF_RETURN_IF_ERROR(ValidateGetModelMetadataRequest(request));

  for (const auto& metadata_field : request.metadata_field()) {
    if (metadata_field == kSignatureDef) {
      SignatureDefMap signature_def_map;
      for (const auto& signature :
           saved_model_->GetMetaGraphDef().signature_def()) {
        (*signature_def_map.mutable_signature_def())[signature.first] =
            signature.second;
      }

      auto* response_model_spec = response->mutable_model_spec();
      response_model_spec->set_name(std::string(name()));
      response_model_spec->mutable_version()->set_value(version());
      (*response->mutable_metadata())[kSignatureDef].PackFrom(
          signature_def_map);
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("MetadataField ", metadata_field, " is not supported"));
    }
  }

  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<TfrtSavedModelServable>>
CreateTfrtSavedModelServable(
    const tensorflow::tfrt_stub::SavedModel::Options& options,
    absl::string_view name, int64_t version, absl::string_view saved_model_dir,
    absl::flat_hash_set<std::string> tags) {
  TF_ASSIGN_OR_RETURN(
      auto saved_model,
      tensorflow::tfrt_stub::SavedModelImpl::LoadSavedModel(
          options, saved_model_dir,
          std::unordered_set<std::string>(tags.begin(), tags.end())));

  TfrtSavedModelConfig config;
  return std::make_unique<TfrtSavedModelServable>(
      name, version, config, std::move(saved_model),
      /*thread_pool_factory=*/nullptr);
}

}  // namespace serving
}  // namespace tensorflow
