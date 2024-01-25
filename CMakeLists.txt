cmake_minimum_required(VERSION 3.0)

option(MPV_BUILD_EMULATOR "Build project for desktop to ease debugging" FALSE)
set(MPV_FFMPEG_LIBS_DIR "" CACHE STRING "Specfiy FFMPEG static library directory")

set(mpv_vita_title_id "MPV000001")
set(mpv_vita_name "mpv-vita")
set(mpv_vita_version "01.00")

# apply vitasdk toolchain setup
if(NOT MPV_BUILD_EMULATOR)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
    endif()
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "toolchain file")
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
endif()

set(mpv_src_files)
set(mpv_gen_files)
set(mpv_gen_dir)
set(mpv_inc_dirs)
set(mpv_link_libs)
set(mpv_compile_defs)
set(mpv_compile_flags)

list(APPEND mpv_inc_dirs
    ${CMAKE_CURRENT_SOURCE_DIR})

list(APPEND mpv_compile_flags
    -std=gnu11)

list(APPEND mpv_gen_dir
    ${CMAKE_CURRENT_BINARY_DIR}/generated_files)

include(FindPkgConfig)

function(_generate_source _out_var _abs_out_path _abs_script_path)
    # accumulate file paths from script arguments
    set(_dep_file_paths)
    list(APPEND _dep_file_paths ${_abs_script_path})
    foreach(_arg IN LISTS ARGN)
        set(_abs_arg_path ${_cwd}/${_arg})
        if(EXISTS ${_abs_arg_path})
            list(APPEND _dep_file_paths ${_abs_arg_path})
        endif()
    endforeach()

    get_filename_component(_abs_out_dir ${_abs_out_path} DIRECTORY)
    add_custom_command(OUTPUT ${_abs_out_path}
        DEPENDS ${_dep_file_paths}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_abs_out_dir}
        COMMAND ${_abs_script_path} ${ARGN} ${_abs_out_path})

    list(APPEND _out_result ${${_out_var}} ${_abs_out_path})
    set(${_out_var} ${_out_result} PARENT_SCOPE)
endfunction()

function(_add_pkg_deps _out_inc_dirs _out_link_libs)
    set(_lib_type)
    if(BUILD_VITA)
        set(_lib_type _STATIC)
    endif()

    set(_inc_result ${${_out_inc_dirs}})
    set(_libs_result ${${_out_link_libs}})
    foreach(_arg IN LISTS ARGN)
        list(APPEND _inc_result ${${_arg}${_lib_type}_INCLUDE_DIRS}})
        list(APPEND _libs_result ${${_arg}${_lib_type}_LIBRARIES})
    endforeach()
    set(${_out_inc_dirs} ${_inc_result} PARENT_SCOPE)
    set(${_out_link_libs} ${_libs_result} PARENT_SCOPE)
endfunction()

project(mpv-vita)

