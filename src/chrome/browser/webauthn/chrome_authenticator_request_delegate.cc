// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/webauthn/chrome_authenticator_request_delegate.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/base64.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/containers/contains.h"
#include "base/feature_list.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/ranges/algorithm.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_split.h"
#include "base/sys_byteorder.h"
#include "base/values.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "chrome/browser/extensions/api/web_authentication_proxy/web_authentication_proxy_service.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "chrome/browser/sync/device_info_sync_service_factory.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/page_action/page_action_icon_type.h"
#include "chrome/browser/webauthn/authenticator_request_dialog_model.h"
#include "chrome/browser/webauthn/cablev2_devices.h"
#include "chrome/browser/webauthn/webauthn_pref_names.h"
#include "chrome/browser/webauthn/webauthn_switches.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/chrome_version.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/generated_resources.h"
#include "components/device_event_log/device_event_log.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/user_prefs/user_prefs.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/device_service.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "crypto/random.h"
#include "device/fido/cable/v2_handshake.h"
#include "device/fido/features.h"
#include "device/fido/fido_authenticator.h"
#include "device/fido/fido_discovery_factory.h"
#include "extensions/common/constants.h"
#include "third_party/icu/source/common/unicode/locid.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/window_open_disposition.h"

#if BUILDFLAG(IS_MAC)
#include "device/fido/mac/authenticator.h"
#include "device/fido/mac/credential_metadata.h"
#endif

#if BUILDFLAG(IS_WIN)
#include "device/fido/win/authenticator.h"
#endif

#if BUILDFLAG(IS_CHROMEOS)
#include "chromeos/components/webauthn/webauthn_request_registrar.h"
#include "ui/aura/window.h"
#endif

namespace {

ChromeAuthenticatorRequestDelegate::TestObserver* g_observer = nullptr;

// Returns true iff |relying_party_id| is listed in the
// SecurityKeyPermitAttestation policy.
bool IsWebAuthnRPIDListedInSecurityKeyPermitAttestationPolicy(
    content::BrowserContext* browser_context,
    const std::string& relying_party_id) {
  const Profile* profile = Profile::FromBrowserContext(browser_context);
  const PrefService* prefs = profile->GetPrefs();
  const base::Value::List& permit_attestation =
      prefs->GetValueList(prefs::kSecurityKeyPermitAttestation);
  return std::any_of(permit_attestation.begin(), permit_attestation.end(),
                     [&relying_party_id](const base::Value& v) {
                       return v.GetString() == relying_party_id;
                     });
}

bool IsOriginListedInEnterpriseAttestationSwitch(
    const url::Origin& caller_origin) {
  std::string cmdline_origins =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          webauthn::switches::kPermitEnterpriseAttestationOriginList);
  std::vector<base::StringPiece> origin_strings = base::SplitStringPiece(
      cmdline_origins, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  return base::ranges::any_of(
      origin_strings, [&caller_origin](base::StringPiece origin_string) {
        return url::Origin::Create(GURL(origin_string)) == caller_origin;
      });
}

#if BUILDFLAG(IS_WIN)
// kWebAuthnLastOperationWasNativeAPI is a boolean preference that records
// whether the last successful operation used the Windows native API. If so
// then we'll try and jump directly to it next time.
const char kWebAuthnLastOperationWasNativeAPI[] =
    "webauthn.last_op_used_native_api";
#endif

#if BUILDFLAG(IS_MAC)
const char kWebAuthnTouchIdMetadataSecretPrefName[] =
    "webauthn.touchid.metadata_secret";
#endif

// CableLinkingEventHandler handles linking information sent by caBLEv2
// authenticators. This linking information can come after the WebAuthn
// operation has resolved and thus after the
// `ChromeAuthenticatorRequestDelegate` has been destroyed. Thus this object is
// owned by the callback itself, and can save linking information until the
// point where the `Profile` itself is destroyed.
class CableLinkingEventHandler : public ProfileObserver {
 public:
  explicit CableLinkingEventHandler(Profile* profile) : profile_(profile) {
    profile_->AddObserver(this);
  }

  ~CableLinkingEventHandler() override {
    if (profile_) {
      profile_->RemoveObserver(this);
      profile_ = nullptr;
    }
  }

  void OnNewCablePairing(std::unique_ptr<device::cablev2::Pairing> pairing) {
    if (!profile_) {
      FIDO_LOG(DEBUG) << "Linking event was discarded because it was received "
                         "after the profile was destroyed.";
      return;
    }

    // Drop linking in Incognito sessions. While an argument could be made that
    // it's OK to persist them, this seems like the safe option.
    if (profile_->IsOffTheRecord()) {
      FIDO_LOG(DEBUG) << "Linking event was discarded because the profile is "
                         "Off The Record.";
      return;
    }

    cablev2::AddPairing(profile_, std::move(pairing));
  }

  // ProfileObserver:

