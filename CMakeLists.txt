cmake_minimum_required(VERSION 3.21)
project(NotionRecorder VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# macOS deployment target: ScreenCaptureKit requires macOS 12.3+
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS deployment target")

find_package(Qt6 6.4 REQUIRED COMPONENTS Quick Qml)
qt_policy(SET QTP0004 NEW)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample
)

qt_add_executable(NotionRecorder
    src/main.cpp
    src/audio/Denoiser.cpp
    src/audio/Denoiser.h
    src/ui/AppController.cpp
    src/ui/AppController.h
    src/core/Config.cpp
    src/core/Config.h
    src/core/RecordingSession.cpp
    src/core/RecordingSession.h
    src/core/ThreadSafeQueue.h
    src/core/TimeBase.h
    src/capture/ScreenCapturer.h
    src/capture/AudioCapturer.h
    src/capture/macos/ScreenCaptureKitCapturer.mm
    src/capture/macos/ScreenCaptureKitCapturer.h
    src/capture/macos/CoreAudioCapturer.mm
    src/capture/macos/CoreAudioCapturer.h
    src/capture/macos/CoreAudioTapCapturer.mm
    src/capture/macos/CoreAudioTapCapturer.h
    src/codec/VideoEncoder.cpp
    src/codec/VideoEncoder.h
    src/codec/AudioEncoder.cpp
    src/codec/AudioEncoder.h
    src/codec/EncoderFactory.cpp
    src/codec/EncoderFactory.h
    src/mux/Muxer.cpp
    src/mux/Muxer.h
)

target_include_directories(NotionRecorder PRIVATE
    src
    ${FFMPEG_INCLUDE_DIRS}
)

target_compile_definitions(NotionRecorder PRIVATE
    ${FFMPEG_CFLAGS_OTHER}
    QT_NO_KEYWORDS
)

target_link_libraries(NotionRecorder PRIVATE
    Qt6::Quick
    Qt6::Qml
    PkgConfig::FFMPEG
    "-framework ScreenCaptureKit"
    "-framework CoreMedia"
    "-framework CoreVideo"
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework VideoToolbox"
    "-framework Foundation"
    "-framework AppKit"
    "-framework Carbon"
    "-framework Accelerate"
)

# Self-test: synthetic frames -> encode -> fragmented MP4 (no screen/mic access).
if(APPLE)
    add_executable(pipeline_test
        tests/pipeline_test.cpp
        src/codec/VideoEncoder.cpp
        src/codec/AudioEncoder.cpp
        src/mux/Muxer.cpp
    )
    target_include_directories(pipeline_test PRIVATE src ${FFMPEG_INCLUDE_DIRS})
    target_compile_definitions(pipeline_test PRIVATE ${FFMPEG_CFLAGS_OTHER} QT_NO_KEYWORDS)
    target_link_libraries(pipeline_test PRIVATE
        PkgConfig::FFMPEG
        "-framework CoreVideo"
        "-framework CoreMedia"
        "-framework Foundation"
    )
endif()

if(APPLE)
    # Denoiser self-test: verifies SNR improvement on synthetic noisy tone.
    add_executable(denoise_test
        tests/denoise_test.cpp
        src/audio/Denoiser.cpp
        src/audio/Denoiser.h
    )
    target_include_directories(denoise_test PRIVATE src)
    target_link_libraries(denoise_test PRIVATE "-framework Accelerate")

    # QML click smoke test: verifies floating-bar buttons react to clicks.
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test Quick Qml)
    add_executable(qml_click_test
        tests/qml_click_test.cpp
    )
    target_include_directories(qml_click_test PRIVATE src)
    # Reuse the QML module resources generated for the app so the test can
    # load the real Main.qml through the module (same as production).
    target_sources(qml_click_test PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/notionrecorder_qmltyperegistrations.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/.qt/rcc/qrc_NotionRecorder_raw_qml_0.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/.qt/rcc/qrc_NotionRecorder_raw_qml_0_extra_qmldirs.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/.qt/rcc/qrc_qmake_NotionRecorder.cpp"
    )
    target_link_libraries(qml_click_test PRIVATE
        Qt6::Test
        Qt6::Quick
        Qt6::Qml
    )
endif()

set_target_properties(NotionRecorder PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "NotionRecorder"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.notionrecorder.app"
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
)

qt_add_qml_module(NotionRecorder
    URI NotionRecorder
    VERSION 1.0
    QML_FILES
        src/ui/qml/Main.qml
        src/ui/qml/FloatingBar.qml
        src/ui/qml/SettingsDialog.qml
        src/ui/qml/RegionSelector.qml
        src/ui/qml/AnnotationOverlay.qml
        src/ui/qml/RecordingHistory.qml
)

# Deploy the QML module into the app bundle (qmldir + sources) and finalize
# macOS bundle metadata.
qt_finalize_executable(NotionRecorder)

set_source_files_properties(
    src/capture/macos/ScreenCaptureKitCapturer.mm
    src/capture/macos/CoreAudioCapturer.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
)

if(APPLE)
    # Deploy the generated QML module directory into the bundle so the app
    # runs from build/NotionRecorder.app without extra import paths.
    add_custom_command(TARGET NotionRecorder POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_BINARY_DIR}/NotionRecorder"
            "$<TARGET_BUNDLE_CONTENT_DIR:NotionRecorder>/Resources/qml/NotionRecorder"
        COMMENT "Deploying QML module into app bundle"
    )
    # Screen recording / microphone usage descriptions
    set_target_properties(NotionRecorder PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist"
    )
endif()
