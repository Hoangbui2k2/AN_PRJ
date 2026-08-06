# ESP32 LoRa-to-MQTT Gateway Project
# ESP-IDF CMake build file

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set project name
set(PROJECT_NAME "gateway_irr")

# Add the main component
project(${PROJECT_NAME})
