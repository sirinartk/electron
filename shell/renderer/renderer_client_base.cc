// Copyright (c) 2017 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/renderer/renderer_client_base.h"

#include <memory>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "content/common/buildflags.h"
#include "content/public/common/content_constants.h"
#include "content/public/common/content_switches.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_view.h"
#include "electron/buildflags/buildflags.h"
#include "native_mate/dictionary.h"
#include "printing/buildflags/buildflags.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "shell/common/color_util.h"
#include "shell/common/native_mate_converters/value_converter.h"
#include "shell/common/options_switches.h"
#include "shell/renderer/atom_autofill_agent.h"
#include "shell/renderer/atom_render_frame_observer.h"
#include "shell/renderer/content_settings_observer.h"
#include "shell/renderer/electron_api_service_impl.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/blink/public/web/web_custom_element.h"  // NOLINT(build/include_alpha)
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_plugin_params.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "third_party/blink/public/web/web_security_policy.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/renderer/platform/weborigin/scheme_registry.h"  // nogncheck

#if defined(OS_MACOSX)
#include "base/strings/sys_string_conversions.h"
#endif

#if defined(OS_WIN)
#include <shlobj.h>
#endif

#if BUILDFLAG(ENABLE_PDF_VIEWER)
#include "shell/common/atom_constants.h"
#endif  // BUILDFLAG(ENABLE_PDF_VIEWER)

#if BUILDFLAG(ENABLE_PEPPER_FLASH)
#include "chrome/renderer/pepper/pepper_helper.h"
#endif  // BUILDFLAG(ENABLE_PEPPER_FLASH)

#if BUILDFLAG(ENABLE_TTS)
#include "chrome/renderer/tts_dispatcher.h"
#endif  // BUILDFLAG(ENABLE_TTS)

#if BUILDFLAG(ENABLE_PRINTING)
#include "components/printing/renderer/print_render_frame_helper.h"
#include "printing/print_settings.h"
#include "shell/renderer/printing/print_render_frame_helper_delegate.h"
#endif  // BUILDFLAG(ENABLE_PRINTING)

