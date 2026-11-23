# SPDX-License-Identifier: AGPL-3.0-or-later
# Built into baresip by sip/build.sh (APP_MODULES_DIR=sip/modules).
project(t48mwi)

set(SRCS t48mwi.c)

if(STATIC)
    add_library(${PROJECT_NAME} OBJECT ${SRCS})
else()
    add_library(${PROJECT_NAME} MODULE ${SRCS})
endif()
