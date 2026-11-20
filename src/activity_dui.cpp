
#include "stdafx.h"
#include "ui_element.h"
#include "activity_window.h"

namespace
{

    // Unique GUID identifying this UI element.
    const GUID guid_dui_activity =
    {
        0x9d8dc7e3, 0x705c, 0x415a,
        { 0x8b, 0xf4, 0x31, 0x45, 0xcd, 0xac, 0x33, 0x18 }
    };

    // The standalone "now playing" element.
    const GUID guid_dui_nowplaying =
        { 0xf332ea91, 0xd32f, 0x4ada, { 0x82, 0x82, 0xeb, 0x64, 0x19, 0xb1, 0x5c, 0x3a } };


    // ============================================================
    // UI element instance
    // ============================================================

    class dui_activity_instance
        : public ui_element_instance
        , public activity_host
    {
    public:
        dui_activity_instance(
            ui_element_config::ptr config,
            ui_element_instance_callback_ptr callback,
            const GUID& guid,
            bool now_playing_only
        )
            : m_callback(callback)
            , m_config(config)
            , m_guid(guid)
            , m_np_only(now_playing_only)
        {
        }

        ~dui_activity_instance() override
        {
            if (m_window.IsWindow())
            {
                m_window.DestroyWindow();
            }
        }

        // Create the child window.
        HWND create(HWND parent)
        {
            m_window.set_host(this);

            if (m_np_only)
            {
                m_window.show_now_playing_only();
            }

            return m_window.Create(
                parent,
                CWindow::rcDefault,
                nullptr,
                WS_CHILD |
                WS_VISIBLE |
                WS_CLIPCHILDREN |
                WS_CLIPSIBLINGS
            );
        }

        // --------------------------------------------------------
        // ui_element_instance
        // --------------------------------------------------------

        fb2k::hwnd_t get_wnd() override
        {
            return m_window;
        }

        void set_configuration(ui_element_config::ptr config) override
        {
            m_config = config;
        }

        ui_element_config::ptr get_configuration() override
        {
            return m_config;
        }

        GUID get_guid() override
        {
            return m_guid;
        }

        GUID get_subclass() override
        {
            return ui_element_subclass_utility;
        }

        void notify(
            const GUID& what,
            t_size,
            const void*,
            t_size
        ) override
        {
            if (what == ui_element_notify_edit_mode_changed)
            {
                if (m_window.IsWindow())
                {
                    m_window.on_edit_mode_changed();
                }
            }

            if (what == ui_element_notify_colors_changed ||
                what == ui_element_notify_font_changed)
            {
                if (m_window.IsWindow())
                {
                    m_window.refresh_theme();
                }
            }
        }

        // --------------------------------------------------------
        // activity_host
        // --------------------------------------------------------

        bool query_colours(
            COLORREF& background,
            COLORREF& text
        ) override
        {
            if (m_callback.is_empty())
            {
                return false;
            }

            t_ui_color color;

            if (m_callback->query_color(ui_color_background, color))
            {
                background = static_cast<COLORREF>(color);
            }

            if (m_callback->query_color(ui_color_text, color))
            {
                text = static_cast<COLORREF>(color);
            }

            return true;
        }

        bool is_edit_mode() override
        {
            return m_callback.is_valid() && m_callback->is_edit_mode_enabled();
        }

        HFONT query_font() override
        {
            if (m_callback.is_empty())
            {
                return nullptr;
            }

            return m_callback->query_font_ex(ui_font_default);
        }

    private:
        activity_window m_window;

        ui_element_instance_callback_ptr m_callback;
        ui_element_config::ptr m_config;
        GUID m_guid;
        bool m_np_only;
    };


    // ============================================================
    // UI element factory / entrypoint
    // ============================================================

    // One implementation for both elements: they differ only in GUID, name and
    // whether the window shows just the "Now playing" view.
    class dui_element_base : public ui_element
    {
    public:
        dui_element_base(
            const GUID& guid,
            const char* name,
            const char* description,
            bool now_playing_only
        )
            : m_guid(guid)
            , m_name(name)
            , m_description(description)
            , m_np_only(now_playing_only)
        {
        }

        GUID get_guid() override
        {
            return m_guid;
        }

        GUID get_subclass() override
        {
            return ui_element_subclass_utility;
        }

        void get_name(pfc::string_base& out) override
        {
            out = m_name;
        }

        bool get_description(pfc::string_base& out) override
        {
            out = m_description;
            return true;
        }

        ui_element_instance_ptr instantiate(
            fb2k::hwnd_t parent,
            ui_element_config::ptr config,
            ui_element_instance_callback_ptr callback
        ) override
        {
            // Service implementations must be created through the
            // foobar2000 service implementation wrapper.
            service_ptr_t<dui_activity_instance> instance =
                new service_impl_t<dui_activity_instance>(
                    config,
                    callback,
                    m_guid,
                    m_np_only
                );

            // The UI element window must exist before instantiate()
            // returns.
            instance->create(parent);

            return instance;
        }

        ui_element_config::ptr get_default_configuration() override
        {
            return ui_element_config::g_create_empty(m_guid);
        }

        ui_element_children_enumerator_ptr enumerate_children(
            ui_element_config::ptr
        ) override
        {
            // This is not a container UI element.
            return nullptr;
        }

    private:
        GUID m_guid;
        const char* m_name;
        const char* m_description;
        bool m_np_only;
    };

    class dui_activity_element : public dui_element_base
    {
    public:
        dui_activity_element()
            : dui_element_base(
                guid_dui_activity,
                "MusicBrainz DB activity",
                "Shows MusicBrainz lookup events and database statistics.",
                false)
        {
        }
    };

    class dui_nowplaying_element : public dui_element_base
    {
    public:
        dui_nowplaying_element()
            : dui_element_base(
                guid_dui_nowplaying,
                "MusicBrainz DB now playing",
                "Shows the MusicBrainz information of the playing track's artist.",
                true)
        {
        }
    };


    // Register the UI elements with foobar2000.
    static service_factory_single_t<dui_activity_element>
        g_dui_activity_factory;

    static service_factory_single_t<dui_nowplaying_element>
        g_dui_nowplaying_factory;

} // anonymous namespace
