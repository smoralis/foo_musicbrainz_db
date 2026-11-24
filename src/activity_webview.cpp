
#include "stdafx.h"
#include "activity_webview.h"

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>
#include <wrl/event.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include "preferences.h"
#include <utility>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

struct activity_webview::state {
    HWND parent = nullptr;

    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;

    RECT bounds{};

    bool visible = false;
    bool dark = false;
    bool block_ads = false;
    bool filter_added = false;
    std::unordered_set<std::wstring> blocklist;
    EventRegistrationToken request_token{};
    EventRegistrationToken window_token{};
    bool shutting_down = false;

    std::wstring pending_url;
};

namespace {

// WebView2 keeps its profile (cache, cookies, ...) in a "user data folder". When none is
// given it creates "<exe name>.WebView2" next to foobar2000.exe, so use the component's
// folder in the foobar2000 profile instead: <profile>\foo_musicbrainz_db\WebView2.
// Returns an empty string when the folder cannot be prepared (WebView2 then uses its default).
std::wstring user_data_folder() {
    try {
        pfc::string8 native_profile;
        foobar2000_io::extract_native_path(core_api::get_profile_path(), native_profile);
        const std::filesystem::path dir =
            std::filesystem::u8path(native_profile.c_str()) / "foo_musicbrainz_db" / "WebView2";
        std::filesystem::create_directories(dir);
        return dir.wstring();
    } catch (...) {
        return std::wstring();
    }
}

// Tells pages which colour scheme to prefer and paints the area behind them to match, so
// there is no light flash before a page loads. Needs a WebView2 runtime that has profiles.
void apply_scheme(const std::shared_ptr<activity_webview::state>& s) {
    if (!s || s->shutting_down || !s->webview)
        return;

    ComPtr<ICoreWebView2_13> webview13;
    if (SUCCEEDED(s->webview.As(&webview13)) && webview13) {
        ComPtr<ICoreWebView2Profile> profile;
        if (SUCCEEDED(webview13->get_Profile(&profile)) && profile) {
            profile->put_PreferredColorScheme(s->dark
                ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK
                : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT);
        }
    }

    ComPtr<ICoreWebView2Controller2> controller2;
    if (s->controller && SUCCEEDED(s->controller.As(&controller2)) && controller2) {
        const COREWEBVIEW2_COLOR background = s->dark
            ? COREWEBVIEW2_COLOR{ 255, 32, 32, 32 }
            : COREWEBVIEW2_COLOR{ 255, 255, 255, 255 };
        controller2->put_DefaultBackgroundColor(background);
    }
}

// Well-known advertising and tracking hosts. A request is blocked when its host is one of
// these or a subdomain of one. More hosts can be added in
// <profile>\foo_musicbrainz_db\adblock.txt (one domain per line, # starts a comment).
const wchar_t* const k_builtin_ad_hosts[] = {
    L"doubleclick.net", L"googlesyndication.com", L"googleadservices.com", L"googletagservices.com",
    L"googletagmanager.com", L"google-analytics.com", L"adservice.google.com", L"2mdn.net",
    L"adnxs.com", L"adsrvr.org", L"advertising.com", L"adform.net", L"adition.com",
    L"amazon-adsystem.com", L"taboola.com", L"outbrain.com", L"criteo.com", L"criteo.net",
    L"rubiconproject.com", L"pubmatic.com", L"openx.net", L"casalemedia.com", L"indexww.com",
    L"smartadserver.com", L"bidswitch.net", L"33across.com", L"sharethrough.com", L"teads.tv",
    L"yieldmo.com", L"media.net", L"revcontent.com", L"mgid.com", L"zedo.com", L"adroll.com",
    L"scorecardresearch.com", L"quantserve.com", L"quantcount.com", L"moatads.com",
    L"hotjar.com", L"ads-twitter.com", L"ads.linkedin.com", L"adsafeprotected.com",
    L"serving-sys.com", L"contextweb.com", L"lijit.com", L"sovrn.com", L"gumgum.com"
};

std::wstring user_blocklist_path() {
    try {
        pfc::string8 native_profile;
        foobar2000_io::extract_native_path(core_api::get_profile_path(), native_profile);
        return (std::filesystem::u8path(native_profile.c_str()) / "foo_musicbrainz_db" / "adblock.txt").wstring();
    } catch (...) {
        return std::wstring();
    }
}

std::unordered_set<std::wstring> load_blocklist() {
    std::unordered_set<std::wstring> hosts;
    for (const wchar_t* h : k_builtin_ad_hosts) hosts.insert(h);
    const std::wstring path = user_blocklist_path();
    if (path.empty()) return hosts;
    std::ifstream file{ std::filesystem::path(path) };
    std::string line;
    while (std::getline(file, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::string host;
        for (unsigned char c : line) {
            if (std::isspace(c)) continue;
            host.push_back(static_cast<char>(std::tolower(c)));
        }
        while (!host.empty() && host.front() == '.') host.erase(host.begin());
        if (!host.empty()) hosts.insert(std::wstring(host.begin(), host.end()));
    }
    return hosts;
}

// Host part of a URL, lower case ("https://user@Ads.Example.com:8080/x" -> "ads.example.com").
std::wstring host_of(const std::wstring& url) {
    const auto scheme = url.find(L"://");
    if (scheme == std::wstring::npos) return std::wstring();
    size_t begin = scheme + 3;
    size_t end = url.find_first_of(L"/?#", begin);
    std::wstring authority = url.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
    const auto at = authority.rfind(L'@');
    if (at != std::wstring::npos) authority.erase(0, at + 1);
    const auto colon = authority.find(L':');
    if (colon != std::wstring::npos) authority.erase(colon);
    for (auto& c : authority) c = static_cast<wchar_t>(std::towlower(c));
    return authority;
}

// True when `host` or one of its parent domains is in the list.
bool host_blocked(const std::unordered_set<std::wstring>& list, std::wstring host) {
    while (!host.empty()) {
        if (list.count(host)) return true;
        const auto dot = host.find(L'.');
        if (dot == std::wstring::npos) break;
        host.erase(0, dot + 1);
    }
    return false;
}

// Starts or stops intercepting requests, to match s->block_ads.
void apply_block(const std::shared_ptr<activity_webview::state>& s) {
    if (!s || s->shutting_down || !s->webview) return;
    if (s->block_ads && !s->filter_added) {
        s->blocklist = load_blocklist(); // re-read each time the option is switched on
        if (SUCCEEDED(s->webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL)))
            s->filter_added = true;
    } else if (!s->block_ads && s->filter_added) {
        s->webview->RemoveWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        s->filter_added = false;
    }
}

bool is_http_url(const std::wstring& url) {
    return url.compare(0, 7, L"http://") == 0 ||
           url.compare(0, 8, L"https://") == 0;
}

void apply_bounds(
    const std::shared_ptr<activity_webview::state>& s
) {
    if (!s || !s->controller || s->shutting_down)
        return;

    s->controller->put_Bounds(s->bounds);
    s->controller->put_IsVisible(s->visible ? TRUE : FALSE);
}

} // namespace

