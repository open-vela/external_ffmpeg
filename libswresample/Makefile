NAME = swresample
DESC = FFmpeg audio resampling library
FFLIBS = avutil

HEADERS = swresample.h                       \
          version.h                          \
          version_major.h                    \

OBJS = audioconvert.o                        \
       dither.o                              \
       options.o                             \
       rematrix.o                            \
       swresample.o                          \
       swresample_frame.o                    \
       version.o                             \

OBJS-$(CONFIG_SWR_RESAMPLER) += resample.o resample_dsp.o
OBJS-$(CONFIG_HIFI4SRC)      += hifi4_resample.o
OBJS-$(CONFIG_LIBSOXR)       += soxr_resample.o
OBJS-$(CONFIG_LIBSAMPLERATE) += src_resample.o
OBJS-$(CONFIG_SHARED)        += log2_tab.o

# Objects duplicated from other libraries for shared builds
SHLIBOBJS              += log2_tab.o

# Windows resource file
SLIBOBJS-$(HAVE_GNU_WINDRES) += swresampleres.o

TESTPROGS = swresample