  void OnProfileWillBeDestroyed(Profile* profile) override {
    DCHECK_EQ(profile, profile_);
    profile_->RemoveObserver(this);
    profile_ = nullptr;
  }

 private:
  raw_ptr<Profile> profile_;
};

absl::optional<
    std::pair<AuthenticatorRequestDialogModel::ExperimentServerLinkSheet,
              AuthenticatorRequestDialogModel::ExperimentServerLinkTitle>>
GetServerLinkExperiments(
    base::span<const device::CableDiscoveryData> pairings_from_extension) {
  base::span<const uint8_t> experiment_bytes;
  for (const auto& pairing : pairings_from_extension) {
    if (pairing.version != device::CableDiscoveryData::Version::V2 ||
        pairing.v2->experiments.empty()) {
      continue;
    }

    base::span<const uint8_t> candidate = pairing.v2->experiments;

    if (experiment_bytes.empty()) {
      experiment_bytes = candidate;
      continue;
    }

    if (candidate.size() != experiment_bytes.size() ||
        memcmp(candidate.data(), experiment_bytes.data(), candidate.size()) !=
            0) {
      FIDO_LOG(ERROR) << "Server-link experiment data inconsistent. Ignoring.";
      return absl::nullopt;
    }
  }

  if (experiment_bytes.empty()) {
    return absl::nullopt;
  }

  if (experiment_bytes.size() % sizeof(uint32_t) != 0) {
    FIDO_LOG(ERROR) << "Server-link experiment data is not a multiple of four "
                       "bytes. Ignoring.";
    return absl::nullopt;
  }

  constexpr AuthenticatorRequestDialogModel::ExperimentServerLinkSheet
      kSheetArms[] = {
          AuthenticatorRequestDialogModel::ExperimentServerLinkSheet::CONTROL,
          AuthenticatorRequestDialogModel::ExperimentServerLinkSheet::ARM_2,
          AuthenticatorRequestDialogModel::ExperimentServerLinkSheet::ARM_3,
          AuthenticatorRequestDialogModel::ExperimentServerLinkSheet::ARM_4,
          AuthenticatorRequestDialogModel::ExperimentServerLinkSheet::ARM_5,
          AuthenticatorRequestDialogModel::ExperimentServerLinkSheet::ARM_6,
      };

  constexpr AuthenticatorRequestDialogModel::ExperimentServerLinkTitle
      kTitleArms[] = {
          AuthenticatorRequestDialogModel::ExperimentServerLinkTitle::CONTROL,
          AuthenticatorRequestDialogModel::ExperimentServerLinkTitle::
              UNLOCK_YOUR_PHONE,
      };

  absl::optional<AuthenticatorRequestDialogModel::ExperimentServerLinkSheet>
      sheet_experiment;
  absl::optional<AuthenticatorRequestDialogModel::ExperimentServerLinkTitle>
      title_experiment;

  for (size_t i = 0; i < experiment_bytes.size(); i += sizeof(uint32_t)) {
    uint32_t experiment_id;
    memcpy(&experiment_id, &experiment_bytes[i], sizeof(experiment_id));
    experiment_id = base::ByteSwap(experiment_id);

    for (const auto& arm : kSheetArms) {
      if (experiment_id == static_cast<uint32_t>(arm)) {
        if (sheet_experiment.has_value()) {
          LOG(ERROR) << "Duplicate values for sheet experiment.";
          return absl::nullopt;
        }
        sheet_experiment = arm;
        break;
      }
    }

    for (const auto& arm : kTitleArms) {
      if (experiment_id == static_cast<uint32_t>(arm)) {
        if (title_experiment.has_value()) {
          LOG(ERROR) << "Duplicate values for title experiment.";
          return absl::nullopt;
        }
        title_experiment = arm;
      }
    }

    FIDO_LOG(DEBUG) << "Ignoring unknown experiment ID " << experiment_id;
  }

  return std::make_pair(
      sheet_experiment.value_or(
          AuthenticatorRequestDialogModel::ExperimentServerLinkSheet::CONTROL),
      title_experiment.value_or(
          AuthenticatorRequestDialogModel::ExperimentServerLinkTitle::CONTROL));
}

}  // namespace

// ---------------------------------------------------------------------
// ChromeWebAuthenticationDelegate
// ---------------------------------------------------------------------

ChromeWebAuthenticationDelegate::~ChromeWebAuthenticationDelegate() = default;

#if !BUILDFLAG(IS_ANDROID)

bool ChromeWebAuthenticationDelegate::
    OverrideCallerOriginAndRelyingPartyIdValidation(
        content::BrowserContext* browser_context,
        const url::Origin& caller_origin,
        const std::string& relying_party_id) {
  // Allow chrome-extensions:// origins to make WebAuthn requests.
  // `MaybeGetRelyingPartyId` will override the RP ID to use when processing
  // requests from extensions.
  return caller_origin.scheme() == extensions::kExtensionScheme &&
         caller_origin.host() == relying_party_id;
}

