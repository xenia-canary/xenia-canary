/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/surface_gnulinux.h"

#include <X11/Xlib-xcb.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <cstdlib>
#include <cstring>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#include <wayland-client.h>
#endif

namespace xe {
namespace ui {

bool XcbWindowSurface::GetSizeImpl(uint32_t& width_out,
                                   uint32_t& height_out) const {
  xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(
      connection_, xcb_get_geometry(connection_, window_), nullptr);
  if (!reply) {
    return false;
  }
  width_out = reply->width;
  height_out = reply->height;
  std::free(reply);
  return true;
}

bool WaylandWindowSurface::GetSizeImpl(uint32_t& width_out,
                                       uint32_t& height_out) const {
  // Return the size provided during construction/updates
  width_out = width_;
  height_out = height_;
  return width_ > 0 && height_ > 0;
}

WaylandWindowSurface::~WaylandWindowSurface() {
  if (factory_) {
    factory_->set_wayland_surface(nullptr);
  }
}

GtkSurfaceFactory::~GtkSurfaceFactory() {
#ifdef GDK_WINDOWING_WAYLAND
  if (size_allocate_handler_id_ && size_allocate_widget_) {
    g_signal_handler_disconnect(static_cast<GtkWidget*>(size_allocate_widget_),
                                size_allocate_handler_id_);
  }
  if (wayland_surface_) {
    wayland_surface_->set_factory(nullptr);
    wayland_surface_ = nullptr;
  }
  if (wl_render_subsurface_) {
    wl_subsurface_destroy(wl_render_subsurface_);
  }
  if (wl_render_surface_) {
    wl_surface_destroy(wl_render_surface_);
  }
#endif
}

std::unique_ptr<Surface> GtkSurfaceFactory::Create(
    void* gtk_widget_ptr, Surface::TypeFlags allowed_types) {
  GtkWidget* widget = static_cast<GtkWidget*>(gtk_widget_ptr);
  if (!widget) {
    return nullptr;
  }
  gtk_widget_realize(widget);
  GdkWindow* gdk_window = gtk_widget_get_window(widget);
  if (!gdk_window) {
    return nullptr;
  }
  GdkDisplay* display = gdk_window_get_display(gdk_window);

#ifdef GDK_WINDOWING_WAYLAND
  if (GDK_IS_WAYLAND_DISPLAY(display) &&
      (allowed_types & Surface::kTypeFlag_WaylandWindow)) {
    wl_display* wl_dpy = gdk_wayland_display_get_wl_display(display);
    GtkWidget* toplevel_widget = gtk_widget_get_toplevel(widget);
    GdkWindow* parent_gdk = gtk_widget_get_window(toplevel_widget);
    wl_surface* parent_surf =
        parent_gdk ? gdk_wayland_window_get_wl_surface(parent_gdk) : nullptr;
    if (!wl_dpy || !parent_surf) {
      return nullptr;
    }

    struct RegistryState {
      wl_compositor* compositor = nullptr;
      wl_subcompositor* subcompositor = nullptr;
    } registry_state;
    static const wl_registry_listener registry_listener = {
        [](void* data, wl_registry* registry, uint32_t id, const char* iface,
           uint32_t /*version*/) {
          auto* s = static_cast<RegistryState*>(data);
          if (std::strcmp(iface, "wl_compositor") == 0) {
            s->compositor = static_cast<wl_compositor*>(
                wl_registry_bind(registry, id, &wl_compositor_interface, 4u));
          } else if (std::strcmp(iface, "wl_subcompositor") == 0) {
            s->subcompositor = static_cast<wl_subcompositor*>(
                wl_registry_bind(registry, id, &wl_subcompositor_interface, 1));
          }
        },
        [](void*, wl_registry*, uint32_t) {}};
    wl_registry* registry = wl_display_get_registry(wl_dpy);
    wl_registry_add_listener(registry, &registry_listener, &registry_state);
    wl_display_roundtrip(wl_dpy);
    wl_registry_destroy(registry);
    if (!registry_state.compositor || !registry_state.subcompositor) {
      if (registry_state.compositor) {
        wl_compositor_destroy(registry_state.compositor);
      }
      if (registry_state.subcompositor) {
        wl_subcompositor_destroy(registry_state.subcompositor);
      }
      return nullptr;
    }

    wl_surface* child_surf =
        wl_compositor_create_surface(registry_state.compositor);
    // Empty input region so pointer/touch fall through to the parent (where
    // wxGTK's input handlers live).
    wl_region* empty_region =
        wl_compositor_create_region(registry_state.compositor);
    wl_surface_set_input_region(child_surf, empty_region);
    wl_region_destroy(empty_region);

    wl_subsurface* sub = wl_subcompositor_get_subsurface(
        registry_state.subcompositor, child_surf, parent_surf);
    wl_subsurface_set_desync(sub);
    wl_compositor_destroy(registry_state.compositor);
    wl_subcompositor_destroy(registry_state.subcompositor);
    int sub_x = 0, sub_y = 0;
    gtk_widget_translate_coordinates(widget, toplevel_widget, 0, 0, &sub_x,
                                     &sub_y);
    wl_subsurface_set_position(sub, sub_x, sub_y);
    wl_surface_commit(parent_surf);

    wl_render_surface_ = child_surf;
    wl_render_subsurface_ = sub;
    size_allocate_handler_id_ = g_signal_connect(
        widget, "size-allocate",
        G_CALLBACK(+[](GtkWidget* w, GdkRectangle* /*alloc*/, gpointer data) {
          static_cast<GtkSurfaceFactory*>(data)->OnResize(w);
        }),
        this);
    size_allocate_widget_ = widget;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    int scale = gdk_window_get_scale_factor(parent_gdk);
    auto surface = std::make_unique<WaylandWindowSurface>(
        wl_dpy, child_surf, uint32_t(alloc.width * scale),
        uint32_t(alloc.height * scale));
    wayland_surface_ = surface.get();
    surface->set_factory(this);
    return surface;
  }
#endif

  if (GDK_IS_X11_DISPLAY(display) &&
      (allowed_types & Surface::kTypeFlag_XcbWindow)) {
    xcb_connection_t* connection =
        XGetXCBConnection(gdk_x11_display_get_xdisplay(display));
    xcb_window_t window = gdk_x11_window_get_xid(gdk_window);
    return std::make_unique<XcbWindowSurface>(connection, window);
  }
  return nullptr;
}

void GtkSurfaceFactory::OnResize(void* gtk_widget_ptr) {
#ifdef GDK_WINDOWING_WAYLAND
  if (!wl_render_subsurface_) {
    return;
  }
  GtkWidget* widget = static_cast<GtkWidget*>(gtk_widget_ptr);
  if (!widget) {
    return;
  }
  GtkWidget* toplevel = gtk_widget_get_toplevel(widget);
  int sub_x = 0, sub_y = 0;
  gtk_widget_translate_coordinates(widget, toplevel, 0, 0, &sub_x, &sub_y);
  wl_subsurface_set_position(wl_render_subsurface_, sub_x, sub_y);
  GdkWindow* parent_gdk = gtk_widget_get_window(toplevel);
  if (wayland_surface_) {
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    int scale = parent_gdk ? gdk_window_get_scale_factor(parent_gdk) : 1;
    wayland_surface_->SetSize(uint32_t(alloc.width * scale),
                              uint32_t(alloc.height * scale));
  }
  // wl_subsurface position is double-buffered: must commit the parent for
  // the new position to take effect.
  if (parent_gdk) {
    if (wl_surface* parent_surf =
            gdk_wayland_window_get_wl_surface(parent_gdk)) {
      wl_surface_commit(parent_surf);
    }
  }
#else
  (void)gtk_widget_ptr;
#endif
}

}  // namespace ui
}  // namespace xe
