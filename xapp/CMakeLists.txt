cmake_minimum_required(VERSION 3.16)

project(advanced_handover_xapp)

set(CMAKE_C_STANDARD 11)

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/../../../../src)
include_directories(${CMAKE_SOURCE_DIR}/../../../../src/util)
include_directories(${CMAKE_SOURCE_DIR}/../../../../src/util/alg_ds)
include_directories(${CMAKE_SOURCE_DIR}/../../../../src/xApp)
include_directories(${CMAKE_SOURCE_DIR}/../../../../src/sm)
# Include per RRC messages
include_directories(${CMAKE_SOURCE_DIR}/../monitor/RRC_MESSAGES)

# Source files for compatible version (recommended)

set(UE_ID_COMMON_E2SM_SRCS
  ../../../../src/sm/rc_sm/ie/asn/AMFPointer.c
  ../../../../src/sm/rc_sm/ie/asn/AMFRegionID.c
  ../../../../src/sm/rc_sm/ie/asn/AMFSetID.c
  ../../../../src/sm/rc_sm/ie/asn/AMF-UE-NGAP-ID.c
  ../../../../src/sm/rc_sm/ie/asn/ENB-ID.c
  ../../../../src/sm/rc_sm/ie/asn/ENB-UE-X2AP-ID-Extension.c
  ../../../../src/sm/rc_sm/ie/asn/ENB-UE-X2AP-ID.c
  ../../../../src/sm/rc_sm/ie/asn/GlobalENB-ID.c
  ../../../../src/sm/rc_sm/ie/asn/GlobalGNB-ID.c
  ../../../../src/sm/rc_sm/ie/asn/GlobalNgENB-ID.c
  ../../../../src/sm/rc_sm/ie/asn/GlobalNGRANNodeID.c
  ../../../../src/sm/rc_sm/ie/asn/GNB-CU-CP-UE-E1AP-ID.c
  ../../../../src/sm/rc_sm/ie/asn/GNB-CU-UE-F1AP-ID.c
  ../../../../src/sm/rc_sm/ie/asn/GNB-ID.c
  ../../../../src/sm/rc_sm/ie/asn/GUAMI.c
  ../../../../src/sm/rc_sm/ie/asn/GUMMEI.c
  ../../../../src/sm/rc_sm/ie/asn/MME-Code.c
  ../../../../src/sm/rc_sm/ie/asn/MME-Group-ID.c
  ../../../../src/sm/rc_sm/ie/asn/MME-UE-S1AP-ID.c
  ../../../../src/sm/rc_sm/ie/asn/NGENB-CU-UE-W1AP-ID.c
  ../../../../src/sm/rc_sm/ie/asn/NgENB-ID.c
  ../../../../src/sm/rc_sm/ie/asn/NG-RANnodeUEXnAPID.c
  ../../../../src/sm/rc_sm/ie/asn/PLMNIdentity.c
  ../../../../src/sm/rc_sm/ie/asn/RANUEID.c
  ../../../../src/sm/rc_sm/ie/asn/UEID-ENB.c
  ../../../../src/sm/rc_sm/ie/asn/UEID-EN-GNB.c
  ../../../../src/sm/rc_sm/ie/asn/UEID-GNB-CU-CP-E1AP-ID-Item.c
  ../../../../src/sm/rc_sm/ie/asn/UEID-GNB-CU-CP-E1AP-ID-List.c
  ../../../../src/sm/rc_sm/ie/asn/UEID-GNB-CU-CP-F1AP-ID-Item.c
  ../../../../src/sm/rc_sm/ie/asn/UEID-GNB-CU-F1AP-ID-List.c
  ../../../../src/sm/rc_sm/ie/asn/UEID-GNB-CU-UP.c
  ../../../../src/sm/rc_sm/ie/asn/UEID-GNB-DU.c
  ../../../../src/sm/rc_sm/ie/asn/UEID-GNB.c
  ../../../../src/sm/rc_sm/ie/asn/UEID.c
  ../../../../src/sm/rc_sm/ie/asn/UEID-NG-ENB-DU.c
  ../../../../src/sm/rc_sm/ie/asn/UEID-NG-ENB.c
)

set(SOURCES_SIMPLE
    xapp_advanced_handover_simple.c
    ${UE_ID_COMMON_E2SM_SRCS}
)

set(SOURCES_TIMER
    xapp_advanced_handover_timer.c
    ${UE_ID_COMMON_E2SM_SRCS}
)

# Build xapp_advanced_handover_simple (metrics-based)
add_executable(xapp_advanced_handover_simple ${SOURCES_SIMPLE})

target_link_libraries(xapp_advanced_handover_simple
    PUBLIC
    asn1_nr_rrc
    3gpp_derived_ie_dec_asn_obj_rc
    e42_xapp
    -pthread
    -lsctp
    -ldl
    -lm
)

target_compile_options(xapp_advanced_handover_simple PRIVATE
    -Wall
    -Wextra
    -O2
    -g
)

install(TARGETS xapp_advanced_handover_simple
    RUNTIME DESTINATION bin
)

# Build xapp_advanced_handover_timer (timer-based - 7 seconds)
add_executable(xapp_advanced_handover_timer ${SOURCES_TIMER})

target_link_libraries(xapp_advanced_handover_timer
    PUBLIC
    asn1_nr_rrc
    3gpp_derived_ie_dec_asn_obj_rc
    e42_xapp
    -pthread
    -lsctp
    -ldl
    -lm
)

target_compile_options(xapp_advanced_handover_timer PRIVATE
    -Wall
    -Wextra
    -O2
    -g
)

install(TARGETS xapp_advanced_handover_timer
    RUNTIME DESTINATION bin
)

set(SOURCES_MULTIMETRIC
    xapp_ho_multimetric.c
    ${UE_ID_COMMON_E2SM_SRCS}
)

# Build xapp_ho_multimetric (multi-metric weighted scoring + anti-ping-pong + CSV)
add_executable(xapp_ho_multimetric ${SOURCES_MULTIMETRIC})

target_link_libraries(xapp_ho_multimetric
    PUBLIC
    asn1_nr_rrc
    3gpp_derived_ie_dec_asn_obj_rc
    e42_xapp
    -pthread
    -lsctp
    -ldl
    -lm
)

target_compile_options(xapp_ho_multimetric PRIVATE
    -Wall
    -Wextra
    -O2
    -g
)

install(TARGETS xapp_ho_multimetric
    RUNTIME DESTINATION bin
)

# Note: The other versions (basic, enhanced) have API compatibility issues
# and require modifications to work with the current FlexRIC version.
# The simple version uses metrics-based handover triggers.
# The timer version triggers handover after 7 seconds from first KPM report.
# The multimetric version uses weighted scoring (RSRP+SINR+CQI+Thp+PRB) with
# anti-ping-pong hysteresis and optional RC MeasReport for 3GPP-conformant RSRQ.