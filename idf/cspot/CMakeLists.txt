# ESP-IDF component wrapper around the cspot library.
#
# Lets an ESP-IDF (or ESPHome esp-idf framework) project consume cspot + bell as a
# regular IDF component via a local-path dependency in idf_component.yml:
#
#   dependencies:
#     cspot: { path: /path/to/cspot/esp32-component }
#
# Protobuf sources are pre-generated (see ../proto_generated), so the build needs no
# protoc / Python protobuf tooling.

idf_component_register(
    SRCS "component.c"
    REQUIRES mbedtls pthread driver lwip nvs_flash espressif__mdns
)

# Spotify streams are Ogg Vorbis only — everything else is dead weight on the S3.
set(BELL_DISABLE_SINKS ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
set(BELL_CODEC_VORBIS ON CACHE BOOL "" FORCE)
set(BELL_CODEC_AAC OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_MP3 OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_ALAC OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_OPUS OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../cspot" cspot_build)

target_link_libraries(${COMPONENT_LIB} PUBLIC cspot)