list(APPEND mpv_src_files
    # audio
    audio/out/internal.h

    audio/aframe.h
    audio/aframe.c
    audio/chmap.h
    audio/chmap.c
    audio/chmap_sel.h
    audio/chmap_sel.c
    audio/fmt-conversion.h
    audio/fmt-conversion.c
    audio/format.h
    audio/format.c
    audio/decode/ad_lavc.c
    audio/filter/af_drop.c
    audio/filter/af_scaletempo2.c
    audio/filter/af_scaletempo2_internals.h
    audio/filter/af_scaletempo2_internals.c
    audio/out/ao.h
    audio/out/ao.c
    audio/out/ao_null.c
    audio/out/buffer.c

    # core
    common/encode.h
    common/global.h
    common/msg_control.h

    common/av_common.h
    common/av_common.c
    common/av_log.h
    common/av_log.c
    common/codecs.h
    common/codecs.c
    common/common.h
    common/common.c
    common/encode_lavc.h
    common/encode_lavc.c
    common/msg.h
    common/msg.c
    common/playlist.h
    common/playlist.c
    common/recorder.h
    common/recorder.c
    common/stats.h
    common/stats.c
    common/tags.h
    common/tags.c
    common/version.c

    # demuxers
    demux/matroska.h
    demux/stheader.h

    demux/cue.h
    demux/cue.c
    demux/cache.h
    demux/cache.c
    demux/codec_tags.h
    demux/codec_tags.c
    demux/demux.h
    demux/demux.c
    demux/demux_cue.c
    demux/demux_disc.c
    demux/demux_edl.c
    demux/demux_lavf.c
    demux/demux_mf.c
    demux/demux_mkv.c
    demux/demux_mkv_timeline.c
    demux/demux_null.c
    demux/demux_playlist.c
    demux/demux_raw.c
    demux/demux_timeline.c
    demux/ebml.h
    demux/ebml.c
    demux/packet.h
    demux/packet.c
    demux/timeline.h
    demux/timeline.c

    # filters
    filters/filter_internal.h

    filters/f_async_queue.h
    filters/f_async_queue.c
    filters/f_autoconvert.h
    filters/f_autoconvert.c
    filters/f_auto_filters.h
    filters/f_auto_filters.c
    filters/f_decoder_wrapper.h
    filters/f_decoder_wrapper.c
    filters/f_demux_in.h
    filters/f_demux_in.c
    filters/f_output_chain.h
    filters/f_output_chain.c
    filters/f_swresample.h
    filters/f_swresample.c
    filters/f_swscale.h
    filters/f_swscale.c
    filters/f_utils.h
    filters/f_utils.c
    filters/filter.h
    filters/filter.c
    filters/frame.h
    filters/frame.c
    filters/user_filters.h
    filters/user_filters.c

    # input
    input/cmd.h
    input/cmd.c
    input/event.h
    input/event.c
    input/input.h
    input/input.c
    input/keycodes.h
    input/keycodes.c

    # misc
    misc/ctype.h
    misc/linked_list.h

    misc/bstr.h
    misc/bstr.c
    misc/charset_conv.h
    misc/charset_conv.c
    misc/dispatch.h
    misc/dispatch.c
    misc/json.h
    misc/json.c
    misc/natural_sort.h
    misc/natural_sort.c
    misc/node.h
    misc/node.c
    misc/rendezvous.h
    misc/rendezvous.c
    misc/thread_pool.h
    misc/thread_pool.c
    misc/thread_tools.h
    misc/thread_tools.c

    # options
    options/m_config.h

    options/m_config_core.h
    options/m_config_core.c
    options/m_config_frontend.h
    options/m_config_frontend.c
    options/m_option.h
    options/m_option.c
    options/m_property.h
    options/m_property.c
    options/options.h
    options/options.c
    options/parse_commandline.h
    options/parse_commandline.c
    options/parse_configfile.h
    options/parse_configfile.c
    options/path.h
    options/path.c

    # player
    player/core.h

    player/audio.c
    player/client.h
    player/client.c
    player/command.h
    player/command.c
    player/configfiles.c
    player/external_files.h
    player/external_files.c
    player/loadfile.c
    player/main.c
    player/misc.c
    player/osd.c
    player/playloop.c
    player/screenshot.h
    player/screenshot.c
    player/scripting.c
    player/sub.c
    player/video.c

    # streams
    stream/cookies.h
    stream/cookies.c
    stream/stream.h
    stream/stream.c
    stream/stream_avdevice.c
    stream/stream_cb.c
    stream/stream_concat.c
    stream/stream_edl.c
    stream/stream_file.c
    stream/stream_lavf.c
    stream/stream_memory.c
    stream/stream_mf.c
    stream/stream_null.c
    stream/stream_slice.c

    # subtitles
    sub/sd.h
    sub/osd_state.h

    sub/ass_mp.h
    sub/ass_mp.c
    sub/dec_sub.h
    sub/dec_sub.c
    sub/draw_bmp.h
    sub/draw_bmp.c
    sub/filter_sdh.c
    sub/img_convert.h
    sub/img_convert.c
    sub/lavc_conv.c
    sub/osd.h
    sub/osd.c
    sub/sd_ass.c
    sub/sd_lavc.c

    # video
    video/csputils.h
    video/csputils.c
    video/fmt-conversion.h
    video/fmt-conversion.c
    video/hwdec.h
    video/hwdec.c
    video/image_loader.h
    video/image_loader.c
    video/image_writer.h
    video/image_writer.c
    video/img_format.h
    video/img_format.c
    video/mp_image.h
    video/mp_image.c
    video/mp_image_pool.h
    video/mp_image_pool.c
    video/repack.h
    video/repack.c
    video/sws_utils.h
    video/sws_utils.c
    video/decode/vd_lavc.c
    video/out/aspect.h
    video/out/aspect.c
    video/out/bitmap_packer.h
    video/out/bitmap_packer.c
    video/out/dither.h
    video/out/dither.c
    video/out/dr_helper.h
    video/out/dr_helper.c
    video/out/filter_kernels.h
    video/out/filter_kernels.c
    video/out/vo.h
    video/out/vo.c
    video/out/vo_null.c
    video/out/win_state.h
    video/out/win_state.c

    # os dependent
    osdep/atomic.h
    osdep/compiler.h
    osdep/endian.h
    osdep/getpid.h
    osdep/main-fn.h
    osdep/path.h
    osdep/strnlen.h
    osdep/terminal.h

    osdep/io.h
    osdep/io.c
    osdep/subprocess.h
    osdep/subprocess.c
    osdep/threads.h
    osdep/threads.c
    osdep/timer.h
    osdep/timer.c

    # tree allocator
    ta/ta.h
    ta/ta.c
    ta/ta_talloc.h
    ta/ta_talloc.c
    ta/ta_utils.c

    # vita implementation
    sub/osd_dummy.c
    input/ipc-dummy.c
    osdep/timer-linux.c
    osdep/terminal-dummy.c
    osdep/subprocess-dummy.c

    audio/out/ao_vita.c
    video/out/vo_vita.c

    osdep/path-vita.c
    osdep/main-fn-vita.c
    osdep/vita/ui_context.h
    osdep/vita/ui_device.h
    osdep/vita/ui_driver.h
    osdep/vita/ui_panel.h
    osdep/vita/ui_panel_player.c
    osdep/vita/ui_panel_files.c
    osdep/vita/player_perf.h
    osdep/vita/player_perf.c
    osdep/vita/shape_draw.h
    osdep/vita/shape_draw.c
)

