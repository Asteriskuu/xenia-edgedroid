add_library(xenia-gpu-metal STATIC
  dxil_shader.cc
  dxil_shader.h
  ir_runtime_impl.cc
  metal_command_processor.cc
  metal_command_processor.h
  metal_dxil_binder.cc
  metal_dxil_binder.h
  metal_graphics_system.cc
  metal_graphics_system.h
  metal_heap_pool.cc
  metal_heap_pool.h
  metal_primitive_processor.cc
  metal_primitive_processor.h
  metal_render_target_cache.cc
  metal_render_target_cache.h
  metal_shader_cache.cc
  metal_shader_cache.h
  metal_shader_converter.cc
  metal_shader_converter.h
  metal_shared_memory.cc
  metal_shared_memory.h
  metal_texture_cache.cc
  metal_texture_cache.h
  msl_bindings.h
  msl_shader.cc
  msl_shader.h
  msl_tess_factor_kernels.h
)
target_include_directories(xenia-gpu-metal PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/glslang
)
target_link_libraries(xenia-gpu-metal PUBLIC
  xenia-base
  xenia-gpu
  xenia-ui-metal
  spirv-cross
  xenia-third-party-metal-shader-converter
  "-framework Foundation"
  "-framework Metal"
  "-framework MetalKit"
)
target_link_libraries(xenia-gpu-metal PRIVATE glslang-spirv)
xe_shader_rules_metal(xenia-gpu-metal ${CMAKE_CURRENT_SOURCE_DIR}/../shaders)
xe_shader_rules_slang(xenia-gpu-metal ${CMAKE_CURRENT_SOURCE_DIR}/../shaders
                      TARGET msl)
xe_target_defaults(xenia-gpu-metal)