bool ChromeWebAuthenticationDelegate::OriginMayUseRemoteDesktopClientOverride(
    content::BrowserContext* browser_context,
    const url::Origin& caller_origin) {
  // Allow the Google-internal version of Chrome Remote Desktop to use the
  // RemoteDesktopClientOverride extension and make WebAuthn
  // requests on behalf of other origins, if a corresponding enteprise policy is
  // enabled.
  //
  // The policy explicitly does not cover external instances of CRD. It
  // must not be extended to other origins or be made configurable without going
  // through security review.
  if (!base::FeatureList::IsEnabled(
          device::kWebAuthnGoogleCorpRemoteDesktopClientPrivilege)) {
    return false;
  }

  const Profile* profile = Profile::FromBrowserContext(browser_context);
  const PrefService* prefs = profile->GetPrefs();
  const bool google_corp_remote_proxied_request_allowed =
      prefs->GetBoolean(webauthn::pref_names::kRemoteProxiedRequestsAllowed);
  if (!google_corp_remote_proxied_request_allowed) {
    return false;
  }

  constexpr char kGoogleCorpCrdOrigin[] =
      "https://remotedesktop.corp.google.com";
  if (caller_origin == url::Origin::Create(GURL(kGoogleCorpCrdOrigin))) {
    return true;
  }

  // An additional origin can be passed on the command line for testing.
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          webauthn::switches::kRemoteProxiedRequestsAllowedAdditionalOrigin)) {
    return false;
  }
  // Note that `cmdline_allowed_origin` will be opaque if the flag is not a
  // valid URL, which won't match `caller_origin`.
  const url::Origin cmdline_allowed_origin = url::Origin::Create(
      GURL(base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          webauthn::switches::kRemoteProxiedRequestsAllowedAdditionalOrigin)));
  return caller_origin == cmdline_allowed_origin;
}

absl::optional<std::string>
ChromeWebAuthenticationDelegate::MaybeGetRelyingPartyIdOverride(
    const std::string& claimed_relying_party_id,
    const url::Origin& caller_origin) {
  // Don't override cryptotoken processing.
  constexpr char kCryptotokenOrigin[] =
      "chrome-extension://kmendfapggjehodndflmmgagdbamhnfd";
  if (caller_origin == url::Origin::Create(GURL(kCryptotokenOrigin))) {
    return absl::nullopt;
  }

  // Otherwise, allow extensions to use WebAuthn and map their origins
  // directly to RP IDs.
  if (caller_origin.scheme() == extensions::kExtensionScheme) {
    // `OverrideCallerOriginAndRelyingPartyIdValidation' ensures an extension
    // must only use the extension identifier as the RP ID, no flexibility is
    // permitted. When interacting with authenticators, however, we use the
    // whole origin to avoid collisions with the RP ID space for HTTPS origins.
    return caller_origin.Serialize();
  }

  return absl::nullopt;
}

bool ChromeWebAuthenticationDelegate::ShouldPermitIndividualAttestation(
    content::BrowserContext* browser_context,
    const url::Origin& caller_origin,
    const std::string& relying_party_id) {
  constexpr char kGoogleCorpAppId[] =
      "https://www.gstatic.com/securitykey/a/google.com/origins.json";

  // If the RP ID is actually the Google corp App ID (because the request is
  // actually a U2F request originating from cryptotoken), or is listed in the
  // enterprise policy, signal that individual attestation is permitted.
  return relying_party_id == kGoogleCorpAppId ||
         IsOriginListedInEnterpriseAttestationSwitch(caller_origin) ||
         IsWebAuthnRPIDListedInSecurityKeyPermitAttestationPolicy(
             browser_context, relying_party_id);
}

bool ChromeWebAuthenticationDelegate::SupportsResidentKeys(
    content::RenderFrameHost* render_frame_host) {
  return true;
}

bool ChromeWebAuthenticationDelegate::IsFocused(
    content::WebContents* web_contents) {
  return web_contents->GetVisibility() == content::Visibility::VISIBLE;
}

#if BUILDFLAG(IS_WIN)
void ChromeWebAuthenticationDelegate::OperationSucceeded(
    content::BrowserContext* browser_context,
    bool used_win_api) {
  // If a registration or assertion operation was successful, record whether the
  // Windows native API was used for it. If so we'll jump directly to the native
  // UI for the next operation.
  Profile* const profile = Profile::FromBrowserContext(browser_context);
  if (profile->IsOffTheRecord()) {
    return;
  }

  profile->GetPrefs()->SetBoolean(kWebAuthnLastOperationWasNativeAPI,
                                  used_win_api);
}
#endif