set(_emulator_flag 0)
if(MPV_BUILD_EMULATOR)
    set(_emulator_flag 1)
endif()

# config file content
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/config.h "\
#define CONFIGURATION           \"vita\"
#define HAVE_GPL                1
#define HAVE_VITA               1
#define HAVE_ZLIB               1
#define HAVE_STDATOMIC          1
#define HAVE_TA_LEAK_REPORT     ${_emulator_flag}
#define MPV_VITA_TITLE_ID       \"${mpv_vita_title_id}\"
")


# generate file
_generate_source(mpv_gen_files ${mpv_gen_dir}/generated/etc/input.conf.inc
    ${CMAKE_CURRENT_SOURCE_DIR}/TOOLS/file2string.py
    ${CMAKE_CURRENT_SOURCE_DIR}/etc/vita/input.conf)
_generate_source(mpv_gen_files ${mpv_gen_dir}/generated/version.h
    ${CMAKE_CURRENT_SOURCE_DIR}/version.py)
_generate_source(mpv_gen_files ${mpv_gen_dir}/generated/ebml_defs.inc
    ${CMAKE_CURRENT_SOURCE_DIR}/TOOLS/matroska.py
    --generate-definitions)
_generate_source(mpv_gen_files ${mpv_gen_dir}/generated/ebml_types.h
    ${CMAKE_CURRENT_SOURCE_DIR}/TOOLS/matroska.py
    --generate-header)
_generate_source(mpv_gen_files ${mpv_gen_dir}/config.h
    cmake -E copy_if_different
    ${CMAKE_CURRENT_BINARY_DIR}/config.h)


