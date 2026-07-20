if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
  add_library(xenia-ui STATIC)
  xe_platform_sources(xenia-ui ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Demo files are standalone executables in XENIA_BUILD_MISC; windowed_app_main_*
# files are added explicitly per-target. wx files are excluded on Android
# (no xe-wxWidgets target there).
file(GLOB _ui_exclude
  "${CMAKE_CURRENT_SOURCE_DIR}/*_demo.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/windowed_app_main_*.cc"
)

file(GLOB _wx_exclude
  "${CMAKE_CURRENT_SOURCE_DIR}/*_wx.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/*_wx.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/wx_locale.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/wx_locale.h"
)
list(APPEND _ui_exclude
  ${_wx_exclude}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  list(APPEND _ui_exclude
    "${CMAKE_CURRENT_SOURCE_DIR}/icon_decode.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/icon_decode.h"
  )
endif()

if(TARGET xenia-ui)
  get_target_property(_ui_srcs xenia-ui SOURCES)
  if(_ui_exclude AND _ui_srcs)
    list(REMOVE_ITEM _ui_srcs ${_ui_exclude})
    set_target_properties(xenia-ui PROPERTIES SOURCES "${_ui_srcs}")
  endif()

  target_link_libraries(xenia-ui PUBLIC xenia-base)

  if(WIN32)
    target_link_libraries(xenia-ui PRIVATE dxgi)
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(xenia-ui PRIVATE xcb X11 X11-xcb fontconfig ${GTK3_LIBRARIES})
  elseif(APPLE)
    target_sources(xenia-ui PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/surface_mac.mm
    )
    target_link_libraries(xenia-ui PRIVATE
      "-framework Cocoa"
      "-framework QuartzCore"
    )
  endif()

  xe_target_defaults(xenia-ui)

  xe_embed_binary_assets(xenia-ui
      "${PROJECT_SOURCE_DIR}/assets/font" font)
  xe_embed_binary_assets(xenia-ui
      "${PROJECT_SOURCE_DIR}/assets/icon" app_icon)
endif()
