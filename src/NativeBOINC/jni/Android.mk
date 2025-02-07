#
# NativeBOINC - Native BOINC Client with Manager
# Copyright (C) 2011, Mateusz Szpakowski
# 
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := nativeboinc_utils
LOCAL_SRC_FILES := chmod.c putils.c execperms.c bugcatch.c

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_MODULE    := execwrapper
LOCAL_SRC_FILES := libexecwrapper.so
LOCAL_PREBUILTS := libexecwrapper.so
include $(PREBUILT_SHARED_LIBRARY)