# ffmpeg depedencies
if(EXISTS ${MPV_FFMPEG_LIBS_DIR})
    list(APPEND mpv_inc_dirs
        ${MPV_FFMPEG_LIBS_DIR}/include)
    list(APPEND mpv_link_libs
        ${MPV_FFMPEG_LIBS_DIR}/lib/libavformat.a
        ${MPV_FFMPEG_LIBS_DIR}/lib/libswscale.a
        ${MPV_FFMPEG_LIBS_DIR}/lib/libavfilter.a
        ${MPV_FFMPEG_LIBS_DIR}/lib/libswresample.a
        ${MPV_FFMPEG_LIBS_DIR}/lib/libavcodec.a
        ${MPV_FFMPEG_LIBS_DIR}/lib/libavutil.a
        SceAudiodec_stub
        SceVideodec_stub
        SceCodecEngine_stub
        m
        z)
else()
    pkg_check_modules(_pkg_ffmpeg REQUIRED
        libavutil>=56.12.100
        libavcodec>=58.12.100
        libavformat>=58.9.100
        libswscale>=5.0.101
        libavfilter>=7.14.100
        libswresample>=3.0.100)
    _add_pkg_deps(mpv_inc_dirs mpv_link_libs _pkg_ffmpeg)
endif()


# libass
pkg_check_modules(_pkg_ass REQUIRED libass>=0.12.2)
_add_pkg_deps(mpv_inc_dirs mpv_link_libs _pkg_ass)

# zlib
pkg_check_modules(_pkg_zlib REQUIRED zlib)
_add_pkg_deps(mpv_inc_dirs mpv_link_libs _pkg_zlib)

if(BUILD_VITA)
    list(APPEND mpv_src_files
        osdep/vita/vita_audio.c
        osdep/vita/vita_render.c
        osdep/vita/vita_platform.c)

    list(APPEND mpv_link_libs
        vita2d
        ScePgf_stub
        SceGxm_stub
        SceCtrl_stub
        SceAudio_stub
        ScePower_stub
        SceAppMgr_stub
        SceAppUtil_stub
        SceDisplay_stub
        SceSysmodule_stub
        SceCommonDialog_stub)

    list(APPEND mpv_compile_flags
        -fno-optimize-sibling-calls)
else()
    if(POLICY CMP0072)
        set(OpenGL_GL_PREFERENCE LEGACY)
    endif()

    find_package(OpenAL REQUIRED)
    find_package(OpenGL REQUIRED)
    find_package(Freetype REQUIRED)
    find_package(glfw3 REQUIRED)

    list(APPEND mpv_src_files
        osdep/vita/emulator.h
        osdep/vita/emulator_audio.c
        osdep/vita/emulator_render.c
        osdep/vita/emulator_platform.c)

    list(APPEND mpv_inc_dirs
        ${OPENAL_INCLUDE_DIR}
        ${OPENGL_INCLUDE_DIR}
        ${FREETYPE_INCLUDE_DIRS})

    list(APPEND mpv_link_libs
        ${OPENAL_LIBRARY}
        ${OPENGL_LIBRARIES}
        ${FREETYPE_LIBRARIES}
        glfw)

    list(APPEND mpv_compile_defs
        _GNU_SOURCE
        GLFW_INCLUDE_ES2
        HAVE_GLIBC_THREAD_NAME=1)
endif()


add_executable(${PROJECT_NAME}
    ${mpv_src_files}
    ${mpv_gen_files})

target_compile_options(${PROJECT_NAME} PRIVATE
    ${mpv_compile_flags})

target_compile_definitions(${PROJECT_NAME} PRIVATE
    ${mpv_compile_defs})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${mpv_gen_dir}
    ${mpv_inc_dirs})

target_link_libraries(${PROJECT_NAME}
    ${mpv_link_libs}
    -pthread
    m)

if(BUILD_VITA)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
    vita_create_self(eboot.bin ${PROJECT_NAME})
    vita_create_vpk(${PROJECT_NAME}.vpk
        "${mpv_vita_title_id}" eboot.bin
        NAME "${mpv_vita_name}"
        VERSION "${mpv_vita_version}"
    )
endif()