activity_webview::~activity_webview() {
    close();
}

bool activity_webview::create(HWND parent) {
    if (!parent || !::IsWindow(parent))
        return false;

    if (m_state)
        return true;

    auto s = std::make_shared<state>();
    s->parent = parent;
    s->visible = false;

    m_state = s;

    const std::weak_ptr<state> weak = s;

    const std::wstring data_folder = user_data_folder();

    // "Force dark mode": let Chromium darken pages that have no dark theme of their own.
    // This is a browser start-up argument, so it applies after foobar2000 is restarted.
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (mbdb_prefs::force_dark_browser())
        options->put_AdditionalBrowserArguments(L"--enable-features=WebContentsForceDark");

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        data_folder.empty() ? nullptr : data_folder.c_str(),
        options.Get(),
        Callback<
            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
        >(
            [weak](HRESULT result,
                   ICoreWebView2Environment* environment) -> HRESULT {

                auto s = weak.lock();

                if (!s || s->shutting_down)
                    return S_OK;

                if (FAILED(result) || !environment)
                    return S_OK;

                s->environment = environment;

                const std::weak_ptr<state> weak_controller = s;

                HRESULT controller_hr =
                    environment->CreateCoreWebView2Controller(
                        s->parent,
                        Callback<
                            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
                        >(
                            [weak_controller](
                                HRESULT controller_result,
                                ICoreWebView2Controller* controller
                            ) -> HRESULT {

                                auto s = weak_controller.lock();

                                if (!s || s->shutting_down)
                                    return S_OK;

                                if (FAILED(controller_result) ||
                                    !controller) {
                                    return S_OK;
                                }

                                s->controller = controller;

                                HRESULT webview_hr =
                                    controller->get_CoreWebView2(
                                        &s->webview
                                    );

                                if (FAILED(webview_hr) || !s->webview) {
                                    s->controller->Close();
                                    s->controller.Reset();
                                    return S_OK;
                                }

                                apply_bounds(s);
                                apply_scheme(s);

                                // Ad blocking: requests to blocked hosts get an empty 403 answer, and
                                // windows a page opens without a user action are suppressed. Both
                                // handlers do nothing unless blocking is switched on (apply_block).
                                const std::weak_ptr<state> weak_blocker = s;
                                s->webview->add_WebResourceRequested(
                                    Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                        [weak_blocker](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                            auto s = weak_blocker.lock();
                                            if (!s || !s->block_ads || !s->environment) return S_OK;
                                            ComPtr<ICoreWebView2WebResourceRequest> request;
                                            if (FAILED(args->get_Request(&request)) || !request) return S_OK;
                                            LPWSTR uri = nullptr;
                                            if (FAILED(request->get_Uri(&uri)) || !uri) return S_OK;
                                            const std::wstring host = host_of(uri);
                                            CoTaskMemFree(uri);
                                            if (host_blocked(s->blocklist, host)) {
                                                ComPtr<ICoreWebView2WebResourceResponse> response;
                                                if (SUCCEEDED(s->environment->CreateWebResourceResponse(
                                                        nullptr, 403, L"Blocked", L"", &response)) && response)
                                                    args->put_Response(response.Get());
                                            }
                                            return S_OK;
                                        }).Get(),
                                    &s->request_token);
                                s->webview->add_NewWindowRequested(
                                    Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                        [weak_blocker](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                            auto s = weak_blocker.lock();
                                            if (!s || !s->block_ads) return S_OK;
                                            BOOL user_initiated = FALSE;
                                            args->get_IsUserInitiated(&user_initiated);
                                            if (!user_initiated) args->put_Handled(TRUE);
                                            return S_OK;
                                        }).Get(),
                                    &s->window_token);
                                apply_block(s);

                                // Complete a navigation requested while
                                // WebView2 was still initializing.
                                if (!s->pending_url.empty()) {
                                    s->webview->Navigate(
                                        s->pending_url.c_str()
                                    );
                                    s->pending_url.clear();
                                }

                                return S_OK;
                            }
                        ).Get()
                    );

                if (FAILED(controller_hr))
                    return S_OK;

                return S_OK;
            }
        ).Get()
    );

    if (FAILED(hr)) {
        m_state.reset();
        return false;
    }

    return true;
}

