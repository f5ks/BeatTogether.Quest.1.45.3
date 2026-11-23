#include "config.hpp"
#include "hooking.hpp"
#include "logging.hpp"

#include "BGNet/Core/GameLift/GameLiftPlayerSessionProvider.hpp"
#include "BGNet/Core/GameLift/GetMultiplayerInstanceRequest.hpp"
#include "BGNet/Core/GameLift/GetMultiplayerInstanceResponse.hpp"
#include "BGNet/Core/GameLift/PlayerSessionInfo.hpp"
#include "GlobalNamespace/ConnectionFailedException.hpp"
#include "GlobalNamespace/INetworkConfig.hpp"
#include "GlobalNamespace/MultiplayerPlacementErrorCodeMethods.hpp"
#include "GlobalNamespace/NetworkUtility.hpp"
#include "Newtonsoft/Json/Formatting.hpp"
#include "Newtonsoft/Json/JsonConvert.hpp"
#include "Newtonsoft/Json/Linq/JObject.hpp"
#include "Newtonsoft/Json/Linq/JValue.hpp"
#include "System/Net/Http/HttpClient.hpp"
#include "System/Net/Http/HttpClientHandler.hpp"
#include "System/Net/Http/HttpResponseMessage.hpp"
#include "System/Net/Http/StringContent.hpp"
#include "System/String.hpp"
#include "System/Text/Encoding.hpp"
#include "System/Threading/CancellationTokenSource.hpp"
#include "System/Threading/Tasks/TaskCompletionSource_1.hpp"
#include "System/Threading/Tasks/TaskCreationOptions.hpp"
#include "System/Threading/Tasks/Task_1.hpp"
#include "System/Uri.hpp"
#include "UnityEngine/Application.hpp"
#include "beatsaber-hook/shared/safeptr.hpp"
#include "beatsaber-hook/shared/threading.hpp"

#include <algorithm>
#include <memory>

namespace {
    using namespace BGNet::Core::GameLift;
    using namespace GlobalNamespace;
    using namespace Newtonsoft::Json;
    using namespace Newtonsoft::Json::Linq;
    using namespace System::Net::Http;
    using namespace System::Threading;
    using namespace System::Threading::Tasks;

    using SessionSource = TaskCompletionSource_1<PlayerSessionInfo*>;

    // Dispose managed resources on the attached worker, including on a failed or canceled request.
    constexpr auto dispose = [](auto* resource) noexcept {
        try { resource->Dispose(); } catch (...) {}
    };
    constexpr auto disposeClient = [](HttpClient* client) noexcept {
        dispose(static_cast<HttpMessageInvoker*>(client));
    };

    void PlaceLegacyLobby(SessionSource* result, System::Object* request, StringW authUserId,
                          CancellationToken callerToken) {
        try {
            const std::unique_ptr<CancellationTokenSource, decltype(dispose)> deadline(
                CancellationTokenSource::CreateLinkedTokenSource(callerToken));
            deadline->CancelAfter(GameLiftPlayerSessionProvider::kMatchmakingTimeoutMs);
            auto token = deadline->get_Token();
            const auto handler = HttpClientHandler::New_ctor();
            handler->set_AllowAutoRedirect(false);
            const std::unique_ptr<HttpClient, decltype(disposeClient)> client(HttpClient::New_ctor(handler));
            const auto uri = System::Uri::New_ctor(
                StringW(std::string(Config::defaultGraphUrl) + "/beat_saber_get_multiplayer_instance"));
            const auto body = JObject::Parse(JsonConvert::SerializeObject(request));
            body->set_Item(StringW("auth_user_id"), JValue::New_ctor(authUserId));

            while (!token.IsCancellationRequested) {
                const auto proof = MultiplayerCore::API::RequestQuestUserProof(token);
                if (System::String::IsNullOrEmpty(proof)) {
                    result->TrySetException(ConnectionFailedException::New_ctor(ConnectionFailedReason::AuthenticationFailed));
                    return;
                }

                body->set_Item(StringW("single_use_auth_token"), JValue::New_ctor(proof));
                const std::unique_ptr<StringContent, decltype(dispose)> content(StringContent::New_ctor(
                    body->ToString(Formatting::None, nullptr), System::Text::Encoding::get_UTF8(), StringW("application/json")));
                INFO("Requesting a lobby using the legacy BeatTogether placement API");
                const std::unique_ptr<HttpResponseMessage, decltype(dispose)> response(
                    client->PostAsync(uri, content.get(), token)->get_Result());
                if (!response->get_IsSuccessStatusCode()) {
                    WARNING("BeatTogether placement returned HTTP {}", static_cast<int>(response->get_StatusCode()));
                    result->TrySetException(ConnectionFailedException::New_ctor(ConnectionFailedReason::MultiplayerApiUnreachable));
                    return;
                }

                const auto text = response->get_Content()->ReadAsStringAsync()->get_Result();
                const auto boxed = JsonConvert::DeserializeObject(text, i2c::cs_type_of<GetMultiplayerInstanceResponse>());
                if (!boxed) break;
                const auto& placement = *static_cast<GetMultiplayerInstanceResponse*>(
                    i2c::functions::object_unbox(reinterpret_cast<Il2CppObject*>(boxed)));
                INFO("BeatTogether placement returned code {}", placement.errorCode.value__);
                if (placement.errorCode != MultiplayerPlacementErrorCode::Success &&
                    placement.errorCode != MultiplayerPlacementErrorCode::RequestTimeout) {
                    const auto reason = placement.errorCode.value__ >= 50 && placement.errorCode.value__ <= 52
                        ? ConnectionFailedReason::VersionMismatch
                        : MultiplayerPlacementErrorCodeMethods::ToConnectionFailedReason(placement.errorCode);
                    result->TrySetException(ConnectionFailedException::New_ctor(reason));
                    return;
                }

                const auto session = placement.playerSessionInfo;
                if (session && !System::String::IsNullOrEmpty(session->playerSessionId) &&
                    !System::String::IsNullOrEmpty(session->gameSessionId)) {
                    result->TrySetResult(session);
                    return;
                }
                if (System::String::IsNullOrEmpty(placement.ticketId) &&
                    System::String::IsNullOrEmpty(placement.placementId)) break;

                body->set_Item(StringW("ticket_id"), JValue::New_ctor(placement.ticketId));
                body->set_Item(StringW("placement_id"), JValue::New_ctor(placement.placementId));
                Task::Delay(std::max(placement.pollIntervalMs, 100), token)->Wait();
            }

            if (callerToken.IsCancellationRequested) result->TrySetCanceled(callerToken);
            else result->TrySetException(ConnectionFailedException::New_ctor(ConnectionFailedReason::Timeout));
        } catch (...) {
            if (callerToken.IsCancellationRequested) {
                result->TrySetCanceled(callerToken);
            } else {
                WARNING("The BeatTogether legacy placement request could not complete");
                result->TrySetException(ConnectionFailedException::New_ctor(ConnectionFailedReason::MultiplayerApiUnreachable));
            }
        }
    }
}

