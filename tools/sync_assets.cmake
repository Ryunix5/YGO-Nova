# One-time asset sync for the build output directory.
#
# The assets tree holds ~9k card images; comparing or recopying it on every
# build added ~80s to each incremental build (the "copying assets then it
# hangs" symptom). The card DB and images are effectively static, so we copy
# them exactly once — when the output copy is missing — and skip otherwise.
#
# To force a refresh (e.g. after updating assets), delete the output assets
# folder or do a clean build.
#
# Invoked from CMakeLists.txt POST_BUILD with:
#   -DEDOPP_SRC=<source assets>  -DEDOPP_DST=<output assets>  -P sync_assets.cmake

if(EXISTS "${EDOPP_DST}/cards.cdb")
    message(STATUS "EdoPro+ assets already present — skipping sync "
                   "(delete '${EDOPP_DST}' to refresh).")
else()
    message(STATUS "EdoPro+ assets: first-time copy to output directory...")
    file(MAKE_DIRECTORY "${EDOPP_DST}")
    file(COPY "${EDOPP_SRC}/" DESTINATION "${EDOPP_DST}")
    message(STATUS "EdoPro+ assets: copy complete.")
endif()
