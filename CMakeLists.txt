project(rime-ids-reverse)
cmake_minimum_required(VERSION 3.10)

aux_source_directory(src ids_reverse_src)

add_library(rime-ids-reverse-objs OBJECT ${ids_reverse_src})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-ids-reverse-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-ids-reverse PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-ids-reverse-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} PARENT_SCOPE)
set(plugin_modules "ids_reverse" PARENT_SCOPE)

# Lua 5.5 版本随插件发布。安装到 Rime 的共享数据目录下，供
# lua_processor@*ids_reverse*processor 等组件通过 require 加载。
if(DEFINED RIME_DATA_DIR)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/ids_reverse.lua
    DESTINATION ${CMAKE_INSTALL_FULL_DATADIR}/${RIME_DATA_DIR}/lua)
endif()
