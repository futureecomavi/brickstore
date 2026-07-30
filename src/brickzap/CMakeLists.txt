# Copyright (C) 2004-2026 Robert Griebl
# SPDX-License-Identifier: GPL-3.0-only

qt_add_library(brickzap_module STATIC
    core.h
    core.cpp
    global.h
    mapper.h
    mapper.cpp
    orders.h
    orders.cpp
    store.h
    store.cpp
)

target_link_libraries(brickzap_module PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    QCoro6::Core   # common/currency.h pulls in QCoro
    bricklink_module
)

target_link_libraries(${PROJECT_NAME} PRIVATE brickzap_module)