namespace electron {

namespace {

std::vector<std::string> ParseSchemesCLISwitch(base::CommandLine* command_line,
                                               const char* switch_name) {
  std::string custom_schemes = command_line->GetSwitchValueASCII(switch_name);
  return base::SplitString(custom_schemes, ",", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY);
}

void SetHiddenValue(v8::Handle<v8::Context> context,
                    const base::StringPiece& key,
                    v8::Local<v8::Value> value) {
  v8::Isolate* isolate = context->GetIsolate();
  v8::Local<v8::Private> privateKey =
      v8::Private::ForApi(isolate, mate::StringToV8(isolate, key));
  context->Global()->SetPrivate(context, privateKey, value);
}

v8::Local<v8::Value> ConvertOptionalToV8(v8::Isolate* isolate, int value) {
  if (value) {
    return mate::ConvertToV8(isolate, value);
  } else {
    return v8::Null(isolate);
  }
}

}  // namespace

RendererClientBase::RendererClientBase() {
  auto* command_line = base::CommandLine::ForCurrentProcess();
  // Parse --standard-schemes=scheme1,scheme2
  std::vector<std::string> standard_schemes_list =
      ParseSchemesCLISwitch(command_line, switches::kStandardSchemes);
  for (const std::string& scheme : standard_schemes_list)
    url::AddStandardScheme(scheme.c_str(), url::SCHEME_WITH_HOST);

  // We rely on the unique process host id which is notified to the
  // renderer process via command line switch from the content layer,
  // if this switch is removed from the content layer for some reason,
  // we should define our own.
  DCHECK(command_line->HasSwitch(::switches::kRendererClientId));
  renderer_client_id_ =
      command_line->GetSwitchValueASCII(::switches::kRendererClientId);
}

RendererClientBase::~RendererClientBase() {}

void RendererClientBase::DidCreateScriptContext(
    v8::Handle<v8::Context> context,
    content::RenderFrame* render_frame) {
  // global.setHidden("contextId", `${processHostId}-${++next_context_id_}`)
  auto context_id = base::StringPrintf(
      "%s-%" PRId64, renderer_client_id_.c_str(), ++next_context_id_);
  v8::Isolate* isolate = context->GetIsolate();
  SetHiddenValue(context, "contextId", mate::ConvertToV8(isolate, context_id));

  auto dict = mate::Dictionary::CreateEmpty(isolate);
  dict.Set("preloadScripts", web_preferences_.preload_paths);
  dict.Set(options::kContextIsolation, web_preferences_.context_isolation);
  dict.Set(options::kEnableRemoteModule, web_preferences_.enable_remote_module);
  dict.Set(options::kNodeIntegration, web_preferences_.node_integration);
  dict.Set(options::kNativeWindowOpen, web_preferences_.native_window_open);
  dict.Set(options::kWebviewTag, web_preferences_.webview_tag);
  dict.Set("isHiddenPage",
           mate::ConvertToV8(isolate, web_preferences_.is_hidden_page));
  dict.Set(options::kGuestInstanceID,
           ConvertOptionalToV8(isolate, web_preferences_.guest_instance_id));
  dict.Set(options::kOpenerID,
           ConvertOptionalToV8(isolate, web_preferences_.opener_id));

  SetHiddenValue(context, "webPreferences", dict.GetHandle());
}

void RendererClientBase::AddRenderBindings(
    v8::Isolate* isolate,
    v8::Local<v8::Object> binding_object) {
  mate::Dictionary dict(isolate, binding_object);
}

void RendererClientBase::RenderThreadStarted() {
  auto* command_line = base::CommandLine::ForCurrentProcess();

#if BUILDFLAG(USE_EXTERNAL_POPUP_MENU)
  // On macOS, popup menus are rendered by the main process by default.
  // This causes problems in OSR, since when the popup is rendered separately,
  // it won't be captured in the rendered image.
  if (command_line->HasSwitch(options::kOffscreen)) {
    blink::WebView::SetUseExternalPopupMenus(false);
  }
#endif

  blink::WebCustomElement::AddEmbedderCustomElementName("webview");
  blink::WebCustomElement::AddEmbedderCustomElementName("browserplugin");

  WTF::String extension_scheme("chrome-extension");
  // Extension resources are HTTP-like and safe to expose to the fetch API. The
  // rules for the fetch API are consistent with XHR.
  blink::SchemeRegistry::RegisterURLSchemeAsSupportingFetchAPI(
      extension_scheme);
  // Extension resources, when loaded as the top-level document, should bypass
  // Blink's strict first-party origin checks.
  blink::SchemeRegistry::RegisterURLSchemeAsFirstPartyWhenTopLevel(
      extension_scheme);
  // In Chrome we should set extension's origins to match the pages they can
  // work on, but in Electron currently we just let extensions do anything.
  blink::SchemeRegistry::RegisterURLSchemeAsSecure(extension_scheme);
  blink::SchemeRegistry::RegisterURLSchemeAsBypassingContentSecurityPolicy(
      extension_scheme);

  // Parse --secure-schemes=scheme1,scheme2
  std::vector<std::string> secure_schemes_list =
      ParseSchemesCLISwitch(command_line, switches::kSecureSchemes);
  for (const std::string& scheme : secure_schemes_list)
    blink::SchemeRegistry::RegisterURLSchemeAsSecure(
        WTF::String::FromUTF8(scheme.data(), scheme.length()));

  std::vector<std::string> fetch_enabled_schemes =
      ParseSchemesCLISwitch(command_line, switches::kFetchSchemes);
  for (const std::string& scheme : fetch_enabled_schemes) {
    blink::WebSecurityPolicy::RegisterURLSchemeAsSupportingFetchAPI(
        blink::WebString::FromASCII(scheme));
  }

  std::vector<std::string> service_worker_schemes =
      ParseSchemesCLISwitch(command_line, switches::kServiceWorkerSchemes);
  for (const std::string& scheme : service_worker_schemes)
    blink::WebSecurityPolicy::RegisterURLSchemeAsAllowingServiceWorkers(
        blink::WebString::FromASCII(scheme));

  std::vector<std::string> csp_bypassing_schemes =
      ParseSchemesCLISwitch(command_line, switches::kBypassCSPSchemes);
  for (const std::string& scheme : csp_bypassing_schemes)
    blink::SchemeRegistry::RegisterURLSchemeAsBypassingContentSecurityPolicy(
        WTF::String::FromUTF8(scheme.data(), scheme.length()));

  // Allow file scheme to handle service worker by default.
  // FIXME(zcbenz): Can this be moved elsewhere?
  blink::WebSecurityPolicy::RegisterURLSchemeAsAllowingServiceWorkers("file");
  blink::SchemeRegistry::RegisterURLSchemeAsSupportingFetchAPI("file");

#if defined(OS_WIN)
  // Set ApplicationUserModelID in renderer process.
  base::string16 app_id =
      command_line->GetSwitchValueNative(switches::kAppUserModelId);
  if (!app_id.empty()) {
    SetCurrentProcessExplicitAppUserModelID(app_id.c_str());
  }
#endif
}

void RendererClientBase::RenderFrameCreated(
    content::RenderFrame* render_frame) {
#if defined(TOOLKIT_VIEWS)
  new AutofillAgent(render_frame,
                    render_frame->GetAssociatedInterfaceRegistry());
#endif
#if BUILDFLAG(ENABLE_PEPPER_FLASH)
  new PepperHelper(render_frame);
#endif
  new ContentSettingsObserver(render_frame);
#if BUILDFLAG(ENABLE_PRINTING)
  new printing::PrintRenderFrameHelper(
      render_frame,
      std::make_unique<electron::PrintRenderFrameHelperDelegate>());
#endif

  // TODO(nornagon): it might be possible for an IPC message sent to this
  // service to trigger v8 context creation before the page has begun loading.
  // However, it's unclear whether such a timing is possible to trigger, and we
  // don't have any test to confirm it. Add a test that confirms that a
  // main->renderer IPC can't cause the preload script to be executed twice. If
  // it is possible to trigger the preload script before the document is ready
  // through this interface, we should delay adding it to the registry until
  // the document is ready.
  render_frame->GetAssociatedInterfaceRegistry()->AddInterface(
      base::BindRepeating(&ElectronApiServiceImpl::CreateMojoService,
                          render_frame, this));

  mojom::ElectronBrowserPtr browser_ptr;
  render_frame->GetRemoteInterfaces()->GetInterface(
      mojo::MakeRequest(&browser_ptr));

  mojom::WebPreferencesPtr web_preferences;
  if (browser_ptr->DoGetWebPreferences(&web_preferences)) {
    web_preferences_ = web_preferences->To<mojom::WebPreferences>();
  }

#if BUILDFLAG(ENABLE_PDF_VIEWER)
  // Allow access to file scheme from pdf viewer.
  blink::WebSecurityPolicy::AddOriginAccessWhitelistEntry(
      GURL(kPdfViewerUIOrigin), "file", "", true);
#endif  // BUILDFLAG(ENABLE_PDF_VIEWER)

  content::RenderView* render_view = render_frame->GetRenderView();
  if (render_frame->IsMainFrame() && render_view) {
    if (blink::WebView* webview = render_view->GetWebView()) {
      if (web_preferences_.guest_instance_id) {  // webview.
        webview->SetBaseBackgroundColor(SK_ColorTRANSPARENT);
      } else {  // normal window.
        std::string name = web_preferences_.background_color;
        SkColor color =
            name.empty() ? SK_ColorTRANSPARENT : ParseHexColor(name);
        webview->SetBaseBackgroundColor(color);
      }
    }
  }
}

void RendererClientBase::DidClearWindowObject(
    content::RenderFrame* render_frame) {
  // Make sure every page will get a script context created.
  render_frame->GetWebFrame()->ExecuteScript(blink::WebScriptSource("void 0"));
}

std::unique_ptr<blink::WebSpeechSynthesizer>
RendererClientBase::OverrideSpeechSynthesizer(
    blink::WebSpeechSynthesizerClient* client) {
#if BUILDFLAG(ENABLE_TTS)
  return std::make_unique<TtsDispatcher>(client);
#else
  return nullptr;
#endif
}

bool RendererClientBase::OverrideCreatePlugin(
    content::RenderFrame* render_frame,
    const blink::WebPluginParams& params,
    blink::WebPlugin** plugin) {
  if (params.mime_type.Utf8() == content::kBrowserPluginMimeType ||
#if BUILDFLAG(ENABLE_PDF_VIEWER)
      params.mime_type.Utf8() == kPdfPluginMimeType ||
#endif  // BUILDFLAG(ENABLE_PDF_VIEWER)
      web_preferences_.enable_plugins)
    return false;

  *plugin = nullptr;
  return true;
}

void RendererClientBase::AddSupportedKeySystems(
    std::vector<std::unique_ptr<::media::KeySystemProperties>>* key_systems) {
#if defined(WIDEVINE_CDM_AVAILABLE)
  key_systems_provider_.AddSupportedKeySystems(key_systems);
#endif
}

bool RendererClientBase::IsKeySystemsUpdateNeeded() {
#if defined(WIDEVINE_CDM_AVAILABLE)
  return key_systems_provider_.IsKeySystemsUpdateNeeded();
#else
  return false;
#endif
}

void RendererClientBase::DidSetUserAgent(const std::string& user_agent) {
#if BUILDFLAG(ENABLE_PRINTING)
  printing::SetAgent(user_agent);
#endif
}

v8::Local<v8::Context> RendererClientBase::GetContext(
    blink::WebLocalFrame* frame,
    v8::Isolate* isolate) const {
  if (web_preferences_.context_isolation)
    return frame->WorldScriptContext(isolate, World::ISOLATED_WORLD);
  else
    return frame->MainWorldScriptContext();
}

v8::Local<v8::Value> RendererClientBase::RunScript(
    v8::Local<v8::Context> context,
    v8::Local<v8::String> source) {
  auto maybe_script = v8::Script::Compile(context, source);
  v8::Local<v8::Script> script;
  if (!maybe_script.ToLocal(&script))
    return v8::Local<v8::Value>();
  return script->Run(context).ToLocalChecked();
}

}  // namespace electron
