cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The user-facing firmware version: semantic versioning (MAJOR.MINOR.PATCH), bumped for every
# release. It is the esp_app_desc version, the 设备信息 "固件" row and the protocol `firmware`
# field. Keep it in step with docs/CHANGELOG.md and the v<version> tag.
set(PROJECT_VER "0.2.0")

project(FoloToy-AI-Passport)
