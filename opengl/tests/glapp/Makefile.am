ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -g \
              -DGL_GLEXT_PROTOTYPES \
              -DEGL_EGLEXT_PROTOTYPES

bin_PROGRAMS = glapp
glapp_CC = @CC@
pkgconfigdir = $(libdir)/pkgconfig
glapp_SOURCES = gl2_basic.cpp
glapp_LDADD = -lpthread -lEGL_adreno -lGLESv2_adreno
