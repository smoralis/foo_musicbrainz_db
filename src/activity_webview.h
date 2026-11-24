
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

    // Sets the colour scheme pages are told to prefer (prefers-color-scheme) and the colour
    // shown before a page paints. Safe to call often; only changes are applied.
    void set_dark(bool dark);

    // Shows an empty page (about:blank).
    void navigate_blank();

    // Turns ad, tracker and unsolicited pop-up blocking on or off. Safe to call often.
    void set_block_ads(bool block);

    // Shows or hides the browser.
    void show(bool visible);

    // Navigates to a validated HTTP or HTTPS URL.
    void navigate(const std::wstring& url);

    // Closes the controller and releases WebView2 resources.
    void close();

    bool is_ready() const;

    // True once create() has been called (and close() has not).
    bool exists() const;

public:
    struct state;

private:
    std::shared_ptr<state> m_state;
};
