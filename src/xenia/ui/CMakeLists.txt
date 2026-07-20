# wxWidgets is not available on Android.
# Therefore, xenia-ui is not built on Android.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
  add_library(xenia-ui STATIC)

  xe_platform_sources(
    xenia-ui
    ${CMAKE_CURRENT_SOURCE_DIR}
  )

  # Demo files are standalone executables in XENIA_BUILD_MISC.
  # windowed_app_main_* files are added explicitly per-target.
  # wxWidgets files are excluded from Android builds.
  file(GLOB _ui_exclude
    "${CMAKE_CURRENT_SOURCE_DIR}/*_demo.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/windowed_app_main_*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*_wx.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*_wx.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/wx_locale.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/wx_locale.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/icon_decode.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/icon_decode.h"
  )

  get_target_property(_ui_srcs xenia-ui SOURCES)

  if(_ui_exclude AND _ui_srcs)
    list(REMOVE_ITEM _ui_srcs ${_ui_exclude})

    set_target_properties(
      xenia-ui
      PROPERTIES
      SOURCES "${_ui_srcs}"
    )
  endif()

  target_link_libraries(
    xenia-ui
    PUBLIC
      xenia-base
  )

  if(WIN32)
    target_link_libraries(
      xenia-ui
      PRIVATE
        dxgi
    )

  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(
      xenia-ui
      PRIVATE
        xcb
        X11
        X11-xcb
        fontconfig
        ${GTK3_LIBRARIES}
    )

  elseif(APPLE)
    # xe_platform_sources only handles .cc files.
    target_sources(
      xenia-ui
      PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/surface_mac.mm
    )

    target_link_libraries(
      xenia-ui
      PRIVATE
        "-framework Cocoa"
        "-framework QuartzCore"
    )
  endif()

  xe_target_defaults(xenia-ui)

  xe_embed_binary_assets(
    xenia-ui
    "${PROJECT_SOURCE_DIR}/assets/font"
    font
  )

  xe_embed_binary_assets(
    xenia-ui
    "${PROJECT_SOURCE_DIR}/assets/icon"
    app_icon
  )
endif()


# Compile locale files and embed them into xenia-ui.
# This must also be disabled on Android because xenia-ui does not exist there.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(
    _xe_locale_stage
    "${CMAKE_CURRENT_BINARY_DIR}/locale_staged"
  )

  file(
    GLOB
    _xe_po_files
    "${PROJECT_SOURCE_DIR}/assets/locale/*/xenia.po"
  )

  foreach(_xe_po IN LISTS _xe_po_files)
    get_filename_component(
      _xe_po_dir
      "${_xe_po}"
      DIRECTORY
    )

    get_filename_component(
      _xe_lang
      "${_xe_po_dir}"
      NAME
    )

    set(
      _xe_mo
      "${_xe_locale_stage}/${_xe_lang}/xenia.mo"
    )

    file(
      MAKE_DIRECTORY
      "${_xe_locale_stage}/${_xe_lang}"
    )

    execute_process(
      COMMAND
        ${Python3_EXECUTABLE}
        "${PROJECT_SOURCE_DIR}/tools/build/compile_po.py"
        "${_xe_po}"
        "${_xe_mo}"

      RESULT_VARIABLE
        _xe_rc
    )

    if(NOT _xe_rc EQUAL 0)
      message(
        FATAL_ERROR
        "compile_po.py failed for ${_xe_po}"
      )
    endif()
  endforeach()

  if(TARGET xenia-ui)
    xe_embed_compressed_bundle(
      xenia-ui
      "${_xe_locale_stage}"
      locale
    )
  endif()
endif()
