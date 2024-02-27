/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  DataUploader.cpp

  Dmitry Vedenko

**********************************************************************/

#include "DataUploader.h"

#include <variant>

#include <wx/file.h>

#include "CodeConversions.h"

#include "IResponse.h"
#include "Request.h"
#include "NetworkManager.h"

#include "RequestPayload.h"

#include "BasicUI.h"

using namespace audacity::network_manager;

namespace audacity::cloud::audiocom::sync
{
using UploadData = std::variant<std::vector<uint8_t>, std::string>;

struct DataUploader::Response final
{
   DataUploader& Uploader;
   UploadUrls Target;
   std::function<void(ResponseResult)> Callback;
   std::function<void(double)> ProgressCallback;

   int RetriesCount { 3 };
   int RetriesLeft { 3 };

   std::string MimeType;
   UploadData Data;
   std::shared_ptr<IResponse> NetworkResponse;

   ResponseResult CurrentResult;
   CancellationContextPtr CancelContext;

   std::atomic<bool> UploadFailed { false };

   Response(
      DataUploader& uploader, CancellationContextPtr cancellationContex,
      const UploadUrls& target, UploadData data,
      std::string mimeType, std::function<void(ResponseResult)> callback,
      std::function<void(double)> progressCallback)
       : Uploader { uploader }
       , Target { target }
       , Callback { std::move(callback) }
       , ProgressCallback { std::move(progressCallback) }
       , RetriesLeft { RetriesCount }
       , MimeType { std::move(mimeType) }
       , Data { std::move(data) }
       , CancelContext { std::move(cancellationContex) }
   {
      PerformUpload();
   }

   void PerformUpload()
   {
      Request request { Target.UploadUrl };
      request.setHeader(
         common_headers::ContentType,
         MimeType);

      if (std::holds_alternative<std::vector<uint8_t>>(Data))
      {
         auto data = *std::get_if<std::vector<uint8_t>>(&Data);

         NetworkResponse = NetworkManager::GetInstance().doPut(
            request, data.data(), data.size());
         CancelContext->OnCancelled(NetworkResponse);
      }
      else
      {
         auto filePath = *std::get_if<std::string>(&Data);

         NetworkResponse = NetworkManager::GetInstance().doPut(
            request, CreateRequestPayloadStream(filePath));
         CancelContext->OnCancelled(NetworkResponse);

      }

      NetworkResponse->setRequestFinishedCallback(
         [this](auto)
         {
            if (NetworkResponse->getError() == NetworkError::NoError)
               OnUploadSucceeded();
            else
               OnUploadFailed();
         });

      NetworkResponse->setUploadProgressCallback(
         [this](int64_t current, int64_t total)
         {
            if (total <= 0)
            {
               total = 1;
               current = 0;
            }

            ProgressCallback(static_cast<double>(current) / total);
         });
   }

   void OnUploadSucceeded()
   {
      RetriesLeft = RetriesCount;
      ConfirmUpload();
   }

   void OnUploadFailed()
   {
      CurrentResult = GetResponseResult(*NetworkResponse, false);

      if (
         CurrentResult.Code == ResponseResultCode::ConnectionFailed &&
         --RetriesLeft > 0)
      {
         PerformUpload();
      }
      else
      {
         RetriesLeft = RetriesCount;
         FailUpload();
      }
   }

   void ConfirmUpload()
   {
      Data = {};
      Request request { Target.SuccessUrl };

      NetworkResponse =
         NetworkManager::GetInstance().doPost(request, nullptr, 0);
      CancelContext->OnCancelled(NetworkResponse);

      NetworkResponse->setRequestFinishedCallback(
         [this](auto)
         {
            CurrentResult = GetResponseResult(*NetworkResponse, false);

            if (CurrentResult.Code == ResponseResultCode::Success)
            {
               Callback(ResponseResult { ResponseResultCode::Success, {} });
               CleanUp();
            }
            else if (
               CurrentResult.Code == ResponseResultCode::ConnectionFailed &&
               --RetriesLeft > 0)
            {
               ConfirmUpload();
            }
            else
            {
               RetriesLeft = RetriesCount;
               FailUpload();
            }
         });
   }

   void FailUpload()
   {
      if (!UploadFailed.exchange(true))
      {
         Data = {};
         Callback(CurrentResult);
      }

      Request request { Target.FailUrl };

      NetworkResponse = NetworkManager::GetInstance().doPost(request, nullptr, 0);

      NetworkResponse->setRequestFinishedCallback(
         [this](auto)
         {
            const auto result = GetResponseResult(*NetworkResponse, false);

            if (
               result.Code == ResponseResultCode::ConnectionFailed &&
               --RetriesLeft > 0)
               FailUpload();
            else
               CleanUp();

            // Ignore other errors, server will collect garbage
            // and delete the file eventually
         });
      CancelContext->OnCancelled(NetworkResponse);
   }

   void CleanUp()
   {
      BasicUI::CallAfter([this]() { Uploader.RemoveResponse(*this); });
   }
};

DataUploader::~DataUploader()
{
}

DataUploader& DataUploader::Get()
{
   static DataUploader instance;
   return instance;
}

void DataUploader::Upload(
   CancellationContextPtr cancellationContex,
   const ServiceConfig&, const UploadUrls& target, std::vector<uint8_t> data,
   std::function<void(ResponseResult)> callback,
   std::function<void(double)> progressCallback)
{
   if (!callback)
      callback = [](auto...) {};

   if (!progressCallback)
      progressCallback = [](auto...) { return true; };

   if (!cancellationContex)
      cancellationContex = concurrency::CancellationContext::Create();

   auto lock = std::lock_guard { mResponseMutex };

   mResponses.emplace_back(std::make_unique<Response>(
      *this, cancellationContex, target, std::move(data),
      audacity::network_manager::common_content_types::ApplicationXOctetStream,
      std::move(callback), std::move(progressCallback)));
}

void DataUploader::Upload(
   CancellationContextPtr cancellationContex,
   const ServiceConfig& config, const UploadUrls& target, std::string filePath,
   std::function<void(ResponseResult)> callback,
   std::function<void(double)> progressCallback)
{
   if (!callback)
      callback = [](auto...) {};

   if (!progressCallback)
      progressCallback = [](auto...) { return true; };

   if (!cancellationContex)
      cancellationContex = concurrency::CancellationContext::Create();

   if (!wxFileExists(audacity::ToWXString(filePath)))
   {
      if (callback)
         callback(ResponseResult {
            ResponseResultCode::UnknownError,
            audacity::ToUTF8(XO("File not found").Translation()) });

      return;
   }

   auto lock = std::lock_guard { mResponseMutex };

   mResponses.emplace_back(std::make_unique<Response>(
      *this, cancellationContex, target, std::move(filePath),
      audacity::network_manager::common_content_types::ApplicationXOctetStream,
      std::move(callback), std::move(progressCallback)));
}

void DataUploader::RemoveResponse(Response& response)
{
   auto lock = std::lock_guard { mResponseMutex };

   mResponses.erase(
      std::remove_if(
         mResponses.begin(), mResponses.end(),
         [&response](const auto& item) { return item.get() == &response; }),
      mResponses.end());
}
} // namespace audacity::cloud::audiocom::sync