absl::optional<bool> ChromeWebAuthenticationDelegate::
    IsUserVerifyingPlatformAuthenticatorAvailableOverride(
        content::RenderFrameHost* render_frame_host) {
  // If the testing API is active, its override takes precedence.
  absl::optional<bool> testing_api_override =
      content::WebAuthenticationDelegate::
          IsUserVerifyingPlatformAuthenticatorAvailableOverride(
              render_frame_host);
  if (testing_api_override) {
    return *testing_api_override;
  }

#if BUILDFLAG(IS_WIN)
  // TODO(crbug.com/908622): Enable platform authenticators in Incognito on
  // Windows once the API allows triggering an adequate warning dialog.
  if (render_frame_host->GetBrowserContext()->IsOffTheRecord()) {
    return false;
  }
#endif

  // Chrome disables platform authenticators is Guest sessions. They may be
  // available (behind an additional interstitial) in Incognito mode.
  Profile* profile =
      Profile::FromBrowserContext(render_frame_host->GetBrowserContext());
  if (profile->IsGuestSession()) {
    return false;
  }

  return absl::nullopt;
}

content::WebAuthenticationRequestProxy*
ChromeWebAuthenticationDelegate::MaybeGetRequestProxy(
    content::BrowserContext* browser_context) {
  return extensions::WebAuthenticationProxyServiceFactory::GetForBrowserContext(
      browser_context);
}

#endif  // !IS_ANDROID

#if BUILDFLAG(IS_MAC)
// static
ChromeWebAuthenticationDelegate::TouchIdAuthenticatorConfig
ChromeWebAuthenticationDelegate::TouchIdAuthenticatorConfigForProfile(
    Profile* profile) {
  constexpr char kKeychainAccessGroup[] =
      MAC_TEAM_IDENTIFIER_STRING "." MAC_BUNDLE_IDENTIFIER_STRING ".webauthn";

  std::string metadata_secret =
      profile->GetPrefs()->GetString(kWebAuthnTouchIdMetadataSecretPrefName);
  if (metadata_secret.empty() ||
      !base::Base64Decode(metadata_secret, &metadata_secret)) {
    metadata_secret = device::fido::mac::GenerateCredentialMetadataSecret();
    profile->GetPrefs()->SetString(
        kWebAuthnTouchIdMetadataSecretPrefName,
        base::Base64Encode(base::as_bytes(base::make_span(metadata_secret))));
  }

  return TouchIdAuthenticatorConfig{
      .keychain_access_group = kKeychainAccessGroup,
      .metadata_secret = std::move(metadata_secret)};
}

absl::optional<ChromeWebAuthenticationDelegate::TouchIdAuthenticatorConfig>
ChromeWebAuthenticationDelegate::GetTouchIdAuthenticatorConfig(
    content::BrowserContext* browser_context) {
  return TouchIdAuthenticatorConfigForProfile(
      Profile::FromBrowserContext(browser_context));
}
#endif  // BUILDFLAG(IS_MAC)

#if BUILDFLAG(IS_CHROMEOS)
content::WebAuthenticationDelegate::ChromeOSGenerateRequestIdCallback
ChromeWebAuthenticationDelegate::GetGenerateRequestIdCallback(
    content::RenderFrameHost* render_frame_host) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  aura::Window* window =
      render_frame_host->GetNativeView()->GetToplevelWindow();
  return chromeos::webauthn::WebAuthnRequestRegistrar::Get()
      ->GetRegisterCallback(window);
}
#endif  // BUILDFLAG(IS_CHROMEOS)

// ---------------------------------------------------------------------
// ChromeAuthenticatorRequestDelegate
// ---------------------------------------------------------------------

// static
void ChromeAuthenticatorRequestDelegate::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
#if BUILDFLAG(IS_WIN)
  registry->RegisterBooleanPref(kWebAuthnLastOperationWasNativeAPI, false);
#endif
#if BUILDFLAG(IS_MAC)
  registry->RegisterStringPref(kWebAuthnTouchIdMetadataSecretPrefName,
                               std::string());
#endif
  cablev2::RegisterProfilePrefs(registry);
}

ChromeAuthenticatorRequestDelegate::ChromeAuthenticatorRequestDelegate(
    content::RenderFrameHost* render_frame_host)
    : render_frame_host_id_(render_frame_host->GetGlobalId()),
      dialog_model_(std::make_unique<AuthenticatorRequestDialogModel>(
          content::WebContents::FromRenderFrameHost(GetRenderFrameHost()))) {
  dialog_model_->AddObserver(this);
  if (g_observer) {
    g_observer->Created(this);
  }
}

ChromeAuthenticatorRequestDelegate::~ChromeAuthenticatorRequestDelegate() {
  // Currently, completion of the request is indicated by //content destroying
  // this delegate.
  dialog_model_->OnRequestComplete();
  dialog_model_->RemoveObserver(this);
}

// static
void ChromeAuthenticatorRequestDelegate::SetGlobalObserverForTesting(
    TestObserver* observer) {
  CHECK(!observer || !g_observer);
  g_observer = observer;
}

base::WeakPtr<ChromeAuthenticatorRequestDelegate>
ChromeAuthenticatorRequestDelegate::AsWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

