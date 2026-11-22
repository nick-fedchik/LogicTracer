# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Mykola Fedchyk

# Widgets live in a library so headless GUI tests can drive MainWindow directly.
add_library(logictracer_gui STATIC
    Builders.cpp Builders.hpp
    DiagnosticsDialog.cpp DiagnosticsDialog.hpp
    GuiDiagnostics.cpp GuiDiagnostics.hpp
    CaptureFields.hpp CaptureProfiles.cpp CaptureProfiles.hpp
    MainWindow.cpp MainWindow.hpp
    PicoPinout.cpp PicoPinout.hpp
    ProfileSelector.cpp ProfileSelector.hpp
    SerialPortPicker.cpp SerialPortPicker.hpp
    ViewTransform.hpp
    WaveformWidget.cpp WaveformWidget.hpp
)
add_library(LogicTracer::Gui ALIAS logictracer_gui)
target_link_libraries(logictracer_gui
    PUBLIC LogicTracer::Services Qt6::Widgets
    PRIVATE LogicTracer::Options Qt6::Concurrent)

qt_add_executable(logictracer-gui WIN32 MACOSX_BUNDLE main.cpp)
qt_add_resources(logictracer-gui "branding"
    PREFIX "/branding" BASE ${PROJECT_SOURCE_DIR}/packaging
    FILES ${PROJECT_SOURCE_DIR}/packaging/logictracer.png)
target_link_libraries(logictracer-gui PRIVATE LogicTracer::Gui LogicTracer::Options)

install(TARGETS logictracer-gui
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/logictracer-gui.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/logictracer.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
