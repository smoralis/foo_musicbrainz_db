
#pragma once

#include <windows.h>
#include <memory>
#include <string>

class activity_webview {
public:
    activity_webview() = default;
    ~activity_webview();

    activity_webview(const activity_webview&) = delete;
    activity_webview& operator=(const activity_webview&) = delete;

    // Starts asynchronous WebView2 initialization.
    bool create(HWND parent);

    // Updates the browser's position and size in parent-client coordinates.
    void set_bounds(const RECT& bounds);

    // Shows or hides the browser.
    void show(bool visible);

    // Navigates to a validated HTTP or HTTPS URL.
    void navigate(const std::wstring& url);

    // Closes the controller and releases WebView2 resources.
    void close();

    bool is_ready() const;

public:
    struct state;

private:
    std::shared_ptr<state> m_state;
};
