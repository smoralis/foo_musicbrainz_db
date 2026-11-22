
#include "stdafx.h"
#include "activity_webview.h"

#include <WebView2.h>
#include <wrl.h>
#include <wrl/event.h>

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
    bool shutting_down = false;

    std::wstring pending_url;
};

namespace {

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

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        nullptr,
        nullptr,
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

bool activity_webview::is_ready() const {
    return m_state &&
           !m_state->shutting_down &&
           m_state->webview != nullptr;
}
