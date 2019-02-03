find_package(PkgConfig REQUIRED)

foreach(lib avcodec avdevice avfilter avformat avutil swresample swscale)
  pkg_check_modules(FFMPEG_${lib} REQUIRED lib${lib})
endforeach()

# handle the QUIETLY and REQUIRED arguments and set xxx_FOUND to TRUE if all
# listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFMPEG
                                  DEFAULT_MSG
                                  FFMPEG_avcodec_INCLUDEDIR
                                  FFMPEG_avcodec_LIBRARIES
                                  FFMPEG_avdevice_INCLUDEDIR
                                  FFMPEG_avdevice_LIBRARIES
                                  FFMPEG_avfilter_INCLUDEDIR
                                  FFMPEG_avfilter_LIBRARIES
                                  FFMPEG_avformat_INCLUDEDIR
                                  FFMPEG_avformat_LIBRARIES
                                  FFMPEG_avutil_INCLUDEDIR
                                  FFMPEG_avutil_LIBRARIES
                                  FFMPEG_swresample_INCLUDEDIR
                                  FFMPEG_swresample_LIBRARIES
                                  FFMPEG_swscale_INCLUDEDIR
                                  FFMPEG_swscale_LIBRARIES)

if(FFMPEG_FOUND)
  set(FFMPEG_LIBRARIES
      ${FFMPEG_avcodec_LIBRARIES}
      ${FFMPEG_avdevice_LIBRARIES}
      ${FFMPEG_avfilter_LIBRARIES}
      ${FFMPEG_avformat_LIBRARIES}
      ${FFMPEG_avutil_LIBRARIES}
      ${FFMPEG_swresample_LIBRARIES}
      ${FFMPEG_swscale_LIBRARIES})
  set(FFMPEG_INCLUDE_DIRS
      ${FFMPEG_avcodec_INCLUDEDIR}
      ${FFMPEG_avdevice_INCLUDEDIR}
      ${FFMPEG_avfilter_INCLUDEDIR}
      ${FFMPEG_avformat_INCLUDEDIR}
      ${FFMPEG_avutil_INCLUDEDIR}
      ${FFMPEG_swresample_INCLUDEDIR}
      ${FFMPEG_swscale_INCLUDEDIR})
  set(FFMPEG_LDFLAGS
      ${FFMPEG_avcodec_LDFLAGS}
      ${FFMPEG_avdevice_LDFLAGS}
      ${FFMPEG_avfilter_LDFLAGS}
      ${FFMPEG_avformat_LDFLAGS}
      ${FFMPEG_avutil_LDFLAGS}
      ${FFMPEG_swresample_LDFLAGS}
      ${FFMPEG_swscale_LDFLAGS})
endif()
mark_as_advanced(FFMPEG_LIBRARIES FFMPEG_INCLUDEDIRS FFMPEG_LDFLAGS)

if(FFMPEG_FOUND)
  message("FFmpeg found")
endif()