void ChromeAuthenticatorRequestDelegate::SetRelyingPartyId(
    const std::string& rp_id) {
  dialog_model_->set_relying_party_id(rp_id);
}

bool ChromeAuthenticatorRequestDelegate::DoesBlockRequestOnFailure(
    InterestingFailureReason reason) {
  if (!IsWebAuthnUIEnabled())
    return false;

  switch (reason) {
    case InterestingFailureReason::kTimeout:
      dialog_model_->OnRequestTimeout();
      break;
    case InterestingFailureReason::kKeyNotRegistered:
      dialog_model_->OnActivatedKeyNotRegistered();
      break;
    case InterestingFailureReason::kKeyAlreadyRegistered:
      dialog_model_->OnActivatedKeyAlreadyRegistered();
      break;
    case InterestingFailureReason::kSoftPINBlock:
      dialog_model_->OnSoftPINBlock();
      break;
    case InterestingFailureReason::kHardPINBlock:
      dialog_model_->OnHardPINBlock();
      break;
    case InterestingFailureReason::kAuthenticatorRemovedDuringPINEntry:
      dialog_model_->OnAuthenticatorRemovedDuringPINEntry();
      break;
    case InterestingFailureReason::kAuthenticatorMissingResidentKeys:
      dialog_model_->OnAuthenticatorMissingResidentKeys();
      break;
    case InterestingFailureReason::kAuthenticatorMissingUserVerification:
      dialog_model_->OnAuthenticatorMissingUserVerification();
      break;
    case InterestingFailureReason::kAuthenticatorMissingLargeBlob:
      dialog_model_->OnAuthenticatorMissingLargeBlob();
      break;
    case InterestingFailureReason::kNoCommonAlgorithms:
      dialog_model_->OnNoCommonAlgorithms();
      break;
    case InterestingFailureReason::kStorageFull:
      dialog_model_->OnAuthenticatorStorageFull();
      break;
    case InterestingFailureReason::kUserConsentDenied:
      dialog_model_->OnUserConsentDenied();
      break;
    case InterestingFailureReason::kWinUserCancelled:
      return dialog_model_->OnWinUserCancelled();
  }
  return true;
}

void ChromeAuthenticatorRequestDelegate::RegisterActionCallbacks(
    base::OnceClosure cancel_callback,
    base::RepeatingClosure start_over_callback,
    AccountPreselectedCallback account_preselected_callback,
    device::FidoRequestHandlerBase::RequestCallback request_callback,
    base::RepeatingClosure bluetooth_adapter_power_on_callback) {
  request_callback_ = request_callback;
  cancel_callback_ = std::move(cancel_callback);
  start_over_callback_ = std::move(start_over_callback);
  account_preselected_callback_ = std::move(account_preselected_callback);

  dialog_model_->SetRequestCallback(request_callback);
  dialog_model_->SetAccountPreselectedCallback(account_preselected_callback_);
  dialog_model_->SetBluetoothAdapterPowerOnCallback(
      bluetooth_adapter_power_on_callback);
}

void ChromeAuthenticatorRequestDelegate::ShouldReturnAttestation(
    const std::string& relying_party_id,
    const device::FidoAuthenticator* authenticator,
    bool is_enterprise_attestation,
    base::OnceCallback<void(bool)> callback) {
  if (IsWebAuthnRPIDListedInSecurityKeyPermitAttestationPolicy(
          GetBrowserContext(), relying_party_id)) {
    // Enterprise attestations should have been approved already and not reach
    // this point.
    DCHECK(!is_enterprise_attestation);
    std::move(callback).Run(true);
    return;
  }

  // Cryptotoken displays its own attestation consent prompt.
  // AuthenticatorCommon does not invoke ShouldReturnAttestation() for those
  // requests.
  if (disable_ui_) {
    NOTREACHED();
    std::move(callback).Run(false);
    return;
  }

#if BUILDFLAG(IS_WIN)
  if (authenticator->GetType() == device::FidoAuthenticator::Type::kWinNative &&
      static_cast<const device::WinWebAuthnApiAuthenticator*>(authenticator)
          ->ShowsPrivacyNotice()) {
    // The OS' native API includes an attestation prompt.
    std::move(callback).Run(true);
    return;
  }
#endif  // BUILDFLAG(IS_WIN)

  dialog_model_->RequestAttestationPermission(is_enterprise_attestation,
                                              std::move(callback));
}