void activity_webview::set_bounds(const RECT& bounds) {
    if (!m_state)
        return;

    m_state->bounds = bounds;
    apply_bounds(m_state);
}

void activity_webview::set_dark(bool dark) {
    if (!m_state || m_state->dark == dark)
        return;

    m_state->dark = dark;
    apply_scheme(m_state);
}

void activity_webview::set_block_ads(bool block) {
    if (!m_state || m_state->block_ads == block)
        return;

    m_state->block_ads = block;
    apply_block(m_state); // before the browser exists this is a no-op; creation applies it
}

void activity_webview::show(bool visible) {
    if (!m_state)
        return;

    m_state->visible = visible;
    apply_bounds(m_state);
}

void activity_webview::navigate(const std::wstring& url) {
    if (!m_state || m_state->shutting_down)
        return;

    if (!is_http_url(url))
        return;

    if (m_state->webview) {
        m_state->webview->Navigate(url.c_str());
    }
    else {
        m_state->pending_url = url;
    }
}

void activity_webview::navigate_blank() {
    if (!m_state || m_state->shutting_down)
        return;

    if (m_state->webview) {
        m_state->webview->Navigate(L"about:blank");
    }
    else {
        m_state->pending_url = L"about:blank";
    }
}

void activity_webview::close() {
    if (!m_state)
        return;

    auto s = std::move(m_state);
    s->shutting_down = true;
    s->visible = false;

    if (s->controller) {
        s->controller->put_IsVisible(FALSE);
        s->controller->Close();
    }

    s->webview.Reset();
    s->controller.Reset();
    s->environment.Reset();
}

bool activity_webview::exists() const {
    return m_state != nullptr;
}

bool activity_webview::is_ready() const {
    return m_state &&
           !m_state->shutting_down &&
           m_state->webview != nullptr;
}
