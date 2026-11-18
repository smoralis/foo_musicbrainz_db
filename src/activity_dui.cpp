
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
            ui_element_instance_callback_ptr callback
        )
            : m_callback(callback)
            , m_config(config)
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
            return guid_dui_activity;
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
    };


    // ============================================================
    // UI element factory / entrypoint
    // ============================================================

    class dui_activity_element : public ui_element
    {
    public:

        GUID get_guid() override
        {
            return guid_dui_activity;
        }

        GUID get_subclass() override
        {
            return ui_element_subclass_utility;
        }

        void get_name(pfc::string_base& out) override
        {
            out = "MusicBrainz DB activity";
        }

        bool get_description(pfc::string_base& out) override
        {
            out = "Shows MusicBrainz lookup events and database statistics.";
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
                    callback
                );

            // The UI element window must exist before instantiate()
            // returns.
            instance->create(parent);

            return instance;
        }

        ui_element_config::ptr get_default_configuration() override
        {
            return ui_element_config::g_create_empty(guid_dui_activity);
        }

        ui_element_children_enumerator_ptr enumerate_children(
            ui_element_config::ptr
        ) override
        {
            // This is not a container UI element.
            return nullptr;
        }
    };


    // Register the UI element with foobar2000.
    static service_factory_single_t<dui_activity_element>
        g_dui_activity_factory;

} // anonymous namespace