void ChromeAuthenticatorRequestDelegate::ConfigureCable(
    const url::Origin& origin,
    device::FidoRequestType request_type,
    base::span<const device::CableDiscoveryData> pairings_from_extension,
    device::FidoDiscoveryFactory* discovery_factory) {
  phone_names_.clear();
  phone_public_keys_.clear();

  const bool cable_extension_permitted = ShouldPermitCableExtension(origin);
  const bool cable_extension_provided =
      cable_extension_permitted && !pairings_from_extension.empty();

  auto experiments = GetServerLinkExperiments(pairings_from_extension);
  if (experiments.has_value()) {
    std::tie(dialog_model_->experiment_server_link_sheet_,
             dialog_model_->experiment_server_link_title_) = *experiments;
  }

  if (g_observer) {
    for (const auto& pairing : pairings_from_extension) {
      if (pairing.version == device::CableDiscoveryData::Version::V2) {
        g_observer->CableV2ExtensionSeen(
            pairing.v2->server_link_data, pairing.v2->experiments,
            dialog_model_->experiment_server_link_sheet_,
            dialog_model_->experiment_server_link_title_);
      }
    }
  }

#if BUILDFLAG(IS_LINUX)
  // No caBLEv1 on Linux. It tends to crash bluez.
  if (std::any_of(pairings_from_extension.begin(),
                  pairings_from_extension.end(),
                  [](const device::CableDiscoveryData& v) -> bool {
                    return v.version == device::CableDiscoveryData::Version::V1;
                  })) {
    pairings_from_extension = base::span<const device::CableDiscoveryData>();
  }
#endif

  std::vector<device::CableDiscoveryData> pairings;
  if (cable_extension_permitted) {
    pairings.insert(pairings.end(), pairings_from_extension.begin(),
                    pairings_from_extension.end());
  }
  const bool cable_extension_accepted = !pairings.empty();
  const bool cablev2_extension_provided =
      std::any_of(pairings.begin(), pairings.end(),
                  [](const device::CableDiscoveryData& v) -> bool {
                    return v.version == device::CableDiscoveryData::Version::V2;
                  });

  std::vector<std::unique_ptr<device::cablev2::Pairing>> paired_phones;
  std::vector<AuthenticatorRequestDialogModel::PairedPhone>
      paired_phone_entries;
  base::RepeatingCallback<void(size_t)> contact_phone_callback;
  if (!cable_extension_provided ||
      base::FeatureList::IsEnabled(device::kWebAuthCableExtensionAnywhere)) {
    DCHECK(phone_names_.empty());
    DCHECK(phone_public_keys_.empty());

    std::unique_ptr<cablev2::KnownDevices> known_devices =
        cablev2::KnownDevices::FromProfile(
            Profile::FromBrowserContext(GetBrowserContext()));
    if (g_observer) {
      known_devices->synced_devices =
          g_observer->GetCablePairingsFromSyncedDevices();
    }
    paired_phones = cablev2::MergeDevices(std::move(known_devices),
                                          &icu::Locale::getDefault());

    // The debug log displays in reverse order, so the headline is emitted after
    // the names.
    for (const auto& pairing : paired_phones) {
      FIDO_LOG(DEBUG) << "• " << pairing->name << " " << pairing->last_updated
                      << " priority:" << pairing->channel_priority;
    }
    FIDO_LOG(DEBUG) << "Found " << paired_phones.size() << " caBLEv2 devices";

    if (!paired_phones.empty()) {
      for (size_t i = 0; i < paired_phones.size(); i++) {
        const auto& phone = paired_phones[i];
        paired_phone_entries.emplace_back(phone->name, i,
                                          phone->peer_public_key_x962);
        phone_names_.push_back(phone->name);
        phone_public_keys_.push_back(phone->peer_public_key_x962);
      }

      contact_phone_callback = discovery_factory->get_cable_contact_callback();
    }
  }

  const bool non_extension_cablev2_enabled =
      (!cable_extension_permitted ||
       (!cable_extension_provided &&
        request_type == device::FidoRequestType::kGetAssertion) ||
       base::FeatureList::IsEnabled(device::kWebAuthCableExtensionAnywhere));

  absl::optional<std::array<uint8_t, device::cablev2::kQRKeySize>>
      qr_generator_key;
  absl::optional<std::string> qr_string;
  if (non_extension_cablev2_enabled || cablev2_extension_provided) {
    // A QR key is generated for all caBLEv2 cases but whether the QR code is
    // displayed is up to the UI.
    qr_generator_key.emplace();
    crypto::RandBytes(*qr_generator_key);
    qr_string = device::cablev2::qr::Encode(*qr_generator_key, request_type);

    auto linking_handler = std::make_unique<CableLinkingEventHandler>(
        Profile::FromBrowserContext(GetBrowserContext()));
    discovery_factory->set_cable_pairing_callback(
        base::BindRepeating(&CableLinkingEventHandler::OnNewCablePairing,
                            std::move(linking_handler)));
    discovery_factory->set_cable_invalidated_pairing_callback(
        base::BindRepeating(
            &ChromeAuthenticatorRequestDelegate::OnInvalidatedCablePairing,
            weak_ptr_factory_.GetWeakPtr()));
    if (SystemNetworkContextManager::GetInstance()) {
      discovery_factory->set_network_context(
          SystemNetworkContextManager::GetInstance()->GetContext());
    }
  }

  mojo::Remote<device::mojom::UsbDeviceManager> usb_device_manager;
  if (!pass_empty_usb_device_manager_) {
    content::GetDeviceService().BindUsbDeviceManager(
        usb_device_manager.BindNewPipeAndPassReceiver());
  }
  discovery_factory->set_android_accessory_params(
      std::move(usb_device_manager),
      l10n_util::GetStringUTF8(IDS_WEBAUTHN_CABLEV2_AOA_REQUEST_DESCRIPTION));

  if (cable_extension_accepted || non_extension_cablev2_enabled) {
    absl::optional<bool> extension_is_v2;
    if (cable_extension_provided) {
      extension_is_v2 = cablev2_extension_provided;
    }
    dialog_model_->set_cable_transport_info(
        extension_is_v2, std::move(paired_phone_entries),
        std::move(contact_phone_callback), qr_string);
    discovery_factory->set_cable_data(request_type, std::move(pairings),
                                      qr_generator_key,
                                      std::move(paired_phones));
  }
}