MAKE_AUTO_HOOK_MATCH(GameLiftPlayerSessionProvider_LegacyPlacement, &GameLiftPlayerSessionProvider::GetGameLiftPlayerSessionInfo,
    Task_1<PlayerSessionInfo*>*, GameLiftPlayerSessionProvider* self, IAuthenticationTokenProvider* authenticationTokenProvider,
    StringW userId, BeatmapLevelSelectionMask mask, GameplayServerConfiguration configuration,
    StringW secret, StringW code, CancellationToken cancellationToken) {
    if (!UsesBeatTogetherLegacyServer()) {
        return GameLiftPlayerSessionProvider_LegacyPlacement(self, authenticationTokenProvider, userId, mask,
            configuration, secret, code, cancellationToken);
    }

    INFO("Using BeatTogether legacy lobby placement");
    auto result = safe_ptr(SessionSource::New_ctor(TaskCreationOptions::RunContinuationsAsynchronously));
    auto task = result->get_Task();
    try {
        const auto questUserId = MultiplayerCore::API::GetQuestAuthenticationUserId(authenticationTokenProvider);
        if (System::String::IsNullOrEmpty(questUserId)) {
            result->TrySetException(ConnectionFailedException::New_ctor(ConnectionFailedReason::AuthenticationFailed));
            return task;
        }

        const auto authUserId = safe_ptr<StringW>(questUserId);
        if (System::String::IsNullOrEmpty(userId)) {
            userId = NetworkUtility::GetHashBase64(StringW("OculusQuest#" + static_cast<std::string>(authUserId.ptr())));
        }
        GetMultiplayerInstanceRequest request;
        request._ctor(UnityEngine::Application::get_version(), self->_networkConfig->get_serviceEnvironment(), userId, mask,
            configuration, AuthenticationToken_PlatformType::OculusQuest, secret, code, self->GetAverageLatencies(),
            nullptr, nullptr, self->_networkConfig->get_customLocation());
        auto payload = safe_ptr(reinterpret_cast<System::Object*>(
            i2c::functions::value_box(i2c::class_of<GetMultiplayerInstanceRequest>(), &request)));
        auto cancellation = safe_ptr(cancellationToken._source);
        il2cpp_thread([result, payload, authUserId, cancellation] {
            PlaceLegacyLobby(result.ptr(), payload.ptr(), authUserId.ptr(), CancellationToken(cancellation.unchecked_ptr()));
        }).detach();
    } catch (...) {
        WARNING("Could not prepare the BeatTogether legacy placement request");
        result->TrySetException(ConnectionFailedException::New_ctor(ConnectionFailedReason::MultiplayerApiUnreachable));
    }
    return task;
}
