##########################################################################
# This is an EPICS Makefile for StreamDevice.
# Normally it should not be necessary to modify this file.
# All configuration can be done in CONFIG_STREAM
#
# (C) 2007,2018 Dirk Zimoch (dirk.zimoch@psi.ch)
#
# This file is part of StreamDevice.
#
# StreamDevice is free software: You can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# StreamDevice is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with StreamDevice. If not, see https://www.gnu.org/licenses/.
#########################################################################/

TOP = .
DIRS = configure
src_DEPEND_DIRS := $(DIRS)
include $(TOP)/configure/CONFIG

DIRS += pcre-7_5
DIRS += src
DIRS += streamApp
streamApp_DEPEND_DIRS = src

include $(CONFIG)/RULES_TOP

docs/stream.pdf: docs/*.html docs/*.css docs/*.png
	cd docs; makepdf

pdf: docs/stream.pdf