void ChromeAuthenticatorRequestDelegate::SelectAccount(
    std::vector<device::AuthenticatorGetAssertionResponse> responses,
    base::OnceCallback<void(device::AuthenticatorGetAssertionResponse)>
        callback) {
  if (disable_ui_) {
    // Cryptotoken requests should never reach account selection.
    DCHECK(IsVirtualEnvironmentEnabled());

    // The browser is being automated. Select the first credential to support
    // automation of discoverable credentials.
    // TODO(crbug.com/991666): Provide a way to determine which account gets
    // picked.
    std::move(callback).Run(std::move(responses.at(0)));
    return;
  }

  if (g_observer) {
    g_observer->AccountSelectorShown(responses);
    std::move(callback).Run(std::move(responses.at(0)));
    return;
  }

  dialog_model_->SelectAccount(std::move(responses), std::move(callback));
}

void ChromeAuthenticatorRequestDelegate::DisableUI() {
  disable_ui_ = true;
}

bool ChromeAuthenticatorRequestDelegate::IsWebAuthnUIEnabled() {
  // The UI is fully disabled for the entire request duration if either:
  // 1) The request originates from cryptotoken. The UI may be hidden in other
  // circumstances (e.g. while showing the native Windows WebAuthn UI). But in
  // those cases the UI is still enabled and can be shown e.g. for an
  // attestation consent prompt.
  // 2) A specialized UI is replacing the default WebAuthn UI, such as
  // Secure Payment Confirmation or Autofill.
  return !disable_ui_;
}

void ChromeAuthenticatorRequestDelegate::SetConditionalRequest(
    bool is_conditional) {
  is_conditional_ = is_conditional;
}

void ChromeAuthenticatorRequestDelegate::OnTransportAvailabilityEnumerated(
    device::FidoRequestHandlerBase::TransportAvailabilityInfo data) {
  if (g_observer) {
    g_observer->OnTransportAvailabilityEnumerated(this, &data);
  }

  if (disable_ui_ || dialog_model_->current_step() !=
                         AuthenticatorRequestDialogModel::Step::kNotStarted) {
    return;
  }

  bool last_used_native_api = false;
#if BUILDFLAG(IS_WIN)
  PrefService* const prefs =
      user_prefs::UserPrefs::Get(GetRenderFrameHost()->GetBrowserContext());
  last_used_native_api = prefs->GetBoolean(kWebAuthnLastOperationWasNativeAPI);
#endif

  dialog_model_->StartFlow(std::move(data), is_conditional_,
                           last_used_native_api);

  if (g_observer) {
    g_observer->UIShown(this);
  }
}

bool ChromeAuthenticatorRequestDelegate::EmbedderControlsAuthenticatorDispatch(
    const device::FidoAuthenticator& authenticator) {
  // Decide whether the //device/fido code should dispatch the current
  // request to an authenticator immediately after it has been
  // discovered, or whether the embedder/UI takes charge of that by
  // invoking its RequestCallback.
  if (!IsWebAuthnUIEnabled()) {
    // There is no UI to handle request dispatch.
    return false;
  }
  if (is_conditional_ &&
      (dialog_model_->current_step() ==
           AuthenticatorRequestDialogModel::Step::kConditionalMediation ||
       dialog_model_->current_step() ==
           AuthenticatorRequestDialogModel::Step::kNotStarted)) {
    // There is an active conditional request that is not showing any UI. The UI
    // will dispatch to any plugged in authenticators after the user selects an
    // option.
    return true;
  }
  auto transport = authenticator.AuthenticatorTransport();
  return !transport ||  // Windows
         *transport == device::FidoTransportProtocol::kInternal;
}

void ChromeAuthenticatorRequestDelegate::FidoAuthenticatorAdded(
    const device::FidoAuthenticator& authenticator) {
  if (!IsWebAuthnUIEnabled())
    return;

  dialog_model_->AddAuthenticator(authenticator);
}

