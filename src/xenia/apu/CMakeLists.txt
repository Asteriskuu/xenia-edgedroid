add_library(xenia-apu STATIC)
xe_platform_sources(xenia-apu ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(xenia-apu PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/FFmpeg
  ${PROJECT_SOURCE_DIR}/third_party/ffmpeg-xenia
)
target_link_libraries(xenia-apu PUBLIC libavcodec libavutil libavformat xenia-base)
# AudioMediaPlayer instantiates SDLAudioDriver directly on Linux, so the
# SDL chain is a transitive dependency of xenia-apu there.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(xenia-apu PUBLIC xenia-helper-sdl SDL3)
endif()
xe_target_defaults(xenia-apu)