void ChromeAuthenticatorRequestDelegate::FidoAuthenticatorRemoved(
    base::StringPiece authenticator_id) {
  if (!IsWebAuthnUIEnabled())
    return;

  dialog_model_->RemoveAuthenticator(authenticator_id);
}

void ChromeAuthenticatorRequestDelegate::BluetoothAdapterPowerChanged(
    bool is_powered_on) {
  dialog_model_->OnBluetoothPoweredStateChanged(is_powered_on);
}

bool ChromeAuthenticatorRequestDelegate::SupportsPIN() const {
  return true;
}

void ChromeAuthenticatorRequestDelegate::CollectPIN(
    CollectPINOptions options,
    base::OnceCallback<void(std::u16string)> provide_pin_cb) {
  dialog_model_->CollectPIN(options.reason, options.error,
                            options.min_pin_length, options.attempts,
                            std::move(provide_pin_cb));
}

void ChromeAuthenticatorRequestDelegate::StartBioEnrollment(
    base::OnceClosure next_callback) {
  dialog_model_->StartInlineBioEnrollment(std::move(next_callback));
}

void ChromeAuthenticatorRequestDelegate::OnSampleCollected(
    int bio_samples_remaining) {
  dialog_model_->OnSampleCollected(bio_samples_remaining);
}

void ChromeAuthenticatorRequestDelegate::FinishCollectToken() {
  dialog_model_->FinishCollectToken();
}

void ChromeAuthenticatorRequestDelegate::OnRetryUserVerification(int attempts) {
  dialog_model_->OnRetryUserVerification(attempts);
}

void ChromeAuthenticatorRequestDelegate::OnStartOver() {
  DCHECK(start_over_callback_);
  start_over_callback_.Run();
}

void ChromeAuthenticatorRequestDelegate::OnModelDestroyed(
    AuthenticatorRequestDialogModel* model) {
  DCHECK_EQ(model, dialog_model_.get());
}

void ChromeAuthenticatorRequestDelegate::OnCancelRequest() {
  // |cancel_callback_| must be invoked at most once as invocation of
  // |cancel_callback_| will destroy |this|.
  DCHECK(cancel_callback_);
  std::move(cancel_callback_).Run();
}

void ChromeAuthenticatorRequestDelegate::OnManageDevicesClicked() {
  content::WebContents* web_contents =
      content::WebContents::FromRenderFrameHost(GetRenderFrameHost());
  Browser* browser = chrome::FindBrowserWithWebContents(web_contents);
  if (browser) {
    NavigateParams params(browser,
                          GURL("chrome://settings/securityKeys/phones"),
                          ui::PageTransition::PAGE_TRANSITION_AUTO_TOPLEVEL);
    params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
    Navigate(&params);
  }
}

raw_ptr<AuthenticatorRequestDialogModel>
ChromeAuthenticatorRequestDelegate::GetDialogModelForTesting() {
  return dialog_model_.get();
}

void ChromeAuthenticatorRequestDelegate::SetPassEmptyUsbDeviceManagerForTesting(
    bool value) {
  pass_empty_usb_device_manager_ = value;
}

content::RenderFrameHost*
ChromeAuthenticatorRequestDelegate::GetRenderFrameHost() const {
  content::RenderFrameHost* ret =
      content::RenderFrameHost::FromID(render_frame_host_id_);
  DCHECK(ret);
  return ret;
}

content::BrowserContext* ChromeAuthenticatorRequestDelegate::GetBrowserContext()
    const {
  return GetRenderFrameHost()->GetBrowserContext();
}

bool ChromeAuthenticatorRequestDelegate::ShouldPermitCableExtension(
    const url::Origin& origin) {
  if (base::FeatureList::IsEnabled(device::kWebAuthCableExtensionAnywhere)) {
    return true;
  }

  // Because the future of the caBLE extension might be that we transition
  // everything to QR-code or sync-based pairing, we don't want use of the
  // extension to spread without consideration. Therefore it's limited to
  // origins that are already depending on it and test sites.
  if (origin.DomainIs("google.com")) {
    return true;
  }

  const GURL test_site("https://webauthndemo.appspot.com");
  DCHECK(test_site.is_valid());
  return origin.IsSameOriginWith(test_site);
}

void ChromeAuthenticatorRequestDelegate::OnInvalidatedCablePairing(
    size_t failed_contact_index) {
  PrefService* const prefs =
      Profile::FromBrowserContext(GetBrowserContext())->GetPrefs();

  // A pairing was reported to be invalid. Delete it unless it came from Sync,
  // in which case there's nothing to be done.
  cablev2::DeletePairingByPublicKey(
      prefs, phone_public_keys_.at(failed_contact_index));

  // Contact the next phone with the same name, if any, given that no
  // notification has been sent.
  dialog_model_->OnPhoneContactFailed(phone_names_.at(failed_contact_index));
}
